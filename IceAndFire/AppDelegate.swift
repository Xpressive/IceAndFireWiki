//
//  AppDelegate.swift
//  IceAndFire
//
//  Created by Alexey Kuznetsov on 04.07.17.
//  Copyright © 2017 Alexey Kuznetsov. All rights reserved.
//

import UIKit
import Moya

@UIApplicationMain
class AppDelegate: UIResponder, UIApplicationDelegate {

    var window: UIWindow?

    func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplicationLaunchOptionsKey: Any]?) -> Bool {
        
        let iAFProvider = MoyaProvider<IceAndFire>(plugins: [NetworkLoggerPlugin(verbose: true, responseDataFormatter: JSONResponseDataFormatter)])
        
        let iAFBooksRepository = IceAndFireBooksRepository(provider: iAFProvider, datasource: IAFBooksRealmDataSource())
        
        if let firstVC = window?.rootViewController as? ViewController {
            firstVC.iAFBooksRepository = iAFBooksRepository
        }
        
        return true
    }

}

