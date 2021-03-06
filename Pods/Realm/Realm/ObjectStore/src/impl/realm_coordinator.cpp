////////////////////////////////////////////////////////////////////////////
//
// Copyright 2015 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#include "impl/realm_coordinator.hpp"

#include "impl/collection_notifier.hpp"
#include "impl/external_commit_helper.hpp"
#include "impl/transact_log_handler.hpp"
#include "impl/weak_realm_notifier.hpp"
#include "binding_context.hpp"
#include "object_schema.hpp"
#include "object_store.hpp"
#include "schema.hpp"

#if REALM_ENABLE_SYNC
#include "sync/sync_config.hpp"
#include "sync/sync_manager.hpp"
#include "sync/sync_session.hpp"
#endif

#include <realm/group_shared.hpp>
#include <realm/lang_bind_helper.hpp>
#include <realm/string_data.hpp>

#include <algorithm>
#include <unordered_map>

using namespace realm;
using namespace realm::_impl;

static auto& s_coordinator_mutex = *new std::mutex;
static auto& s_coordinators_per_path = *new std::unordered_map<std::string, std::weak_ptr<RealmCoordinator>>;

std::shared_ptr<RealmCoordinator> RealmCoordinator::get_coordinator(StringData path)
{
    std::lock_guard<std::mutex> lock(s_coordinator_mutex);

    auto& weak_coordinator = s_coordinators_per_path[path];
    if (auto coordinator = weak_coordinator.lock()) {
        return coordinator;
    }

    auto coordinator = std::make_shared<RealmCoordinator>();
    weak_coordinator = coordinator;
    return coordinator;
}

std::shared_ptr<RealmCoordinator> RealmCoordinator::get_coordinator(const Realm::Config& config)
{
    auto coordinator = get_coordinator(config.path);
    std::lock_guard<std::mutex> lock(coordinator->m_realm_mutex);
    coordinator->set_config(config);
    return coordinator;
}

std::shared_ptr<RealmCoordinator> RealmCoordinator::get_existing_coordinator(StringData path)
{
    std::lock_guard<std::mutex> lock(s_coordinator_mutex);
    auto it = s_coordinators_per_path.find(path);
    return it == s_coordinators_per_path.end() ? nullptr : it->second.lock();
}

void RealmCoordinator::create_sync_session()
{
#if REALM_ENABLE_SYNC
    if (m_sync_session)
        return;

    if (!m_config.encryption_key.empty() && !m_config.sync_config->realm_encryption_key) {
        throw std::logic_error("A realm encryption key was specified in Realm::Config but not in SyncConfig");
    } else if (m_config.sync_config->realm_encryption_key && m_config.encryption_key.empty()) {
        throw std::logic_error("A realm encryption key was specified in SyncConfig but not in Realm::Config");
    } else if (m_config.sync_config->realm_encryption_key &&
               !std::equal(m_config.sync_config->realm_encryption_key->begin(), m_config.sync_config->realm_encryption_key->end(),
                           m_config.encryption_key.begin(), m_config.encryption_key.end())) {
        throw std::logic_error("The realm encryption key specified in SyncConfig does not match the one in Realm::Config");
    }

    m_sync_session = SyncManager::shared().get_session(m_config.path, *m_config.sync_config);

    std::weak_ptr<RealmCoordinator> weak_self = shared_from_this();
    SyncSession::Internal::set_sync_transact_callback(*m_sync_session,
                                                      [weak_self](VersionID old_version, VersionID new_version) {
        if (auto self = weak_self.lock()) {
            if (self->m_transaction_callback)
                self->m_transaction_callback(old_version, new_version);
            if (self->m_notifier)
                self->m_notifier->notify_others();
        }
    });
    if (m_config.sync_config->error_handler) {
        SyncSession::Internal::set_error_handler(*m_sync_session, m_config.sync_config->error_handler);
    }
#endif
}

void RealmCoordinator::set_config(const Realm::Config& config)
{
    if (config.encryption_key.data() && config.encryption_key.size() != 64)
        throw InvalidEncryptionKeyException();
    if (config.schema_mode == SchemaMode::ReadOnly && config.sync_config)
        throw std::logic_error("Synchronized Realms cannot be opened in read-only mode");
    if (config.schema_mode == SchemaMode::Additive && config.migration_function)
        throw std::logic_error("Realms opened in Additive-only schema mode do not use a migration function");
    if (config.schema_mode == SchemaMode::ReadOnly && config.migration_function)
        throw std::logic_error("Realms opened in read-only mode do not use a migration function");
    if (config.schema && config.schema_version == ObjectStore::NotVersioned)
        throw std::logic_error("A schema version must be specified when the schema is specified");
    if (!config.realm_data.is_null() && (!config.read_only() || !config.in_memory))
        throw std::logic_error("In-memory realms initialized from memory buffers can only be opened in read-only mode");
    if (!config.realm_data.is_null() && !config.path.empty())
        throw std::logic_error("Specifying both memory buffer and path is invalid");
    if (!config.realm_data.is_null() && !config.encryption_key.empty())
        throw std::logic_error("Memory buffers do not support encryption");
    // ResetFile also won't use the migration function, but specifying one is
    // allowed to simplify temporarily switching modes during development

    bool no_existing_realm = std::all_of(begin(m_weak_realm_notifiers), end(m_weak_realm_notifiers),
                                         [](auto& notifier) { return notifier.expired(); });
    if (no_existing_realm) {
        m_config = config;
    }
    else {
        if (m_config.read_only() != config.read_only()) {
            throw MismatchedConfigException("Realm at path '%1' already opened with different read permissions.", config.path);
        }
        if (m_config.in_memory != config.in_memory) {
            throw MismatchedConfigException("Realm at path '%1' already opened with different inMemory settings.", config.path);
        }
        if (m_config.encryption_key != config.encryption_key) {
            throw MismatchedConfigException("Realm at path '%1' already opened with a different encryption key.", config.path);
        }
        if (m_config.schema_mode != config.schema_mode) {
            throw MismatchedConfigException("Realm at path '%1' already opened with a different schema mode.", config.path);
        }
        if (config.schema && m_schema_version != ObjectStore::NotVersioned && m_schema_version != config.schema_version) {
            throw MismatchedConfigException("Realm at path '%1' already opened with different schema version.", config.path);
        }

#if REALM_ENABLE_SYNC
        if (bool(m_config.sync_config) != bool(config.sync_config)) {
            throw MismatchedConfigException("Realm at path '%1' already opened with different sync configurations.", config.path);
        }

        if (config.sync_config) {
            if (m_config.sync_config->user != config.sync_config->user) {
                throw MismatchedConfigException("Realm at path '%1' already opened with different sync user.", config.path);
            }
            if (m_config.sync_config->realm_url != config.sync_config->realm_url) {
                throw MismatchedConfigException("Realm at path '%1' already opened with different sync server URL.", config.path);
            }
            if (m_config.sync_config->transformer != config.sync_config->transformer) {
                throw MismatchedConfigException("Realm at path '%1' already opened with different transformer.", config.path);
            }
            if (m_config.sync_config->realm_encryption_key != config.sync_config->realm_encryption_key) {
                throw MismatchedConfigException("Realm at path '%1' already opened with sync session encryption key.", config.path);
            }
        }
#endif

        // Realm::update_schema() handles complaining about schema mismatches
    }

#if REALM_ENABLE_SYNC
    if (config.sync_config) {
        create_sync_session();
    }
#endif
}

std::shared_ptr<Realm> RealmCoordinator::get_realm(Realm::Config config)
{
    // realm must be declared before lock so that the mutex is released before
    // we release the strong reference to realm, as Realm's destructor may want
    // to acquire the same lock
    std::shared_ptr<Realm> realm;
    std::unique_lock<std::mutex> lock(m_realm_mutex);

    set_config(config);

    auto schema = std::move(config.schema);
    auto migration_function = std::move(config.migration_function);
    config.schema = {};

    if (config.cache) {
        AnyExecutionContextID execution_context(config.execution_context);
        for (auto& cached_realm : m_weak_realm_notifiers) {
            if (!cached_realm.is_cached_for_execution_context(execution_context))
                continue;
            // can be null if we jumped in between ref count hitting zero and
            // unregister_realm() getting the lock
            if ((realm = cached_realm.realm())) {
                // If the file is uninitialized and was opened without a schema,
                // do the normal schema init
                if (realm->schema_version() == ObjectStore::NotVersioned)
                    break;

                // Otherwise if we have a realm schema it needs to be an exact
                // match (even having the same properties but in different
                // orders isn't good enough)
                if (schema && realm->schema() != *schema)
                    throw MismatchedConfigException("Realm at path '%1' already opened on current thread with different schema.", config.path);

                return realm;
            }
        }
    }

    if (!realm) {
        realm = Realm::make_shared_realm(std::move(config), shared_from_this());
        if (!config.read_only() && !m_notifier && config.automatic_change_notifications) {
            try {
                m_notifier = std::make_unique<ExternalCommitHelper>(*this);
            }
            catch (std::system_error const& ex) {
                throw RealmFileException(RealmFileException::Kind::AccessError, get_path(), ex.code().message(), "");
            }
        }
        m_weak_realm_notifiers.emplace_back(realm, m_config.cache);
    }

    if (schema) {
        lock.unlock();
        realm->update_schema(std::move(*schema), config.schema_version, std::move(migration_function));
    }

    return realm;
}

std::shared_ptr<Realm> RealmCoordinator::get_realm()
{
    return get_realm(m_config);
}

bool RealmCoordinator::get_cached_schema(Schema& schema, uint64_t& schema_version,
                                         uint64_t& transaction) const noexcept
{
    std::lock_guard<std::mutex> lock(m_schema_cache_mutex);
    if (!m_cached_schema)
        return false;
    schema = *m_cached_schema;
    schema_version = m_schema_version;
    transaction = m_schema_transaction_version_max;
    return true;
}

void RealmCoordinator::cache_schema(Schema const& new_schema, uint64_t new_schema_version,
                                    uint64_t transaction_version)
{
    std::lock_guard<std::mutex> lock(m_schema_cache_mutex);
    if (transaction_version < m_schema_transaction_version_max)
        return;
    if (new_schema.empty() || new_schema_version == ObjectStore::NotVersioned)
        return;

    m_cached_schema = new_schema;
    m_schema_version = new_schema_version;
    m_schema_transaction_version_min = transaction_version;
    m_schema_transaction_version_max = transaction_version;
}

void RealmCoordinator::clear_schema_cache_and_set_schema_version(uint64_t new_schema_version)
{
    std::lock_guard<std::mutex> lock(m_schema_cache_mutex);
    m_cached_schema = util::none;
    m_schema_version = new_schema_version;
}

void RealmCoordinator::advance_schema_cache(uint64_t previous, uint64_t next)
{
    std::lock_guard<std::mutex> lock(m_schema_cache_mutex);
    if (!m_cached_schema)
        return;
    REALM_ASSERT(previous <= m_schema_transaction_version_max);
    if (next < m_schema_transaction_version_min)
        return;
    m_schema_transaction_version_min = std::min(previous, m_schema_transaction_version_min);
    m_schema_transaction_version_max = std::max(next, m_schema_transaction_version_max);
}

RealmCoordinator::RealmCoordinator() = default;

RealmCoordinator::~RealmCoordinator()
{
    std::lock_guard<std::mutex> coordinator_lock(s_coordinator_mutex);
    for (auto it = s_coordinators_per_path.begin(); it != s_coordinators_per_path.end(); ) {
        if (it->second.expired()) {
            it = s_coordinators_per_path.erase(it);
        }
        else {
            ++it;
        }
    }
}

void RealmCoordinator::unregister_realm(Realm* realm)
{
    std::lock_guard<std::mutex> lock(m_realm_mutex);
    auto new_end = remove_if(begin(m_weak_realm_notifiers), end(m_weak_realm_notifiers),
                             [=](auto& notifier) { return notifier.expired() || notifier.is_for_realm(realm); });
    m_weak_realm_notifiers.erase(new_end, end(m_weak_realm_notifiers));
}

void RealmCoordinator::clear_cache()
{
    std::vector<WeakRealm> realms_to_close;
    {
        std::lock_guard<std::mutex> lock(s_coordinator_mutex);

        for (auto& weak_coordinator : s_coordinators_per_path) {
            auto coordinator = weak_coordinator.second.lock();
            if (!coordinator) {
                continue;
            }

            coordinator->m_notifier = nullptr;

            // Gather a list of all of the realms which will be removed
            for (auto& weak_realm_notifier : coordinator->m_weak_realm_notifiers) {
                if (auto realm = weak_realm_notifier.realm()) {
                    realms_to_close.push_back(realm);
                }
            }
        }

        s_coordinators_per_path.clear();
    }

    // Close all of the previously cached Realms. This can't be done while
    // s_coordinator_mutex is held as it may try to re-lock it.
    for (auto& weak_realm : realms_to_close) {
        if (auto realm = weak_realm.lock()) {
            realm->close();
        }
    }
}

void RealmCoordinator::clear_all_caches()
{
    std::vector<std::weak_ptr<RealmCoordinator>> to_clear;
    {
        std::lock_guard<std::mutex> lock(s_coordinator_mutex);
        for (auto iter : s_coordinators_per_path) {
            to_clear.push_back(iter.second);
        }
    }
    for (auto weak_coordinator : to_clear) {
        if (auto coordinator = weak_coordinator.lock()) {
            coordinator->clear_cache();
        }
    }
}

void RealmCoordinator::assert_no_open_realms() noexcept
{
#ifdef REALM_DEBUG
    std::lock_guard<std::mutex> lock(s_coordinator_mutex);
    REALM_ASSERT(s_coordinators_per_path.empty());
#endif
}

void RealmCoordinator::wake_up_notifier_worker()
{
    if (m_notifier) {
        // FIXME: this wakes up the notification workers for all processes and
        // not just us. This might be worth optimizing in the future.
        m_notifier->notify_others();
    }
}

void RealmCoordinator::commit_write(Realm& realm)
{
    REALM_ASSERT(!m_config.read_only());
    REALM_ASSERT(realm.is_in_transaction());

    {
        // Need to acquire this lock before committing or another process could
        // perform a write and notify us before we get the chance to set the
        // skip version
        std::lock_guard<std::mutex> l(m_notifier_mutex);

        transaction::commit(*Realm::Internal::get_shared_group(realm));

        // Don't need to check m_new_notifiers because those don't skip versions
        bool have_notifiers = std::any_of(m_notifiers.begin(), m_notifiers.end(),
                                          [&](auto&& notifier) { return notifier->is_for_realm(realm); });
        if (have_notifiers) {
            m_notifier_skip_version = Realm::Internal::get_shared_group(realm)->get_version_of_current_transaction();
        }
    }

#if REALM_ENABLE_SYNC
    // Realm could be closed in did_change. So send sync notification first before did_change.
    if (m_sync_session) {
        auto& sg = Realm::Internal::get_shared_group(realm);
        auto version = LangBindHelper::get_version_of_latest_snapshot(*sg);
        SyncSession::Internal::nonsync_transact_notify(*m_sync_session, version);
    }
#endif
    if (realm.m_binding_context) {
        realm.m_binding_context->did_change({}, {});
    }

    if (m_notifier) {
        m_notifier->notify_others();
    }
}

void RealmCoordinator::pin_version(VersionID versionid)
{
    REALM_ASSERT_DEBUG(!m_notifier_mutex.try_lock());
    if (m_async_error) {
        return;
    }

    if (!m_advancer_sg) {
        try {
            std::unique_ptr<Group> read_only_group;
            Realm::open_with_config(m_config, m_advancer_history, m_advancer_sg, read_only_group, nullptr);
            REALM_ASSERT(!read_only_group);
            m_advancer_sg->begin_read(versionid);
        }
        catch (...) {
            m_async_error = std::current_exception();
            m_advancer_sg = nullptr;
            m_advancer_history = nullptr;
        }
    }
    else if (m_new_notifiers.empty()) {
        // If this is the first notifier then we don't already have a read transaction
        REALM_ASSERT_3(m_advancer_sg->get_transact_stage(), ==, SharedGroup::transact_Ready);
        m_advancer_sg->begin_read(versionid);
    }
    else {
        REALM_ASSERT_3(m_advancer_sg->get_transact_stage(), ==, SharedGroup::transact_Reading);
        if (versionid < m_advancer_sg->get_version_of_current_transaction()) {
            // Ensure we're holding a readlock on the oldest version we have a
            // handover object for, as handover objects don't
            m_advancer_sg->end_read();
            m_advancer_sg->begin_read(versionid);
        }
    }
}

void RealmCoordinator::register_notifier(std::shared_ptr<CollectionNotifier> notifier)
{
    auto version = notifier->version();
    auto& self = Realm::Internal::get_coordinator(*notifier->get_realm());
    {
        std::lock_guard<std::mutex> lock(self.m_notifier_mutex);
        self.pin_version(version);
        self.m_new_notifiers.push_back(std::move(notifier));
    }
}

void RealmCoordinator::clean_up_dead_notifiers()
{
    auto swap_remove = [&](auto& container) {
        bool did_remove = false;
        for (size_t i = 0; i < container.size(); ++i) {
            if (container[i]->is_alive())
                continue;

            // Ensure the notifier is destroyed here even if there's lingering refs
            // to the async notifier elsewhere
            container[i]->release_data();

            if (container.size() > i + 1)
                container[i] = std::move(container.back());
            container.pop_back();
            --i;
            did_remove = true;
        }
        return did_remove;
    };

    if (swap_remove(m_notifiers)) {
        // Make sure we aren't holding on to read versions needlessly if there
        // are no notifiers left, but don't close them entirely as opening shared
        // groups is expensive
        if (m_notifiers.empty() && m_notifier_sg) {
            REALM_ASSERT_3(m_notifier_sg->get_transact_stage(), ==, SharedGroup::transact_Reading);
            m_notifier_sg->end_read();
            m_notifier_skip_version = {0, 0};
        }
    }
    if (swap_remove(m_new_notifiers) && m_advancer_sg) {
        REALM_ASSERT_3(m_advancer_sg->get_transact_stage(), ==, SharedGroup::transact_Reading);
        if (m_new_notifiers.empty()) {
            m_advancer_sg->end_read();
        }
    }
}

void RealmCoordinator::on_change()
{
    run_async_notifiers();

    std::lock_guard<std::mutex> lock(m_realm_mutex);
    for (auto& realm : m_weak_realm_notifiers) {
        realm.notify();
    }
}

namespace {
class IncrementalChangeInfo {
public:
    IncrementalChangeInfo(SharedGroup& sg,
                          std::vector<std::shared_ptr<_impl::CollectionNotifier>>& notifiers)
    : m_sg(sg)
    {
        if (notifiers.empty())
            return;

        auto cmp = [&](auto&& lft, auto&& rgt) {
            return lft->version() < rgt->version();
        };

        // Sort the notifiers by their source version so that we can pull them
        // all forward to the latest version in a single pass over the transaction log
        std::sort(notifiers.begin(), notifiers.end(), cmp);

        // Preallocate the required amount of space in the vector so that we can
        // safely give out pointers to within the vector
        size_t count = 1;
        for (auto it = notifiers.begin(), next = it + 1; next != notifiers.end(); ++it, ++next) {
            if (cmp(*it, *next))
                ++count;
        }
        m_info.reserve(count);
        m_info.resize(1);
        m_current = &m_info[0];
    }

    TransactionChangeInfo& current() const { return *m_current; }

    bool advance_incremental(VersionID version)
    {
        if (version != m_sg.get_version_of_current_transaction()) {
            transaction::advance(m_sg, *m_current, version);
            m_info.push_back({
                m_current->table_modifications_needed,
                m_current->table_moves_needed,
                std::move(m_current->lists)});
            m_current = &m_info.back();
            return true;
        }
        return false;
    }

    void advance_to_final(VersionID version)
    {
        if (!m_current) {
            transaction::advance(m_sg, nullptr, version);
            return;
        }

        transaction::advance(m_sg, *m_current, version);

        // We now need to combine the transaction change info objects so that all of
        // the notifiers see the complete set of changes from their first version to
        // the most recent one
        for (size_t i = m_info.size() - 1; i > 0; --i) {
            auto& cur = m_info[i];
            if (cur.tables.empty())
                continue;
            auto& prev = m_info[i - 1];
            if (prev.tables.empty()) {
                prev.tables = cur.tables;
                continue;
            }

            for (size_t j = 0; j < prev.tables.size() && j < cur.tables.size(); ++j) {
                prev.tables[j].merge(CollectionChangeBuilder{cur.tables[j]});
            }
            prev.tables.reserve(cur.tables.size());
            while (prev.tables.size() < cur.tables.size()) {
                prev.tables.push_back(cur.tables[prev.tables.size()]);
            }
        }

        // Copy the list change info if there are multiple LinkViews for the same LinkList
        auto id = [](auto const& list) { return std::tie(list.table_ndx, list.col_ndx, list.row_ndx); };
        for (size_t i = 1; i < m_current->lists.size(); ++i) {
            for (size_t j = i; j > 0; --j) {
                if (id(m_current->lists[i]) == id(m_current->lists[j - 1])) {
                    m_current->lists[j - 1].changes->merge(CollectionChangeBuilder{*m_current->lists[i].changes});
                }
            }
        }
    }

private:
    std::vector<TransactionChangeInfo> m_info;
    TransactionChangeInfo* m_current = nullptr;
    SharedGroup& m_sg;
};
} // anonymous namespace

void RealmCoordinator::run_async_notifiers()
{
    std::unique_lock<std::mutex> lock(m_notifier_mutex);

    clean_up_dead_notifiers();

    if (m_notifiers.empty() && m_new_notifiers.empty()) {
        return;
    }

    if (!m_async_error) {
        open_helper_shared_group();
    }

    if (m_async_error) {
        std::move(m_new_notifiers.begin(), m_new_notifiers.end(), std::back_inserter(m_notifiers));
        m_new_notifiers.clear();
        return;
    }

    VersionID version;

    // Advance all of the new notifiers to the most recent version, if any
    auto new_notifiers = std::move(m_new_notifiers);
    IncrementalChangeInfo new_notifier_change_info(*m_advancer_sg, new_notifiers);

    if (!new_notifiers.empty()) {
        REALM_ASSERT_3(m_advancer_sg->get_transact_stage(), ==, SharedGroup::transact_Reading);
        REALM_ASSERT_3(m_advancer_sg->get_version_of_current_transaction().version,
                       <=, new_notifiers.front()->version().version);

        // The advancer SG can be at an older version than the oldest new notifier
        // if a notifier was added and then removed before it ever got the chance
        // to run, as we don't move the pin forward when removing dead notifiers
        transaction::advance(*m_advancer_sg, nullptr, new_notifiers.front()->version());

        // Advance each of the new notifiers to the latest version, attaching them
        // to the SG at their handover version. This requires a unique
        // TransactionChangeInfo for each source version, so that things don't
        // see changes from before the version they were handed over from.
        // Each Info has all of the changes between that source version and the
        // next source version, and they'll be merged together later after
        // releasing the lock
        for (auto& notifier : new_notifiers) {
            new_notifier_change_info.advance_incremental(notifier->version());
            notifier->attach_to(*m_advancer_sg);
            notifier->add_required_change_info(new_notifier_change_info.current());
        }
        new_notifier_change_info.advance_to_final(VersionID{});

        for (auto& notifier : new_notifiers) {
            notifier->detach();
        }

        // We want to advance the non-new notifiers to the same version as the
        // new notifiers to avoid having to merge changes from any new
        // transaction that happen immediately after this into the new notifier
        // changes
        version = m_advancer_sg->get_version_of_current_transaction();
        m_advancer_sg->end_read();
    }
    else {
        // If we have no new notifiers we want to just advance to the latest
        // version, but we have to pick a "latest" version while holding the
        // notifier lock to avoid advancing over a transaction which should be
        // skipped
        m_advancer_sg->begin_read();
        version = m_advancer_sg->get_version_of_current_transaction();
        m_advancer_sg->end_read();
    }
    REALM_ASSERT_3(m_advancer_sg->get_transact_stage(), ==, SharedGroup::transact_Ready);

    auto skip_version = m_notifier_skip_version;
    m_notifier_skip_version = {0, 0};

    // Make a copy of the notifiers vector and then release the lock to avoid
    // blocking other threads trying to register or unregister notifiers while we run them
    auto notifiers = m_notifiers;
    m_notifiers.insert(m_notifiers.end(), new_notifiers.begin(), new_notifiers.end());
    lock.unlock();

    if (skip_version.version) {
        REALM_ASSERT(!notifiers.empty());
        REALM_ASSERT(version >= skip_version);
        IncrementalChangeInfo change_info(*m_notifier_sg, notifiers);
        for (auto& notifier : notifiers)
            notifier->add_required_change_info(change_info.current());
        change_info.advance_to_final(skip_version);

        for (auto& notifier : notifiers)
            notifier->run();

        lock.lock();
        for (auto& notifier : notifiers)
            notifier->prepare_handover();
        lock.unlock();
    }

    // Advance the non-new notifiers to the same version as we advanced the new
    // ones to (or the latest if there were no new ones)
    IncrementalChangeInfo change_info(*m_notifier_sg, notifiers);
    for (auto& notifier : notifiers) {
        notifier->add_required_change_info(change_info.current());
    }
    change_info.advance_to_final(version);

    // Attach the new notifiers to the main SG and move them to the main list
    for (auto& notifier : new_notifiers) {
        notifier->attach_to(*m_notifier_sg);
        notifier->run();
    }

    // Change info is now all ready, so the notifiers can now perform their
    // background work
    for (auto& notifier : notifiers) {
        notifier->run();
    }

    // Reacquire the lock while updating the fields that are actually read on
    // other threads
    lock.lock();
    for (auto& notifier : new_notifiers) {
        notifier->prepare_handover();
    }
    for (auto& notifier : notifiers) {
        notifier->prepare_handover();
    }
    clean_up_dead_notifiers();
    m_notifier_cv.notify_all();
}

void RealmCoordinator::open_helper_shared_group()
{
    if (!m_notifier_sg) {
        try {
            std::unique_ptr<Group> read_only_group;
            Realm::open_with_config(m_config, m_notifier_history, m_notifier_sg, read_only_group, nullptr);
            REALM_ASSERT(!read_only_group);
            m_notifier_sg->begin_read();
        }
        catch (...) {
            // Store the error to be passed to the async notifiers
            m_async_error = std::current_exception();
            m_notifier_sg = nullptr;
            m_notifier_history = nullptr;
        }
    }
    else if (m_notifiers.empty()) {
        m_notifier_sg->begin_read();
    }
}

void RealmCoordinator::advance_to_ready(Realm& realm)
{
    std::unique_lock<std::mutex> lock(m_notifier_mutex);
    _impl::NotifierPackage notifiers(m_async_error, notifiers_for_realm(realm), this);
    lock.unlock();
    notifiers.package_and_wait(util::none);

    auto& sg = Realm::Internal::get_shared_group(realm);
    if (notifiers) {
        auto version = notifiers.version();
        if (version) {
            auto current_version = sg->get_version_of_current_transaction();
            // Notifications are out of date, so just discard
            // This should only happen if begin_read() was used to change the
            // read version outside of our control
            if (*version < current_version)
                return;
            // While there is a newer version, notifications are for the current
            // version so just deliver them without advancing
            if (*version == current_version) {
                notifiers.deliver(*sg);
                notifiers.after_advance();
                return;
            }
        }
    }

    transaction::advance(sg, realm.m_binding_context.get(), notifiers);
}

std::vector<std::shared_ptr<_impl::CollectionNotifier>> RealmCoordinator::notifiers_for_realm(Realm& realm)
{
    std::vector<std::shared_ptr<_impl::CollectionNotifier>> ret;
    for (auto& notifier : m_new_notifiers) {
        if (notifier->is_for_realm(realm))
            ret.push_back(notifier);
    }
    for (auto& notifier : m_notifiers) {
        if (notifier->is_for_realm(realm))
            ret.push_back(notifier);
    }
    return ret;
}

bool RealmCoordinator::advance_to_latest(Realm& realm)
{
    using sgf = SharedGroupFriend;

    auto& sg = Realm::Internal::get_shared_group(realm);
    std::unique_lock<std::mutex> lock(m_notifier_mutex);
    _impl::NotifierPackage notifiers(m_async_error, notifiers_for_realm(realm), this);
    lock.unlock();
    notifiers.package_and_wait(sgf::get_version_of_latest_snapshot(*sg));

    auto version = sg->get_version_of_current_transaction();
    transaction::advance(sg, realm.m_binding_context.get(), notifiers);

    // Realm could be closed in the callbacks.
    if (realm.is_closed())
        return false;

    return version != sg->get_version_of_current_transaction();
}

void RealmCoordinator::promote_to_write(Realm& realm)
{
    REALM_ASSERT(!realm.is_in_transaction());

    std::unique_lock<std::mutex> lock(m_notifier_mutex);
    _impl::NotifierPackage notifiers(m_async_error, notifiers_for_realm(realm), this);
    lock.unlock();

    auto& sg = Realm::Internal::get_shared_group(realm);
    transaction::begin(sg, realm.m_binding_context.get(), notifiers);
}

void RealmCoordinator::process_available_async(Realm& realm)
{
    REALM_ASSERT(!realm.is_in_transaction());

    std::unique_lock<std::mutex> lock(m_notifier_mutex);
    auto notifiers = notifiers_for_realm(realm);
    if (notifiers.empty())
        return;

    if (auto error = m_async_error) {
        lock.unlock();
        for (auto& notifier : notifiers)
            notifier->deliver_error(m_async_error);
        return;
    }

    bool in_read = realm.is_in_read_transaction();
    auto& sg = Realm::Internal::get_shared_group(realm);
    auto version = sg->get_version_of_current_transaction();
    auto package = [&](auto& notifier) {
        return !(notifier->has_run() && (!in_read || notifier->version() == version) && notifier->package_for_delivery());
    };
    notifiers.erase(std::remove_if(begin(notifiers), end(notifiers), package), end(notifiers));
    lock.unlock();

    // no before advance because the Realm is already at the given version,
    // because we're either sending initial notifications or the write was
    // done on this Realm instance

    // Skip delivering if the Realm isn't in a read transaction
    if (in_read) {
        for (auto& notifier : notifiers)
            notifier->deliver(*sg);
    }

    // but still call the change callbacks
    for (auto& notifier : notifiers)
        notifier->after_advance();
}

void RealmCoordinator::set_transaction_callback(std::function<void(VersionID, VersionID)> fn)
{
    m_transaction_callback = std::move(fn);
}
