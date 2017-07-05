//
//  ViewController.swift
//  IceAndFire
//
//  Created by Alexey Kuznetsov on 04.07.17.
//  Copyright © 2017 Alexey Kuznetsov. All rights reserved.
//

import UIKit

class ViewController: UIViewController {

    var iAFBooksRepository: IceAndFireBooksRepository!
    
    override func viewDidLoad() {
        super.viewDidLoad()
        
        iAFBooksRepository.getAllBooksFromNetwork { (books) in
            if let books = books {
                print(books)
            } else {
                print("Err")
            }
        }
    }

}

