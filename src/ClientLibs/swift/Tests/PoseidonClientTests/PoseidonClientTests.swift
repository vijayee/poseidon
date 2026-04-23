import XCTest
@testable import PoseidonClient

final class PoseidonClientTests: XCTestCase {

    // MARK: - Method Codes

    func testMethodCodes() {
        XCTAssertEqual(PoseidonConnection.methodChannelCreate, 1)
        XCTAssertEqual(PoseidonConnection.methodChannelJoin, 2)
        XCTAssertEqual(PoseidonConnection.methodChannelLeave, 3)
        XCTAssertEqual(PoseidonConnection.methodChannelDestroy, 4)
        XCTAssertEqual(PoseidonConnection.methodChannelModify, 5)
        XCTAssertEqual(PoseidonConnection.methodSubscribe, 6)
        XCTAssertEqual(PoseidonConnection.methodUnsubscribe, 7)
        XCTAssertEqual(PoseidonConnection.methodPublish, 8)
        XCTAssertEqual(PoseidonConnection.methodAliasRegister, 9)
        XCTAssertEqual(PoseidonConnection.methodAliasUnregister, 10)
        XCTAssertEqual(PoseidonConnection.methodAliasResolve, 11)
    }

    // MARK: - Frame Type Codes

    func testFrameTypeCodes() {
        XCTAssertEqual(PoseidonConnection.frameRequest, 0x01)
        XCTAssertEqual(PoseidonConnection.frameResponse, 0x02)
        XCTAssertEqual(PoseidonConnection.frameEvent, 0x03)
    }

    // MARK: - Request Encoding

    func testEncodeRequestBasic() {
        let data = PoseidonConnection.encodeRequestPublic(
            requestId: 1,
            method: PoseidonConnection.methodSubscribe,
            topicPath: "Feeds/public",
            payload: nil
        )

        // Frame type byte
        XCTAssertEqual(data[0], PoseidonConnection.frameRequest)

        // Request ID (big-endian UInt32)
        let requestId = UInt32(data[1]) << 24 |
                        UInt32(data[2]) << 16 |
                        UInt32(data[3]) << 8 |
                        UInt32(data[4])
        XCTAssertEqual(requestId, 1)

        // Method byte
        XCTAssertEqual(data[5], PoseidonConnection.methodSubscribe)

        // Topic length (big-endian UInt16) starts at byte 6
        let topicLen = Int(data[6]) << 8 | Int(data[7])
        XCTAssertEqual(topicLen, 11) // "Feeds/public" = 11 bytes

        // Topic bytes
        let topicData = data[8..<(8 + topicLen)]
        XCTAssertEqual(String(data: topicData, encoding: .utf8), "Feeds/public")
    }

    func testEncodeRequestWithPayload() {
        let payload = Data([0x01, 0x02, 0x03])
        let data = PoseidonConnection.encodeRequestPublic(
            requestId: 42,
            method: PoseidonConnection.methodPublish,
            topicPath: "test",
            payload: payload
        )

        XCTAssertEqual(data[0], PoseidonConnection.frameRequest)

        let requestId = UInt32(data[1]) << 24 |
                        UInt32(data[2]) << 16 |
                        UInt32(data[3]) << 8 |
                        UInt32(data[4])
        XCTAssertEqual(requestId, 42)

        XCTAssertEqual(data[5], PoseidonConnection.methodPublish)

        let topicLen = Int(data[6]) << 8 | Int(data[7])
        XCTAssertEqual(topicLen, 4) // "test" = 4 bytes

        // Payload follows the topic
        let payloadStart = 8 + topicLen
        let payloadSlice = data[payloadStart..<(payloadStart + payload.count)]
        XCTAssertEqual(Data(payloadSlice), payload)
    }

    func testEncodeRequestIdBigEndian() {
        let data = PoseidonConnection.encodeRequestPublic(
            requestId: 0x01020304,
            method: 1,
            topicPath: "x",
            payload: nil
        )

        // Request ID in big-endian: 0x01 0x02 0x03 0x04
        XCTAssertEqual(data[1], 0x01)
        XCTAssertEqual(data[2], 0x02)
        XCTAssertEqual(data[3], 0x03)
        XCTAssertEqual(data[4], 0x04)
    }

    func testEncodeRequestEmptyTopic() {
        let data = PoseidonConnection.encodeRequestPublic(
            requestId: 1,
            method: PoseidonConnection.methodChannelCreate,
            topicPath: "",
            payload: nil
        )

        let topicLen = Int(data[6]) << 8 | Int(data[7])
        XCTAssertEqual(topicLen, 0)
        XCTAssertEqual(data.count, 8) // header only, no topic bytes
    }

    // MARK: - Response Decoding

    func testDecodeResponse() {
        // Build a response frame: [0x02, request_id(4), error_code, result_data...]
        var frame = Data()
        frame.append(0x02) // frame type: response
        frame.append(contentsOf: encodeBigEndian(UInt32(7)))
        frame.append(0x00) // error code: OK
        frame.append(contentsOf: "created".data(using: .utf8)!)

        let result = PoseidonConnection.decodeResponsePublic(frame)
        XCTAssertEqual(result.requestId, 7)
        XCTAssertEqual(result.errorCode, 0)
    }

    func testDecodeResponseErrorCode() {
        var frame = Data()
        frame.append(0x02) // frame type: response
        frame.append(contentsOf: encodeBigEndian(UInt32(1)))
        frame.append(0x03) // error code: CHANNEL_NOT_FOUND

        let result = PoseidonConnection.decodeResponsePublic(frame)
        XCTAssertEqual(result.errorCode, 3)
    }

    // MARK: - Event Decoding

    func testDecodeMessageEvent() {
        // Build an event frame: [0x03, event_type, topic_len(2), topic, subtopic_len(2), subtopic, data...]
        var frame = Data()
        frame.append(0x03) // frame type: event
        frame.append(0x01) // event type: MESSAGE
        let topicData = "Feeds/public".data(using: .utf8)!
        frame.append(contentsOf: encodeBigEndian(UInt16(topicData.count)))
        frame.append(topicData)
        let subtopicData = "updates".data(using: .utf8)!
        frame.append(contentsOf: encodeBigEndian(UInt16(subtopicData.count)))
        frame.append(subtopicData)
        let eventData = Data([0xAA, 0xBB, 0xCC])
        frame.append(contentsOf: encodeBigEndian(UInt32(eventData.count)))
        frame.append(eventData)

        let result = PoseidonConnection.decodeEventPublic(frame)
        XCTAssertEqual(result.eventType, 1)
        XCTAssertEqual(result.topicId, "Feeds/public")
        XCTAssertEqual(result.subtopic, "updates")
        XCTAssertEqual(result.data, eventData)
    }

    func testDecodeChannelJoinedEvent() {
        var frame = Data()
        frame.append(0x03) // event type: event
        frame.append(0x02) // event type: CHANNEL_JOINED
        let topicData = "abc123".data(using: .utf8)!
        frame.append(contentsOf: encodeBigEndian(UInt16(topicData.count)))
        frame.append(topicData)
        // Empty subtopic
        frame.append(contentsOf: encodeBigEndian(UInt16(0)))
        // Empty data
        frame.append(contentsOf: encodeBigEndian(UInt32(0)))

        let result = PoseidonConnection.decodeEventPublic(frame)
        XCTAssertEqual(result.eventType, 2)
        XCTAssertEqual(result.topicId, "abc123")
        XCTAssertEqual(result.subtopic, "")
    }

    // MARK: - Request ID Generation

    func testRequestIdMonotonicallyIncreases() {
        let conn = PoseidonConnection()
        let id1 = conn.nextRequestIdPublic()
        let id2 = conn.nextRequestIdPublic()
        let id3 = conn.nextRequestIdPublic()
        XCTAssertEqual(id1, 0)
        XCTAssertEqual(id2, 1)
        XCTAssertEqual(id3, 2)
    }

    // MARK: - Error Types

    func testPoseidonErrors() {
        // Verify all error cases exist and are distinct
        let errors: [PoseidonError] = [.notConnected, .timeout, .invalidResponse]
        XCTAssertEqual(errors.count, 3)

        // Verify each error can be caught separately
        do {
            throw PoseidonError.notConnected
        } catch PoseidonError.notConnected {
            // expected
        } catch {
            XCTFail("Expected notConnected")
        }

        do {
            throw PoseidonError.timeout
        } catch PoseidonError.timeout {
            // expected
        } catch {
            XCTFail("Expected timeout")
        }

        do {
            throw PoseidonError.invalidResponse
        } catch PoseidonError.invalidResponse {
            // expected
        } catch {
            XCTFail("Expected invalidResponse")
        }
    }

    // MARK: - ObjC Wrapper Creation

    func testObjCWrapperCreation() {
        let connection = PoseidonConnection()
        let client = PoseidonClient(connection: connection)
        let objc = PoseidonClientObjC(client: client)
        XCTAssertNotNil(objc)
    }

    // MARK: - Message Callback

    func testOnMessageCallbackRegistration() {
        let connection = PoseidonConnection()
        var callbackCalled = false
        connection.onMessage { topic, subtopic, data in
            callbackCalled = true
        }
        // Trigger the callback manually to verify it was stored
        connection.triggerMessageCallbackPublic(topic: "test", subtopic: "sub", data: Data())
        XCTAssertTrue(callbackCalled)
    }

    func testOnEventCallbackRegistration() {
        let connection = PoseidonConnection()
        var receivedEventType: UInt8 = 0
        var receivedData: Data?
        connection.onEvent { eventType, data in
            receivedEventType = eventType
            receivedData = data
        }
        let testData = Data([0x01, 0x02])
        connection.triggerEventCallbackPublic(eventType: 1, data: testData)
        XCTAssertEqual(receivedEventType, 1)
        XCTAssertEqual(receivedData, testData)
    }

    // MARK: - Subscribe with Loopback

    func testSubscribeWithLoopback() {
        // Verify that subscribe with loopback encodes the loopback flag in payload
        let data = PoseidonConnection.encodeRequestPublic(
            requestId: 1,
            method: PoseidonConnection.methodSubscribe,
            topicPath: "test",
            payload: Data([0x01]) // loopback = true
        )

        let topicLen = Int(data[6]) << 8 | Int(data[7])
        let payloadStart = 8 + topicLen
        XCTAssertEqual(data[payloadStart], 0x01) // loopback flag
    }

    // MARK: - Disconnect

    func testDisconnectClearsConnection() {
        let connection = PoseidonConnection()
        // Without connecting, disconnect should not crash
        connection.disconnect()
    }
}

// Helper to convert values to big-endian bytes
private func encodeBigEndian<T>(_ value: T) -> Data where T: FixedWidthInteger {
    var mutableValue = value.bigEndian
    return withUnsafeBytes(of: &mutableValue) { Data($0) }
}