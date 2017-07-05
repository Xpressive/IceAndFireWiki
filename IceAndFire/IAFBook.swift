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

class RealmString : Object {
    dynamic var stringValue = ""
}

class IAFBook: Object, Mappable {
    
    dynamic var urlString = ""
    dynamic var name = ""
    dynamic var isbn = ""
    dynamic var numberOfPages = 0
    dynamic var publisher = ""
    dynamic var country = ""
    dynamic var mediaType = ""
    dynamic var released = Date()
    
    required convenience init?(map: Map) {
        self.init()
    }
    
    func mapping(map: Map) {
        urlString       <- map["url"]
        name            <- map["name"]
        isbn            <- map["isbn"]
        numberOfPages   <- map["numberOfPages"]
        publisher       <- map["publisher"]
        country         <- map["country"]
        mediaType       <- map["mediaType"]
        released        <- map["released"]
    }
    
}
