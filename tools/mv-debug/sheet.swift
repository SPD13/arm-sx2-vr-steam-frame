import AppKit
// contact sheet: sheet.swift out.jpg cols files...
let args = CommandLine.arguments
let out = args[1]; let cols = Int(args[2])!
let files = Array(args[3...])
let imgs = files.compactMap { NSImage(contentsOfFile: $0) }
let w = 320, h = 240
let rows = (imgs.count + cols - 1) / cols
let sheet = NSImage(size: NSSize(width: w*cols, height: h*rows))
sheet.lockFocus()
for (i, img) in imgs.enumerated() {
  let x = (i % cols) * w, y = (rows - 1 - i / cols) * h
  img.draw(in: NSRect(x: x, y: y, width: w, height: h))
  let label = NSString(string: URL(fileURLWithPath: files[i]).lastPathComponent)
  label.draw(at: NSPoint(x: x + 4, y: y + 4), withAttributes: [.foregroundColor: NSColor.yellow, .font: NSFont.boldSystemFont(ofSize: 14)])
}
sheet.unlockFocus()
let rep = NSBitmapImageRep(data: sheet.tiffRepresentation!)!
try! rep.representation(using: .jpeg, properties: [.compressionFactor: 0.8])!.write(to: URL(fileURLWithPath: out))
