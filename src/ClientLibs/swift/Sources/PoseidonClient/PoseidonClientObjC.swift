import Foundation

/// Objective-C compatible wrappers for PoseidonClient.
/// Uses completion handlers instead of async/await for ObjC interop.
@objcMembers
class PoseidonClientObjC: NSObject {
    private let client: PoseidonClient

    init(client: PoseidonClient) {
        self.client = client
    }

    func createChannel(name: String, completion: @escaping (String?, Error?) -> Void) {
        Task {
            do {
                let result = try await client.createChannel(name: name)
                completion(result, nil)
            } catch {
                completion(nil, error)
            }
        }
    }

    func joinChannel(topicOrAlias: String, completion: @escaping (String?, Error?) -> Void) {
        Task {
            do {
                let result = try await client.joinChannel(topicOrAlias: topicOrAlias)
                completion(result, nil)
            } catch {
                completion(nil, error)
            }
        }
    }

    func leaveChannel(topicId: String, completion: @escaping (Error?) -> Void) {
        Task {
            do {
                try await client.leaveChannel(topicId: topicId)
                completion(nil)
            } catch {
                completion(error)
            }
        }
    }

    func subscribe(topicPath: String, loopback: Bool = false, completion: @escaping (Error?) -> Void) {
        Task {
            do {
                try await client.subscribe(topicPath: topicPath, loopback: loopback)
                completion(nil)
            } catch {
                completion(error)
            }
        }
    }

    func unsubscribe(topicPath: String, completion: @escaping (Error?) -> Void) {
        Task {
            do {
                try await client.unsubscribe(topicPath: topicPath)
                completion(nil)
            } catch {
                completion(error)
            }
        }
    }

    func publish(topicPath: String, data: Data, completion: @escaping (Error?) -> Void) {
        Task {
            do {
                try await client.publish(topicPath: topicPath, data: data)
                completion(nil)
            } catch {
                completion(error)
            }
        }
    }

    func resolveAlias(name: String, completion: @escaping (String?, Error?) -> Void) {
        Task {
            do {
                let result = try await client.resolveAlias(name: name)
                completion(result, nil)
            } catch {
                completion(nil, error)
            }
        }
    }
}