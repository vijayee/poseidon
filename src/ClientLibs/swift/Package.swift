// swift-tools-version:5.9

import PackageDescription

let package = Package(
    name: "PoseidonClient",
    platforms: [
        .macOS(.v10_15),
        .iOS(.v13)
    ],
    products: [
        .library(
            name: "PoseidonClient",
            targets: ["PoseidonClient"]
        )
    ],
    targets: [
        .target(
            name: "PoseidonClient",
            path: "Sources",
            swiftSettings: [
                .unsafeFlags(["-parse-as-library"], .configuration(.release))
            ]
        ),
        .testTarget(
            name: "PoseidonClientTests",
            dependencies: ["PoseidonClient"],
            path: "Tests"
        )
    ]
)