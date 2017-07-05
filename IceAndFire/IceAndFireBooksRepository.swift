//
//  IceAndFireProvider.swift
//  IceAndFire
//
//  Created by Alexey Kuznetsov on 04.07.17.
//  Copyright © 2017 Alexey Kuznetsov. All rights reserved.
//

import Foundation
import Moya
import Moya_ObjectMapper
import RealmSwift

class IceAndFireBooksRepository {
    private let provider: MoyaProvider<IceAndFire>
    
    private let booksDatasource: BooksDataSource
    
    init(provider: MoyaProvider<IceAndFire>, datasource: BooksDataSource) {
        self.provider = provider
        self.booksDatasource = datasource
    }
    
    func getAllBooksFromNetwork(completion: @escaping ([Book]?) -> ()) {
        provider.request(.books) { [weak self] (result) in
            switch result {
            case .success(let response):
                do {
                    let books = try response.mapArray(IAFBook.self)
                    completion(books)
                    self?.booksDatasource.saveAll(items: books)
                } catch {
                    completion(nil)
                }
            case .failure(let error):
                print(error.localizedDescription)
                completion(nil)
                
            }
        }
        
    }
}
