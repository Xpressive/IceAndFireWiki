//
//  IAFBooksRealmDataSource.swift
//  IceAndFire
//
//  Created by Alexey Kuznetsov on 05.07.17.
//  Copyright © 2017 Alexey Kuznetsov. All rights reserved.
//

import Foundation
import RealmSwift

protocol BooksDataSource {
    typealias T = IAFBook
    
    func getAll() -> [T]
    
    func saveAll(items: [T])
    
}

class IAFBooksRealmDataSource: BooksDataSource {
    typealias T = IAFBook
    
    private let realm = try! Realm()
    
    func getAll() -> [T] {
        return realm.objects(T.self).map {$0}
    }
    
    func saveAll(items: [T]) {
        do {
            try realm.write {
                realm.add(items, update: true)
            }
        } catch {}
    }
}
