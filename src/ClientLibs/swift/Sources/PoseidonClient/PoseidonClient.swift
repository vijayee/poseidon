import Foundation

/// Swift async/await API for Poseidon pub/sub.
@available(macOS 10.15, iOS 13.0, *)
class PoseidonClient {
    private let connection: PoseidonConnection

    init(connection: PoseidonConnection) {
        self.connection = connection
    }

    func createChannel(name: String) async throws -> String {
        let response = try await connection.sendRequest(
            method: PoseidonConnection.methodChannelCreate,
            topicPath: name)
        return String(data: response, encoding: .utf8) ?? ""
    }

    func joinChannel(topicOrAlias: String) async throws -> String {
        let response = try await connection.sendRequest(
            method: PoseidonConnection.methodChannelJoin,
            topicPath: topicOrAlias)
        return String(data: response, encoding: .utf8) ?? ""
    }

    func leaveChannel(topicId: String) async throws {
        _ = try await connection.sendRequest(
            method: PoseidonConnection.methodChannelLeave,
            topicPath: topicId)
    }

    func destroyChannel(topicId: String, ownerKey: Data) async throws {
        _ = try await connection.sendRequest(
            method: PoseidonConnection.methodChannelDestroy,
            topicPath: topicId,
            payload: ownerKey)
    }

    func modifyChannel(topicId: String, config: [String: Any], ownerKey: Data) async throws {
        _ = try await connection.sendRequest(
            method: PoseidonConnection.methodChannelModify,
            topicPath: topicId,
            payload: ownerKey)
    }

    func subscribe(topicPath: String, loopback: Bool = false) async throws {
        let payload: Data? = loopback ? Data([0x01]) : nil
        _ = try await connection.sendRequest(
            method: PoseidonConnection.methodSubscribe,
            topicPath: topicPath,
            payload: payload)
    }

    func unsubscribe(topicPath: String) async throws {
        _ = try await connection.sendRequest(
            method: PoseidonConnection.methodUnsubscribe,
            topicPath: topicPath)
    }

    func publish(topicPath: String, data: Data) async throws {
        _ = try await connection.sendRequest(
            method: PoseidonConnection.methodPublish,
            topicPath: topicPath,
            payload: data)
    }

    func registerAlias(name: String, topicId: String) async throws {
        _ = try await connection.sendRequest(
            method: PoseidonConnection.methodAliasRegister,
            topicPath: name,
            payload: topicId.data(using: .utf8))
    }

    func unregisterAlias(name: String) async throws {
        _ = try await connection.sendRequest(
            method: PoseidonConnection.methodAliasUnregister,
            topicPath: name)
    }

    func resolveAlias(name: String) async throws -> String {
        let response = try await connection.sendRequest(
            method: PoseidonConnection.methodAliasResolve,
            topicPath: name)
        return String(data: response, encoding: .utf8) ?? ""
    }

    func onMessage(_ callback: @escaping (String, String, Data) -> Void) {
        connection.onMessage(callback)
    }

    func onEvent(_ callback: @escaping (UInt8, Data) -> Void) {
        connection.onEvent(callback)
    }
}