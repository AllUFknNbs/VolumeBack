#!/usr/bin/swift
// Generiert Resources/AppIcon.icns: macOS-Squircle mit Blauverlauf und
// weissem Lautsprecher-Symbol. Aufruf: swift Scripts/make_icon.swift
import AppKit

let iconsetVariants: [(pixels: Int, name: String)] = [
    (16, "icon_16x16"), (32, "icon_16x16@2x"),
    (32, "icon_32x32"), (64, "icon_32x32@2x"),
    (128, "icon_128x128"), (256, "icon_128x128@2x"),
    (256, "icon_256x256"), (512, "icon_256x256@2x"),
    (512, "icon_512x512"), (1024, "icon_512x512@2x"),
]

func tinted(_ image: NSImage, color: NSColor) -> NSImage {
    let result = NSImage(size: image.size)
    result.lockFocus()
    let rect = NSRect(origin: .zero, size: image.size)
    image.draw(in: rect)
    color.set()
    rect.fill(using: .sourceAtop)
    result.unlockFocus()
    return result
}

func renderIcon(pixels: Int) -> NSBitmapImageRep {
    guard let rep = NSBitmapImageRep(
        bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels,
        bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
        colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0
    ) else { fatalError("bitmap rep") }
    rep.size = NSSize(width: pixels, height: pixels)

    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    defer { NSGraphicsContext.restoreGraphicsState() }

    let side = CGFloat(pixels)

    // macOS-Squircle: ~82% der Flaeche, zentriert
    let inset = side * 0.09
    let squircle = NSRect(x: inset, y: inset, width: side - 2 * inset, height: side - 2 * inset)
    let radius = squircle.width * 0.225
    let path = NSBezierPath(roundedRect: squircle, xRadius: radius, yRadius: radius)

    // Dezenter Schatten wie bei System-Icons
    NSGraphicsContext.current?.saveGraphicsState()
    let shadow = NSShadow()
    shadow.shadowColor = NSColor.black.withAlphaComponent(0.3)
    shadow.shadowBlurRadius = side * 0.015
    shadow.shadowOffset = NSSize(width: 0, height: -side * 0.008)
    shadow.set()
    NSColor.black.withAlphaComponent(0.001).setFill()
    path.fill()
    NSGraphicsContext.current?.restoreGraphicsState()

    let gradient = NSGradient(
        starting: NSColor(calibratedRed: 0.42, green: 0.65, blue: 1.00, alpha: 1),
        ending: NSColor(calibratedRed: 0.10, green: 0.24, blue: 0.78, alpha: 1)
    )!
    gradient.draw(in: path, angle: -90)

    // Lautsprecher-Symbol in Weiss
    guard let symbol = NSImage(systemSymbolName: "speaker.wave.2.fill", accessibilityDescription: nil) else {
        fatalError("SF Symbol nicht verfügbar")
    }
    let config = NSImage.SymbolConfiguration(pointSize: side * 0.45, weight: .medium)
    guard let sized = symbol.withSymbolConfiguration(config) else { fatalError("symbol config") }
    let white = tinted(sized, color: .white)

    let targetWidth = squircle.width * 0.62
    let scale = targetWidth / white.size.width
    let drawSize = NSSize(width: white.size.width * scale, height: white.size.height * scale)
    let origin = NSPoint(
        x: squircle.midX - drawSize.width / 2,
        y: squircle.midY - drawSize.height / 2
    )
    white.draw(
        in: NSRect(origin: origin, size: drawSize),
        from: .zero, operation: .sourceOver, fraction: 1.0
    )

    return rep
}

// Iconset schreiben
let scriptDir = URL(fileURLWithPath: CommandLine.arguments[0]).deletingLastPathComponent()
let projectRoot = scriptDir.deletingLastPathComponent()
let iconsetURL = projectRoot.appendingPathComponent("build/AppIcon.iconset")
let icnsURL = projectRoot.appendingPathComponent("Resources/AppIcon.icns")

let fm = FileManager.default
try? fm.removeItem(at: iconsetURL)
try! fm.createDirectory(at: iconsetURL, withIntermediateDirectories: true)

for variant in iconsetVariants {
    let rep = renderIcon(pixels: variant.pixels)
    guard let png = rep.representation(using: .png, properties: [:]) else { fatalError("png") }
    try! png.write(to: iconsetURL.appendingPathComponent("\(variant.name).png"))
}

// icns bauen
let iconutil = Process()
iconutil.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
iconutil.arguments = ["-c", "icns", iconsetURL.path, "-o", icnsURL.path]
try! iconutil.run()
iconutil.waitUntilExit()
guard iconutil.terminationStatus == 0 else { fatalError("iconutil fehlgeschlagen") }
print("OK: \(icnsURL.path)")
