//
//  IceAndFireAPI.swift
//  IceAndFire
//
//  Created by Alexey Kuznetsov on 04.07.17.
//  Copyright © 2017 Alexey Kuznetsov. All rights reserved.
//

import Foundation
import Moya

func JSONResponseDataFormatter(_ data: Data) -> Data {
    do {
        let dataAsJSON = try JSONSerialization.jsonObject(with: data)
        let prettyData =  try JSONSerialization.data(withJSONObject: dataAsJSON, options: .prettyPrinted)
        return prettyData
    } catch {
        return data // fallback to original data if it can't be serialized.
    }
}

// MARK: - Provider support

private extension String {
    var urlEscaped: String {
        return self.addingPercentEncoding(withAllowedCharacters: .urlHostAllowed)!
    }
}

public enum IceAndFire {
    case books
}

extension IceAndFire: TargetType {
    public var baseURL: URL { return URL(string: "https://www.anapioficeandfire.com/api")! }
    public var path: String {
        switch self {
        case .books:
            return "/books"
        }
    }
    public var method: Moya.Method {
        return .get
    }
    public var parameters: [String: Any]? {
        return nil
    }
    public var parameterEncoding: ParameterEncoding {
        return URLEncoding.default
    }
    public var task: Task {
        return .request
    }
    public var validate: Bool {
        switch self {
        case .books:
            return true
        }
    }
    public var sampleData: Data {
        switch self {
        case .books:
            return "[{\"url\": \"bookUrl\"}]".data(using: String.Encoding.utf8)!
        }
    }
}
