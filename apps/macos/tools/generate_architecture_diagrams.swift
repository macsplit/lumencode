import AppKit
import Foundation

struct DiagramBox {
    let rect: CGRect
    let title: String
    let body: String
    let fill: NSColor
}

enum DiagramRenderer {
    static let canvas = CGSize(width: 1600, height: 980)
    static let background = NSColor(calibratedWhite: 0.11, alpha: 1)
    static let panel = NSColor(calibratedWhite: 0.16, alpha: 1)
    static let stroke = NSColor(calibratedWhite: 0.28, alpha: 1)
    static let textPrimary = NSColor(calibratedWhite: 0.96, alpha: 1)
    static let textSecondary = NSColor(calibratedWhite: 0.80, alpha: 1)
    static let accent = NSColor(calibratedRed: 245 / 255, green: 166 / 255, blue: 35 / 255, alpha: 1)
    static let green = NSColor(calibratedRed: 77 / 255, green: 189 / 255, blue: 116 / 255, alpha: 1)
    static let blue = NSColor(calibratedRed: 83 / 255, green: 157 / 255, blue: 255 / 255, alpha: 1)
    static let pink = NSColor(calibratedRed: 221 / 255, green: 104 / 255, blue: 173 / 255, alpha: 1)

    static func image(title: String, subtitle: String, boxes: [DiagramBox], arrows: [(CGPoint, CGPoint, NSColor)], footer: String) -> NSImage {
        let image = NSImage(size: NSSize(width: canvas.width, height: canvas.height))
        image.lockFocus()

        background.setFill()
        NSBezierPath(rect: CGRect(origin: .zero, size: canvas)).fill()

        drawHeader(title: title, subtitle: subtitle)
        drawFooter(footer)

        for arrow in arrows {
            drawArrow(from: arrow.0, to: arrow.1, color: arrow.2)
        }

        for box in boxes {
            drawBox(box)
        }

        image.unlockFocus()
        return image
    }

    static func drawHeader(title: String, subtitle: String) {
        let titleAttributes: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 40, weight: .bold),
            .foregroundColor: textPrimary
        ]
        let subtitleAttributes: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 20, weight: .regular),
            .foregroundColor: textSecondary
        ]

        title.draw(at: CGPoint(x: 70, y: canvas.height - 88), withAttributes: titleAttributes)
        subtitle.draw(at: CGPoint(x: 72, y: canvas.height - 128), withAttributes: subtitleAttributes)
    }

    static func drawFooter(_ text: String) {
        let attributes: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 16, weight: .medium),
            .foregroundColor: textSecondary
        ]
        text.draw(at: CGPoint(x: 72, y: 28), withAttributes: attributes)
    }

    static func drawBox(_ box: DiagramBox) {
        let path = NSBezierPath(roundedRect: box.rect, xRadius: 22, yRadius: 22)
        box.fill.setFill()
        path.fill()

        stroke.setStroke()
        path.lineWidth = 2
        path.stroke()

        let titleRect = CGRect(x: box.rect.minX + 22, y: box.rect.maxY - 56, width: box.rect.width - 44, height: 28)
        let bodyRect = CGRect(x: box.rect.minX + 22, y: box.rect.minY + 22, width: box.rect.width - 44, height: box.rect.height - 78)

        let paragraph = NSMutableParagraphStyle()
        paragraph.lineSpacing = 4

        let titleAttributes: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 24, weight: .bold),
            .foregroundColor: textPrimary
        ]
        let bodyAttributes: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 19, weight: .regular),
            .foregroundColor: textSecondary,
            .paragraphStyle: paragraph
        ]

        (box.title as NSString).draw(in: titleRect, withAttributes: titleAttributes)
        (box.body as NSString).draw(in: bodyRect, withAttributes: bodyAttributes)
    }

    static func drawArrow(from: CGPoint, to: CGPoint, color: NSColor) {
        let path = NSBezierPath()
        path.move(to: from)
        path.line(to: to)
        color.setStroke()
        path.lineWidth = 5
        path.lineCapStyle = .round
        path.stroke()

        let angle = atan2(to.y - from.y, to.x - from.x)
        let headLength: CGFloat = 16
        let left = CGPoint(
            x: to.x - headLength * cos(angle - .pi / 6),
            y: to.y - headLength * sin(angle - .pi / 6)
        )
        let right = CGPoint(
            x: to.x - headLength * cos(angle + .pi / 6),
            y: to.y - headLength * sin(angle + .pi / 6)
        )

        let head = NSBezierPath()
        head.move(to: to)
        head.line(to: left)
        head.move(to: to)
        head.line(to: right)
        head.lineWidth = 5
        head.lineCapStyle = .round
        head.stroke()
    }

    static func pngData(from image: NSImage) -> Data? {
        guard let tiff = image.tiffRepresentation,
              let rep = NSBitmapImageRep(data: tiff) else {
            return nil
        }
        return rep.representation(using: .png, properties: [:])
    }
}

func writeDiagram(_ image: NSImage, to url: URL) throws {
    guard let data = DiagramRenderer.pngData(from: image) else {
        throw NSError(domain: "DiagramRenderer", code: 1)
    }
    try data.write(to: url)
}

guard CommandLine.arguments.count == 2 else {
    fputs("usage: generate_architecture_diagrams.swift <output-dir>\n", stderr)
    exit(1)
}

let outputRoot = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
try FileManager.default.createDirectory(at: outputRoot, withIntermediateDirectories: true)

let overview = DiagramRenderer.image(
    title: "LumenCode Architecture",
    subtitle: "The app is split into a UI layer, a shared Swift core, and a native parser engine.",
    boxes: [
        DiagramBox(
            rect: CGRect(x: 90, y: 520, width: 360, height: 250),
            title: "LumenCode App",
            body: "SwiftUI macOS app\n\nShows the file list, symbol list, inspector, and lower code pane.\n\nThis is the part you click on.",
            fill: DiagramRenderer.panel
        ),
        DiagramBox(
            rect: CGRect(x: 620, y: 520, width: 360, height: 250),
            title: "LumencodeCore",
            body: "Shared Swift logic\n\nProject scanning\nSelection helpers\nProject relationship linking\nModel decoding",
            fill: DiagramRenderer.panel
        ),
        DiagramBox(
            rect: CGRect(x: 1150, y: 520, width: 360, height: 250),
            title: "LumencodeNativeBridge",
            body: "C and C++ bridge\n\nCalls the native parser code\nBuilds symbols, dependencies, routes, snippets, and summaries",
            fill: DiagramRenderer.panel
        ),
        DiagramBox(
            rect: CGRect(x: 620, y: 180, width: 360, height: 220),
            title: "CLI Harness",
            body: "Internal developer loop\n\nLets us scan and inspect output quickly without running the GUI.",
            fill: DiagramRenderer.panel
        ),
        DiagramBox(
            rect: CGRect(x: 1150, y: 180, width: 360, height: 220),
            title: "Vendored Tree-sitter Parsers",
            body: "Checked-in parser code\n\nSwift, JS, TS, TSX, HTML, CSS, Python, Rust, PHP, Java, C# and more.",
            fill: DiagramRenderer.panel
        )
    ],
    arrows: [
        (CGPoint(x: 450, y: 645), CGPoint(x: 620, y: 645), DiagramRenderer.accent),
        (CGPoint(x: 980, y: 645), CGPoint(x: 1150, y: 645), DiagramRenderer.accent),
        (CGPoint(x: 800, y: 520), CGPoint(x: 800, y: 400), DiagramRenderer.green),
        (CGPoint(x: 1330, y: 520), CGPoint(x: 1330, y: 400), DiagramRenderer.blue)
    ],
    footer: "Think of it as: App shell -> shared Swift brain -> native parser engine."
)

let analysisFlow = DiagramRenderer.image(
    title: "What Happens When You Click Something",
    subtitle: "This is the normal path from opening a folder to seeing symbols and code snippets.",
    boxes: [
        DiagramBox(
            rect: CGRect(x: 70, y: 560, width: 250, height: 180),
            title: "1. Open Folder",
            body: "The app asks ProjectScanner to walk the project and decide which files matter.",
            fill: DiagramRenderer.panel
        ),
        DiagramBox(
            rect: CGRect(x: 380, y: 560, width: 250, height: 180),
            title: "2. Pick File",
            body: "WorkspaceViewModel chooses the selected file and asks the analyzer for details.",
            fill: DiagramRenderer.panel
        ),
        DiagramBox(
            rect: CGRect(x: 690, y: 560, width: 250, height: 180),
            title: "3. Native Analysis",
            body: "NativeBridge calls into C++ to parse the file and return JSON.",
            fill: DiagramRenderer.panel
        ),
        DiagramBox(
            rect: CGRect(x: 1000, y: 560, width: 250, height: 180),
            title: "4. Relationship Pass",
            body: "ProjectRelationships adds cross-file links like imported calls and related files.",
            fill: DiagramRenderer.panel
        ),
        DiagramBox(
            rect: CGRect(x: 1310, y: 560, width: 220, height: 180),
            title: "5. UI Update",
            body: "The app redraws the symbol list, inspector, and lower snippet pane.",
            fill: DiagramRenderer.panel
        ),
        DiagramBox(
            rect: CGRect(x: 505, y: 200, width: 600, height: 210),
            title: "If you then click a symbol",
            body: "WorkspaceSupport builds the snippet payload for that symbol, member, route, dependency, or relation.\n\nThat is why the lower pane can jump between different kinds of items without each view inventing its own code-loading logic.",
            fill: DiagramRenderer.panel
        )
    ],
    arrows: [
        (CGPoint(x: 320, y: 650), CGPoint(x: 380, y: 650), DiagramRenderer.accent),
        (CGPoint(x: 630, y: 650), CGPoint(x: 690, y: 650), DiagramRenderer.accent),
        (CGPoint(x: 940, y: 650), CGPoint(x: 1000, y: 650), DiagramRenderer.accent),
        (CGPoint(x: 1250, y: 650), CGPoint(x: 1310, y: 650), DiagramRenderer.accent),
        (CGPoint(x: 800, y: 560), CGPoint(x: 800, y: 410), DiagramRenderer.green)
    ],
    footer: "The key idea: the GUI is mostly choosing what to inspect; the backend decides what the file means."
)

let parserFlow = DiagramRenderer.image(
    title: "Backend Parsing and Data Flow",
    subtitle: "This is the piece we keep improving for parity with the original Kirigami version.",
    boxes: [
        DiagramBox(
            rect: CGRect(x: 90, y: 560, width: 300, height: 220),
            title: "Source File",
            body: "Any selected file on disk\n\nSwift, TSX, Python, Rust, PHP, HTML, CSS, and more.",
            fill: DiagramRenderer.panel
        ),
        DiagramBox(
            rect: CGRect(x: 470, y: 560, width: 300, height: 220),
            title: "Language Detection",
            body: "The bridge chooses a parser path from the file extension and filename.",
            fill: DiagramRenderer.panel
        ),
        DiagramBox(
            rect: CGRect(x: 850, y: 560, width: 300, height: 220),
            title: "Tree-sitter Parse",
            body: "Vendored grammars build an AST.\n\nRegex fallback only stays around where parity is still incomplete.",
            fill: DiagramRenderer.panel
        ),
        DiagramBox(
            rect: CGRect(x: 1230, y: 560, width: 300, height: 220),
            title: "Facts Extracted",
            body: "Symbols\nCalls and callers\nDependencies\nRoutes\nQuick links\nCSS and HTML linkage",
            fill: DiagramRenderer.panel
        ),
        DiagramBox(
            rect: CGRect(x: 360, y: 200, width: 420, height: 220),
            title: "JSON Bridge Output",
            body: "The native layer returns one JSON blob so Swift does not need to understand C++ objects directly.",
            fill: DiagramRenderer.panel
        ),
        DiagramBox(
            rect: CGRect(x: 860, y: 200, width: 420, height: 220),
            title: "Decoded Swift Models",
            body: "FileAnalysis, SymbolItem, DependencyItem, RouteItem, QuickLinkItem and related models power both the GUI and CLI.",
            fill: DiagramRenderer.panel
        )
    ],
    arrows: [
        (CGPoint(x: 390, y: 670), CGPoint(x: 470, y: 670), DiagramRenderer.accent),
        (CGPoint(x: 770, y: 670), CGPoint(x: 850, y: 670), DiagramRenderer.accent),
        (CGPoint(x: 1150, y: 670), CGPoint(x: 1230, y: 670), DiagramRenderer.accent),
        (CGPoint(x: 1380, y: 560), CGPoint(x: 1080, y: 420), DiagramRenderer.pink),
        (CGPoint(x: 570, y: 560), CGPoint(x: 570, y: 420), DiagramRenderer.green),
        (CGPoint(x: 780, y: 310), CGPoint(x: 860, y: 310), DiagramRenderer.blue)
    ],
    footer: "This is why the CLI is valuable: it lets us inspect parser output directly while improving the backend."
)

try writeDiagram(overview, to: outputRoot.appendingPathComponent("architecture-overview.png"))
try writeDiagram(analysisFlow, to: outputRoot.appendingPathComponent("architecture-analysis-flow.png"))
try writeDiagram(parserFlow, to: outputRoot.appendingPathComponent("architecture-parser-flow.png"))
