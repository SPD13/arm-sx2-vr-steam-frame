// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// Title cards and captions, drawn with Core Text into BGRA frames.

import CoreGraphics
import CoreText
import Foundation
import ImageIO
import UniformTypeIdentifiers

// A BGRA (premultiplied) bitmap the size of the output.
final class Bitmap {
    let width: Int, height: Int
    var pixels: [UInt8]

    init(width: Int, height: Int) {
        self.width = width
        self.height = height
        pixels = [UInt8](repeating: 0, count: width * height * 4)
    }

    func draw(_ body: (CGContext) -> Void) {
        pixels.withUnsafeMutableBytes { raw in
            let ctx = CGContext(data: raw.baseAddress, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4,
                                space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
            body(ctx)
        }
    }

    // This bitmap over `frame` (BGRA, opaque), at `alpha`.
    func composite(over frame: inout [UInt8], alpha: Float) {
        guard alpha > 0.001 else { return }
        let a = UInt32(min(max(alpha, 0), 1) * 256)
        pixels.withUnsafeBufferPointer { src in
            frame.withUnsafeMutableBufferPointer { dst in
                var i = 0
                while i < src.count {
                    let sa = UInt32(src[i + 3])
                    if sa != 0 {
                        let k = sa * a >> 8 // coverage, 0..255
                        for c in 0..<3 {
                            let s = UInt32(src[i + c]) * a >> 8
                            dst[i + c] = UInt8(min(255, s + UInt32(dst[i + c]) * (255 - k) / 255))
                        }
                    }
                    i += 4
                }
            }
        }
    }
}

func font(_ size: CGFloat, weight: CTFontUIFontType = .emphasizedSystem) -> CTFont {
    CTFontCreateUIFontForLanguage(weight, size, nil) ?? CTFontCreateWithName("Helvetica Neue" as CFString, size, nil)
}

func textLine(_ text: String, font: CTFont, color: CGColor, kern: CGFloat = 0) -> CTLine {
    let attrs: [NSAttributedString.Key: Any] = [
        NSAttributedString.Key(kCTFontAttributeName as String): font,
        NSAttributedString.Key(kCTForegroundColorAttributeName as String): color,
        NSAttributedString.Key(kCTKernAttributeName as String): kern,
    ]
    return CTLineCreateWithAttributedString(NSAttributedString(string: text, attributes: attrs))
}

func lineWidth(_ line: CTLine) -> CGFloat { CGFloat(CTLineGetTypographicBounds(line, nil, nil, nil)) }

let accent = CGColor(srgbRed: 1.0, green: 0.23, blue: 0.19, alpha: 1) // the gun's beam

// A full-screen card: a kicker line above, the title, a subtitle below.
func titleCard(width: Int, height: Int, kicker: String?, title: String, subtitle: String?) -> Bitmap {
    let b = Bitmap(width: width, height: height)
    let s = CGFloat(height) / 1080
    b.draw { ctx in
        // The void, a little lighter in the middle.
        let space = CGColorSpace(name: CGColorSpace.sRGB)!
        let gradient = CGGradient(colorsSpace: space, colors: [
            CGColor(srgbRed: 0.06, green: 0.07, blue: 0.1, alpha: 1), CGColor(srgbRed: 0.012, green: 0.014, blue: 0.02, alpha: 1),
        ] as CFArray, locations: [0, 1])!
        let centre = CGPoint(x: CGFloat(width) / 2, y: CGFloat(height) / 2)
        ctx.drawRadialGradient(gradient, startCenter: centre, startRadius: 0, endCenter: centre, endRadius: CGFloat(width) * 0.6,
                               options: [.drawsAfterEndLocation])
        let titleLine = textLine(title, font: font(96 * s), color: CGColor(gray: 1, alpha: 1), kern: -1 * s)
        let tw = lineWidth(titleLine)
        let y = CGFloat(height) / 2 - 30 * s
        ctx.textPosition = CGPoint(x: (CGFloat(width) - tw) / 2, y: y)
        CTLineDraw(titleLine, ctx)
        if let kicker {
            let k = textLine(kicker.uppercased(), font: font(26 * s), color: accent, kern: 6 * s)
            ctx.textPosition = CGPoint(x: (CGFloat(width) - lineWidth(k)) / 2, y: y + 120 * s)
            CTLineDraw(k, ctx)
        }
        if let subtitle {
            let sub = textLine(subtitle, font: font(40 * s, weight: .system), color: CGColor(gray: 0.72, alpha: 1))
            ctx.textPosition = CGPoint(x: (CGFloat(width) - lineWidth(sub)) / 2, y: y - 80 * s)
            CTLineDraw(sub, ctx)
        }
        ctx.setFillColor(accent)
        ctx.fill(CGRect(x: CGFloat(width) / 2 - 40 * s, y: y - 120 * s, width: 80 * s, height: 4 * s))
    }
    return b
}

// A caption in the lower left (or at the top of a half, for the comparison's labels).
func caption(width: Int, height: Int, text: String, detail: String?, at origin: CGPoint? = nil, size: CGFloat = 48) -> Bitmap {
    let b = Bitmap(width: width, height: height)
    let s = CGFloat(height) / 1080
    b.draw { ctx in
        let x = origin?.x ?? 96 * s
        let y = origin?.y ?? 120 * s
        ctx.setShadow(offset: CGSize(width: 0, height: -2 * s), blur: 18 * s, color: CGColor(gray: 0, alpha: 0.9))
        let line = textLine(text, font: font(size * s), color: CGColor(gray: 1, alpha: 1))
        if origin == nil {
            ctx.setFillColor(accent)
            ctx.fill(CGRect(x: x, y: y + (size + 14) * s, width: 56 * s, height: 4 * s))
        }
        ctx.textPosition = CGPoint(x: x, y: y)
        CTLineDraw(line, ctx)
        if let detail {
            let d = textLine(detail, font: font(size * 0.58 * s, weight: .system), color: CGColor(gray: 0.85, alpha: 1))
            ctx.textPosition = CGPoint(x: x, y: y - size * 0.95 * s)
            CTLineDraw(d, ctx)
        }
    }
    return b
}

func writePNG(_ bgra: [UInt8], width: Int, height: Int, to url: URL, alpha: Bool = false) throws {
    var pixels = bgra
    let image: CGImage? = pixels.withUnsafeMutableBytes { raw in
        let ctx = CGContext(data: raw.baseAddress, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4,
                            space: CGColorSpace(name: CGColorSpace.sRGB)!,
                            bitmapInfo: (alpha ? CGImageAlphaInfo.premultipliedFirst : CGImageAlphaInfo.noneSkipFirst).rawValue |
                                CGBitmapInfo.byteOrder32Little.rawValue)
        return ctx?.makeImage()
    }
    guard let image, let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil) else {
        throw TrailerError("cannot write \(url.path)")
    }
    CGImageDestinationAddImage(dest, image, nil)
    if !CGImageDestinationFinalize(dest) { throw TrailerError("cannot write \(url.path)") }
}
