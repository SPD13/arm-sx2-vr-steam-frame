// swift-tools-version:5.9
// The trailer renderer (tools/trailer/README.md): reads a capture from the headset and renders shots.
import PackageDescription

let package = Package(
    name: "trailer",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(
            name: "trailer",
            path: "Sources/trailer",
            swiftSettings: [.unsafeFlags(["-Ounchecked"])]
        )
    ]
)
