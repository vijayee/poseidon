import Foundation

/// Manages XPC connection to the Poseidon daemon.
/// Handles CBOR frame serialization/deserialization, request ID tracking,
/// and response matching.
@available(macOS 10.15, iOS 13.0, *)
class PoseidonConnection {
    private var connection: xpc_connection_t?
    private var requestId: UInt32 = 0
    private var pendingRequests: [UInt32: (Result<Data, Error>) -> Void] = [:]
    private let queue = DispatchQueue(label: "com.poseidon.connection", attributes: .concurrent)
    private var messageCallback: ((String, String, Data) -> Void)?
    private var eventCallback: ((UInt8, Data) -> Void)?

    // Method codes matching client_protocol.h
    static let methodChannelCreate: UInt8 = 1
    static let methodChannelJoin: UInt8 = 2
    static let methodChannelLeave: UInt8 = 3
    static let methodChannelDestroy: UInt8 = 4
    static let methodChannelModify: UInt8 = 5
    static let methodSubscribe: UInt8 = 6
    static let methodUnsubscribe: UInt8 = 7
    static let methodPublish: UInt8 = 8
    static let methodAliasRegister: UInt8 = 9
    static let methodAliasUnregister: UInt8 = 10

    // Frame types
    static let frameRequest: UInt8 = 0x01
    static let frameResponse: UInt8 = 0x02
    static let frameEvent: UInt8 = 0x03

    func connect(serviceName: String) {
        connection = xpc_connection_create_mach_service(serviceName, nil, 0)
        guard let conn = connection else { return }

        xpc_connection_set_event_handler(conn) { [weak self] event in
            self?.handleEvent(event)
        }
        xpc_connection_resume(conn)
    }

    func disconnect() {
        guard let conn = connection else { return }
        xpc_connection_cancel(conn)
        connection = nil
    }

    func sendRequest(method: UInt8, topicPath: String, payload: Data? = nil) async throws -> Data {
        let id = nextRequestId()
        let frame = encodeRequest(requestId: id, method: method, topicPath: topicPath, payload: payload)

        return try await withCheckedThrowingContinuation { continuation in
            queue.sync(flags: .barrier) {
                pendingRequests[id] = { result in
                    switch result {
                    case .success(let data):
                        continuation.resume(returning: data)
                    case .failure(let error):
                        continuation.resume(throwing: error)
                    }
                }
            }

            guard let conn = connection else {
                continuation.resume(throwing: PoseidonError.notConnected)
                return
            }

            let xpcMessage = xpc_dictionary_create(nil, nil, 0)
            xpc_dictionary_set_uint64(xpcMessage, "request_id", UInt64(id))
            xpc_dictionary_set_data(xpcMessage, "frame", frame, frame.count)
            xpc_connection_send_message(conn, xpcMessage)
        }
    }

    func onMessage(_ callback: @escaping (String, String, Data) -> Void) {
        messageCallback = callback
    }

    func onEvent(_ callback: @escaping (UInt8, Data) -> Void) {
        eventCallback = callback
    }

    // MARK: - Private

    private func nextRequestId() -> UInt32 {
        queue.sync(flags: .barrier) {
            let id = requestId
            requestId &+= 1
            return id
        }
    }

    private func handleEvent(_ event: xpc_object_t) {
        guard xpc_get_type(event) == XPC_TYPE_DICTIONARY else { return }

        let frameData = xpc_dictionary_get_data(event, "frame", nil)
        guard let frame = frameData else { return }

        // Decode CBOR frame and dispatch
        // Simplified — production would use a CBOR decoder
        let frameType = frame.pointee

        if frameType == Self.frameResponse {
            let requestId = UInt32(frame[1]) << 24 | UInt32(frame[2]) << 16 |
                           UInt32(frame[3]) << 8 | UInt32(frame[4])
            let resultData = Data(bytes: frame.advanced(by: 8), count: 0) // Simplified
            queue.sync(flags: .barrier) {
                pendingRequests.removeValue(forKey: requestId)?(.success(resultData))
            }
        } else if frameType == Self.frameEvent {
            let eventType = frame[1]
            let data = Data() // Simplified
            eventCallback?(eventType, data)
        }
    }

    private func encodeRequest(requestId: UInt32, method: UInt8, topicPath: String, payload: Data?) -> Data {
        // Simplified CBOR encoding — production would use a CBOR library
        var data = Data()
        data.append(Self.frameRequest)
        data.append(contentsOf: withUnsafeBytes(of: requestId.bigEndian) { Array($0) })
        data.append(method)
        let topicBytes = topicPath.data(using: .utf8) ?? Data()
        data.append(contentsOf: withUnsafeBytes(of: UInt16(topicBytes.count).bigEndian) { Array($0) })
        data.append(topicBytes)
        if let payload = payload {
            data.append(payload)
        }
        return data
    }
}

enum PoseidonError: Error {
    case notConnected
    case timeout
    case invalidResponse
}

private func withUnsafeBytes<T>(of value: T, body: (UnsafeRawBufferPointer) -> [UInt8]) -> [UInt8] {
    var mutableValue = value
    return withUnsafeBytes(of: &mutableValue) { body($0) }
}