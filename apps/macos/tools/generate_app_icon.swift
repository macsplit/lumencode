import AppKit
import Foundation

struct IconRenderer {
    static let glyphColor = NSColor(calibratedRed: 245 / 255, green: 166 / 255, blue: 35 / 255, alpha: 1)

    static func makeImage(size: CGFloat) -> NSImage {
        let image = NSImage(size: NSSize(width: size, height: size))
        image.lockFocus()

        let shadow = NSShadow()
        shadow.shadowColor = NSColor.black.withAlphaComponent(0.14)
        shadow.shadowBlurRadius = size * 0.025
        shadow.shadowOffset = NSSize(width: 0, height: -size * 0.008)
        shadow.set()

        let glyphScale = (size * 0.84) / 256
        let xOffset = (size - (256 * glyphScale)) / 2
        let yOffset = (size - (256 * glyphScale)) / 2 - size * 0.02
        if let context = NSGraphicsContext.current?.cgContext {
            context.saveGState()
            context.translateBy(x: xOffset, y: size - yOffset)
            context.scaleBy(x: 1, y: -1)

            let outer = NSBezierPath()
            outer.move(to: CGPoint(x: 128 * glyphScale, y: 230 * glyphScale))
            outer.curve(
                to: CGPoint(x: 152.82 * glyphScale, y: 191.7 * glyphScale),
                controlPoint1: CGPoint(x: 141 * glyphScale, y: 230 * glyphScale),
                controlPoint2: CGPoint(x: 149 * glyphScale, y: 225 * glyphScale)
            )
            outer.line(to: CGPoint(x: 103.18 * glyphScale, y: 191.7 * glyphScale))
            outer.curve(
                to: CGPoint(x: 128 * glyphScale, y: 230 * glyphScale),
                controlPoint1: CGPoint(x: 107 * glyphScale, y: 225 * glyphScale),
                controlPoint2: CGPoint(x: 115 * glyphScale, y: 230 * glyphScale)
            )
            outer.move(to: CGPoint(x: 103.18 * glyphScale, y: 191.7 * glyphScale))
            outer.curve(
                to: CGPoint(x: 85.21 * glyphScale, y: 157.47 * glyphScale),
                controlPoint1: CGPoint(x: 102 * glyphScale, y: 178.38 * glyphScale),
                controlPoint2: CGPoint(x: 95.79 * glyphScale, y: 165.64 * glyphScale)
            )
            outer.curve(
                to: CGPoint(x: 54 * glyphScale, y: 98 * glyphScale),
                controlPoint1: CGPoint(x: 67.98 * glyphScale, y: 144.17 * glyphScale),
                controlPoint2: CGPoint(x: 54 * glyphScale, y: 123.83 * glyphScale)
            )
            outer.curve(
                to: CGPoint(x: 128 * glyphScale, y: 26 * glyphScale),
                controlPoint1: CGPoint(x: 54 * glyphScale, y: 58.24 * glyphScale),
                controlPoint2: CGPoint(x: 87.13 * glyphScale, y: 26 * glyphScale)
            )
            outer.curve(
                to: CGPoint(x: 202 * glyphScale, y: 98 * glyphScale),
                controlPoint1: CGPoint(x: 168.87 * glyphScale, y: 26 * glyphScale),
                controlPoint2: CGPoint(x: 202 * glyphScale, y: 58.24 * glyphScale)
            )
            outer.curve(
                to: CGPoint(x: 170.79 * glyphScale, y: 157.47 * glyphScale),
                controlPoint1: CGPoint(x: 202 * glyphScale, y: 123.83 * glyphScale),
                controlPoint2: CGPoint(x: 188.02 * glyphScale, y: 144.17 * glyphScale)
            )
            outer.curve(
                to: CGPoint(x: 152.82 * glyphScale, y: 191.7 * glyphScale),
                controlPoint1: CGPoint(x: 160.21 * glyphScale, y: 165.64 * glyphScale),
                controlPoint2: CGPoint(x: 154 * glyphScale, y: 178.38 * glyphScale)
            )

            let middleBar = NSBezierPath()
            middleBar.move(to: CGPoint(x: 103 * glyphScale, y: 191 * glyphScale))
            middleBar.line(to: CGPoint(x: 153 * glyphScale, y: 191 * glyphScale))

            let lowerBar = NSBezierPath()
            lowerBar.move(to: CGPoint(x: 108 * glyphScale, y: 212 * glyphScale))
            lowerBar.line(to: CGPoint(x: 148 * glyphScale, y: 212 * glyphScale))

            let baseBar = NSBezierPath()
            baseBar.move(to: CGPoint(x: 115 * glyphScale, y: 230 * glyphScale))
            baseBar.line(to: CGPoint(x: 141 * glyphScale, y: 230 * glyphScale))

            let leftChevron = NSBezierPath()
            leftChevron.move(to: CGPoint(x: 111 * glyphScale, y: 92 * glyphScale))
            leftChevron.line(to: CGPoint(x: 91 * glyphScale, y: 112 * glyphScale))
            leftChevron.line(to: CGPoint(x: 111 * glyphScale, y: 132 * glyphScale))

            let rightChevron = NSBezierPath()
            rightChevron.move(to: CGPoint(x: 145 * glyphScale, y: 92 * glyphScale))
            rightChevron.line(to: CGPoint(x: 165 * glyphScale, y: 112 * glyphScale))
            rightChevron.line(to: CGPoint(x: 145 * glyphScale, y: 132 * glyphScale))

            let paths: [(NSBezierPath, CGFloat)] = [
                (outer, 14 * glyphScale),
                (middleBar, 14 * glyphScale),
                (lowerBar, 14 * glyphScale),
                (baseBar, 14 * glyphScale),
                (leftChevron, 12 * glyphScale),
                (rightChevron, 12 * glyphScale)
            ]

            glyphColor.setStroke()
            for (path, width) in paths {
                path.lineWidth = width
                path.lineCapStyle = .round
                path.lineJoinStyle = .round
                path.stroke()
            }
            context.restoreGState()
        }

        image.unlockFocus()
        return image
    }
}

func pngData(from image: NSImage) -> Data? {
    guard let tiff = image.tiffRepresentation,
          let rep = NSBitmapImageRep(data: tiff) else {
        return nil
    }
    return rep.representation(using: .png, properties: [:])
}

let arguments = CommandLine.arguments
guard arguments.count == 2 else {
    fputs("usage: generate_app_icon.swift <output-dir>\n", stderr)
    exit(1)
}

let outputRoot = URL(fileURLWithPath: arguments[1], isDirectory: true)
let fileManager = FileManager.default
try fileManager.createDirectory(at: outputRoot, withIntermediateDirectories: true)

let iconset = outputRoot.appendingPathComponent("LumenCode.iconset", isDirectory: true)
try? fileManager.removeItem(at: iconset)
try fileManager.createDirectory(at: iconset, withIntermediateDirectories: true)

let sizes: [(Int, String)] = [
    (16, "icon_16x16.png"),
    (32, "icon_16x16@2x.png"),
    (32, "icon_32x32.png"),
    (64, "icon_32x32@2x.png"),
    (128, "icon_128x128.png"),
    (256, "icon_128x128@2x.png"),
    (256, "icon_256x256.png"),
    (512, "icon_256x256@2x.png"),
    (512, "icon_512x512.png"),
    (1024, "icon_512x512@2x.png")
]

for (size, name) in sizes {
    let image = IconRenderer.makeImage(size: CGFloat(size))
    guard let data = pngData(from: image) else {
        fputs("failed to render \(name)\n", stderr)
        exit(2)
    }
    try data.write(to: iconset.appendingPathComponent(name))
}
