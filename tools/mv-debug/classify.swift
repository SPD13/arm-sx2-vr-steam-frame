// classify.swift: ok / BROKEN / COVER for present dumps of the TC2 first shooting room (see docs/m3-multiview.md)
import AppKit
// wallcolor.swift files... : mean R-B over the wall band (rows 12-30%, cols 35-65%); pink walls > ~15, gray ~0
for path in CommandLine.arguments.dropFirst() {
  guard let img = NSImage(contentsOfFile: path), let tiff = img.tiffRepresentation, let rep = NSBitmapImageRep(data: tiff) else { print(path, "?"); continue }
  let w = rep.pixelsWide, h = rep.pixelsHigh
  var sr = 0.0, sb = 0.0, n = 0.0
  for y in stride(from: Int(Double(h)*0.12), to: Int(Double(h)*0.30), by: 4) {
    for x in stride(from: Int(Double(w)*0.35), to: Int(Double(w)*0.65), by: 4) {
      if let c = rep.colorAt(x: x, y: y)?.usingColorSpace(.deviceRGB) { sr += Double(c.redComponent); sb += Double(c.blueComponent); n += 1 }
    }
  }
  let d = (sr - sb) / max(n, 1) * 255
  // cover view: the player's blue jacket fills the centre right
  var br = 0.0, bb = 0.0, m = 0.0
  for y in stride(from: Int(Double(h)*0.35), to: Int(Double(h)*0.55), by: 4) {
    for x in stride(from: Int(Double(w)*0.45), to: Int(Double(w)*0.75), by: 4) {
      if let c = rep.colorAt(x: x, y: y)?.usingColorSpace(.deviceRGB) { br += Double(c.redComponent); bb += Double(c.blueComponent); m += 1 }
    }
  }
  let blue = (bb - br) / max(m, 1) * 255
  let verdict = blue > 10 ? "COVER" : (d > 17 ? "ok" : "BROKEN")
  print(String(format: "%@ %.1f blue %.1f %@", (path as NSString).lastPathComponent, d, blue, verdict))
}
