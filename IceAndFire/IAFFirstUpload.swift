//
//  IAFBooksFirstUpload.swift
//  IceAndFire
//
//  Created by Alexey Kuznetsov on 13.07.17.
//  Copyright © 2017 Alexey Kuznetsov. All rights reserved.
//

import Foundation

final class FirstUpload {
    
    let wasUploadedBefore: Bool
    var isFirstUpload: Bool {
        return !wasUploadedBefore
    }
    
    init(getWasUploadedBefore: () -> Bool,
         setWasUploadedBefore: (Bool) -> ()) {
        let wasUploadedBefore = getWasUploadedBefore()
        self.wasUploadedBefore = wasUploadedBefore
        if !wasUploadedBefore {
            setWasUploadedBefore(true)
        }
    }
    
    convenience init(userDefaults: UserDefaults, key: String) {
        self.init(getWasUploadedBefore: {userDefaults.bool(forKey: key)}, setWasUploadedBefore: {userDefaults.set($0, forKey: key)})
    }
    
}
