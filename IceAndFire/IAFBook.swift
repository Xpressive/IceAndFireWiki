//
//  IACBook.swift
//  IceAndFire
//
//  Created by Alexey Kuznetsov on 04.07.17.
//  Copyright © 2017 Alexey Kuznetsov. All rights reserved.
//

import Foundation
import RealmSwift
import ObjectMapper

protocol Book {
    var urlString: String {get}
    var name: String {get}
    var isbn: String {get}
    var numberOfPages: Int {get}
    var publisher: String {get}
    var country: String {get}
    var mediaType: String {get}
    var released: Date {get}
}

class IAFBook: Object, Book {
    
    dynamic var urlString = ""
    dynamic var name = ""
    dynamic var isbn = ""
    dynamic var numberOfPages = 0
    dynamic var publisher = ""
    dynamic var country = ""
    dynamic var mediaType = ""
    dynamic var released = Date()
    
    override static func primaryKey() -> String {
        return "isbn"
    }
    
    required convenience init?(map: Map) {
        self.init()
    }
}

extension IAFBook: Mappable {
    
    func mapping(map: Map) {
        urlString       <- map["url"]
        name            <- map["name"]
        isbn            <- map["isbn"]
        numberOfPages   <- map["numberOfPages"]
        publisher       <- map["publisher"]
        country         <- map["country"]
        mediaType       <- map["mediaType"]
        released        <- (map["released"], CustomDateFormatTransform(formatString: "yyyy-MM-dd'T'HH:mm:ss"))
    }

}
