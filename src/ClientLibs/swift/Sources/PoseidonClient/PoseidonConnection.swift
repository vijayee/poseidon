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
    private(set) var messageCallback: ((String, String, Data) -> Void)?
    private(set) var eventCallback: ((UInt8, Data) -> Void)?

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
    static let methodAliasResolve: UInt8 = 11

    // Frame types matching client_protocol.h
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

    // MARK: - Request ID

    private func nextRequestId() -> UInt32 {
        queue.sync(flags: .barrier) {
            let id = requestId
            requestId &+= 1
            return id
        }
    }

    /// Public wrapper for testing request ID generation.
    func nextRequestIdPublic() -> UInt32 {
        return nextRequestId()
    }

    // MARK: - Frame Encoding

    /// Encodes a request frame as a binary buffer.
    /// Format: [frame_type(1)][request_id(4)][method(1)][topic_len(2)][topic(N)][payload(M)]
    static func encodeRequest(requestId: UInt32, method: UInt8, topicPath: String, payload: Data?) -> Data {
        var data = Data()
        data.append(frameRequest)
        data.append(contentsOf: encodeBigEndian(requestId))
        data.append(method)
        let topicBytes = topicPath.data(using: .utf8) ?? Data()
        data.append(contentsOf: encodeBigEndian(UInt16(topicBytes.count)))
        data.append(topicBytes)
        if let payload = payload {
            data.append(payload)
        }
        return data
    }

    /// Public alias for encodeRequest used by tests.
    static func encodeRequestPublic(requestId: UInt32, method: UInt8, topicPath: String, payload: Data?) -> Data {
        return encodeRequest(requestId: requestId, method: method, topicPath: topicPath, payload: payload)
    }

    // MARK: - Response Decoding

    struct ResponseResult {
        let requestId: UInt32
        let errorCode: UInt8
        let data: Data
    }

    /// Decodes a response frame.
    /// Format: [frame_type(1)][request_id(4)][error_code(1)][result_data...]
    static func decodeResponse(_ frame: Data) -> ResponseResult {
        guard frame.count >= 6 else {
            return ResponseResult(requestId: 0, errorCode: 0xFF, data: Data())
        }
        let requestId = UInt32(frame[1]) << 24 |
                       UInt32(frame[2]) << 16 |
                       UInt32(frame[3]) << 8 |
                       UInt32(frame[4])
        let errorCode = frame[5]
        let resultData = frame.count > 6 ? Data(frame[6...]) : Data()
        return ResponseResult(requestId: requestId, errorCode: errorCode, data: resultData)
    }

    /// Public alias for decodeResponse used by tests.
    static func decodeResponsePublic(_ frame: Data) -> ResponseResult {
        return decodeResponse(frame)
    }

    // MARK: - Event Decoding

    struct EventResult {
        let eventType: UInt8
        let topicId: String
        let subtopic: String
        let data: Data
    }

    /// Decodes an event frame.
    /// Format: [frame_type(1)][event_type(1)][topic_len(2)][topic(N)][subtopic_len(2)][subtopic(N)][data_len(4)][data(M)]
    static func decodeEvent(_ frame: Data) -> EventResult {
        guard frame.count >= 2 else {
            return EventResult(eventType: 0, topicId: "", subtopic: "", data: Data())
        }
        let eventType = frame[1]
        var offset = 2

        // Topic
        guard frame.count >= offset + 2 else {
            return EventResult(eventType: eventType, topicId: "", subtopic: "", data: Data())
        }
        let topicLen = Int(frame[offset]) << 8 | Int(frame[offset + 1])
        offset += 2
        guard frame.count >= offset + topicLen else {
            return EventResult(eventType: eventType, topicId: "", subtopic: "", data: Data())
        }
        let topicId = String(data: frame[offset..<(offset + topicLen)], encoding: .utf8) ?? ""
        offset += topicLen

        // Subtopic
        guard frame.count >= offset + 2 else {
            return EventResult(eventType: eventType, topicId: topicId, subtopic: "", data: Data())
        }
        let subtopicLen = Int(frame[offset]) << 8 | Int(frame[offset + 1])
        offset += 2
        guard frame.count >= offset + subtopicLen else {
            return EventResult(eventType: eventType, topicId: topicId, subtopic: "", data: Data())
        }
        let subtopic = String(data: frame[offset..<(offset + subtopicLen)], encoding: .utf8) ?? ""
        offset += subtopicLen

        // Data
        guard frame.count >= offset + 4 else {
            return EventResult(eventType: eventType, topicId: topicId, subtopic: subtopic, data: Data())
        }
        let dataLen = Int(frame[offset]) << 24 | Int(frame[offset + 1]) << 16 |
                       Int(frame[offset + 2]) << 8 | Int(frame[offset + 3])
        offset += 4
        guard frame.count >= offset + dataLen else {
            return EventResult(eventType: eventType, topicId: topicId, subtopic: subtopic, data: Data())
        }
        let eventData = Data(frame[offset..<(offset + dataLen)])
        return EventResult(eventType: eventType, topicId: topicId, subtopic: subtopic, data: eventData)
    }

    /// Public alias for decodeEvent used by tests.
    static func decodeEventPublic(_ frame: Data) -> EventResult {
        return decodeEvent(frame)
    }

    // MARK: - Callback Triggers (for testing)

    /// Triggers the message callback. Used by tests.
    func triggerMessageCallbackPublic(topic: String, subtopic: String, data: Data) {
        messageCallback?(topic, subtopic, data)
    }

    /// Triggers the event callback. Used by tests.
    func triggerEventCallbackPublic(eventType: UInt8, data: Data) {
        eventCallback?(eventType, data)
    }

    // MARK: - XPC Event Handler

    private func handleEvent(_ event: xpc_object_t) {
        guard xpc_get_type(event) == XPC_TYPE_DICTIONARY else { return }

        let frameData = xpc_dictionary_get_data(event, "frame", nil)
        guard let frame = frameData else { return }

        let frameType = frame.pointee

        if frameType == Self.frameResponse {
            let response = Self.decodeResponse(Data(bytes: frame, count: frame.count))
            queue.sync(flags: .barrier) {
                pendingRequests.removeValue(forKey: response.requestId)?(.success(response.data))
            }
        } else if frameType == Self.frameEvent {
            let event = Self.decodeEvent(Data(bytes: frame, count: frame.count))
            if event.eventType == 1 {
                // MESSAGE event: invoke message callback with topic/subtopic/data
                messageCallback?(event.topicId, event.subtopic, event.data)
            }
            eventCallback?(event.eventType, event.data)
        }
    }
}

enum PoseidonError: Error {
    case notConnected
    case timeout
    case invalidResponse
}

private func encodeBigEndian<T>(_ value: T) -> Data where T: FixedWidthInteger {
    var mutableValue = value.bigEndian
    return withUnsafeBytes(of: &mutableValue) { Data($0) }
}