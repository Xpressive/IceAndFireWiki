//
//  ViewController.swift
//  IceAndFire
//
//  Created by Alexey Kuznetsov on 04.07.17.
//  Copyright © 2017 Alexey Kuznetsov. All rights reserved.
//

import UIKit

class BooksTableViewController: UITableViewController {

    var iAFBooksRepository: IceAndFireBooksRepository!
    
    private var books: [Book] = []
    
    override func viewDidLoad() {
        super.viewDidLoad()
        
        iAFBooksRepository.getAllBooks { (books) in
            if let iAFbooks = books {
                self.books = iAFbooks
                DispatchQueue.main.async {
                    self.tableView.reloadData()
                }
            } else {
                print("Err")
            }
        }
    }
    
    
    @IBAction func addButtonPressed(_ sender: Any) {
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = self.tableView.dequeueReusableCell(withIdentifier: "Cell", for: indexPath)
        cell.textLabel?.text = books[indexPath.row].name
        return cell
    }
    
    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        return books.count
    }
    
    override func numberOfSections(in tableView: UITableView) -> Int {
        return 1
    }
    
    override func prepare(for segue: UIStoryboardSegue, sender: Any?) {
        if segue.identifier == "showDetail" {
            (segue.destination as! DetailCollectionViewController).country = self.books[0].country
        }
    }

}

