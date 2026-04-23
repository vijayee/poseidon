import XCTest
@testable import PoseidonClient

final class PoseidonClientTests: XCTestCase {
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
    }
}