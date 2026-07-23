// swift-tools-version:6.0
import PackageDescription

let package = Package(
    name: "VolumeBack",
    platforms: [.macOS(.v15)],
    targets: [
        .executableTarget(
            name: "VolumeBack",
            path: "Sources/VolumeBack",
            swiftSettings: [.swiftLanguageMode(.v5)]
        )
    ]
)
