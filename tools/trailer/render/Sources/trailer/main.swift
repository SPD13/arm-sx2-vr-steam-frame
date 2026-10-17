// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// trailer: renders trailer shots from a capture made on the headset (tools/trailer/README.md).

import Foundation
import simd

let usage = """
Usage:
  trailer info CAPTURE                 What a capture holds: takes, 3D mode, light gun shots, games
  trailer render EDIT.json [--only N[,N...]] [--no-assemble]
                                       Renders the edit list's shots (OUTPUT/shots/) and the cut (OUTPUT/trailer.mp4)
  trailer still EDIT.json SHOT T OUT.png
                                       One frame of shot SHOT (1-based), T seconds into it
  trailer verify CAPTURE [TIME...]     Rebuilds the left eye's scene and renders it from the right eye: how
                                       close it comes to the right eye's real picture (PSNR, dB)
"""

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data("trailer: \(message)\n".utf8))
    exit(1)
}

func fileURL(_ path: String) -> URL {
    URL(fileURLWithPath: (path as NSString).expandingTildeInPath)
}

func info(_ capture: Capture) {
    let frames = capture.frames
    print("Capture \(capture.dir.path)")
    if let synthetic = capture.summary["synthetic"] as? Bool, synthetic { print("  (synthetic)") }
    let first = frames.first!.header
    print(String(format: "  %d frames over %.1f s, %dx%d pictures, depth %dx%d, %@",
                 first.time.isFinite ? frames.count : 0, capture.duration, first.width, first.height, first.depthWidth, first.depthHeight,
                 capture.summary["complete"] as? Bool == true ? "complete" : "NOT closed properly (the app did not exit cleanly)"))
    let dropped = frames.reduce(0) { $0 + $1.header.droppedBefore }
    if dropped > 0 { print("  \(dropped) frames dropped while capturing (the writers fell behind)") }
    print(capture.audio != nil ? String(format: "  sound: audio.wav, starting at %.2f s", capture.audioStart) : "  sound: none")

    if !first.time.isFinite {
        print(String(format: "  menus only, no game frames: %.1f s of headset frames", capture.duration))
    } else {
    // Takes: stretches without a gap of more than a second (a menu pauses the game).
    print("  Takes:")
    var takeStart = 0
    func printTake(_ a: Int, _ b: Int) {
        let ha = frames[a].header, hb = frames[b].header
        var modes: [String] = []
        for i in a...b where modes.last != frames[i].header.mode.name { modes.append(frames[i].header.mode.name) }
        let depth = frames[a...b].filter { $0.header.depthWidth > 0 }.count
        print(String(format: "    %7.2f - %7.2f s  (%5.1f s)  %@, depth in %d%% of frames", ha.time, hb.time, hb.time - ha.time,
                     modes.joined(separator: " > "), 100 * depth / max(b - a + 1, 1)))
    }
    for i in 1..<frames.count where frames[i].header.time - frames[i - 1].header.time > 1.0 {
        printTake(takeStart, i - 1)
        takeStart = i
    }
    printTake(takeStart, frames.count - 1)
    }

    for e in capture.events { print(String(format: "  %7.2f s  %@: %@", e.time, e.name, e.value)) }
    var shots: [Double] = []
    var gunOn: [(Double, Bool)] = []
    var last = [false, false]
    var lastGun = false
    for s in capture.poses {
        if s.gunMode != lastGun { gunOn.append((s.time, s.gunMode)); lastGun = s.gunMode }
        for g in 0..<2 {
            if s.guns[g].fire && !last[g] { shots.append(s.time) }
            last[g] = s.guns[g].fire
        }
    }
    for (t, on) in gunOn { print(String(format: "  %7.2f s  light gun mode %@", t, on ? "on" : "off")) }
    if !shots.isEmpty {
        print("  Light gun shots (s): " + shots.map { String(format: "%.1f", $0) }.joined(separator: " "))
    }
    var menu = false
    for s in capture.poses where s.menu != menu {
        menu = s.menu
        print(String(format: "  %7.2f s  menu %@", s.time, menu ? "open" : "closed"))
    }
}

func verify(_ capture: Capture, times: [Double]) throws {
    let first = capture.frames[0].header
    let renderer = try Renderer(width: first.width, height: first.height, gunModel: nil)
    let samples = times.isEmpty ? stride(from: 0.0, through: capture.duration, by: max(capture.duration / 5, 0.1)).map { $0 } : times
    var options = SceneOptions()
    options.floor = false
    options.outline = false
    options.guns = false
    print("   time   right vs left (no rebuild)   rebuilt from left, seen from right")
    for t in samples {
        let index = capture.frameIndex(at: t)
        let frame = try capture.frame(index)
        let h = frame.header
        guard let right = frame.right, h.width == renderer.width, h.height == renderer.height else {
            print(String(format: "%7.2f   (no right picture: flat)", t))
            continue
        }
        let eye = SIMD3(h.eye[1].x, h.eye[1].y, h.eye[1].z)
        let camera = Camera(position: eye, viewProj: windowViewProjection(eye: eye, screen: SIMD2(h.screenWidth, h.screenHeight)))
        let scene = Renderer.Scene(frame: frame, frameIndex: index, pose: nil, toScreen: Pose())
        let rebuilt = try renderer.render(scene, camera: camera, options: options)
        func psnr(_ bgra: [UInt8], isBGRA: Bool) -> Double {
            var sum = 0.0, n = 0
            // Inside the picture, away from its edges (where the left eye saw nothing to rebuild from).
            let r = h.drawRect
            let mx = Int((r.z - r.x) * 0.06), my = Int((r.w - r.y) * 0.06)
            for y in (Int(r.y) + my)..<(Int(r.w) - my) {
                for x in (Int(r.x) + mx)..<(Int(r.z) - mx) {
                    let i = (y * h.width + x) * 4
                    for c in 0..<3 {
                        let a = Double(right[i + c])
                        let b = Double(bgra[i + (isBGRA ? 2 - c : c)])
                        sum += (a - b) * (a - b)
                        n += 1
                    }
                }
            }
            let mse = sum / Double(max(n, 1))
            return mse == 0 ? 99 : 10 * log10(255 * 255 / mse)
        }
        print(String(format: "%7.2f   %6.2f dB                     %6.2f dB", h.time, psnr(frame.left, isBGRA: false), psnr(rebuilt, isBGRA: true)))
        if let dir = ProcessInfo.processInfo.environment["TRAILER_VERIFY_PNG"] {
            func bgra(_ rgba: [UInt8]) -> [UInt8] {
                var out = rgba
                for i in stride(from: 0, to: out.count, by: 4) { out.swapAt(i, i + 2) }
                return out
            }
            let stem = String(format: "%@/verify-%.2f", dir, h.time)
            try writePNG(rebuilt, width: h.width, height: h.height, to: fileURL(stem + "-rebuilt.png"))
            try writePNG(bgra(right), width: h.width, height: h.height, to: fileURL(stem + "-right.png"))
            try writePNG(bgra(frame.left), width: h.width, height: h.height, to: fileURL(stem + "-left.png"))
            if let (w, hh, depth) = renderer.lastDepthMap() {
                let far = max(depth.max() ?? 1, 0.01)
                var grey = [UInt8](repeating: 255, count: w * hh * 4)
                for i in 0..<(w * hh) {
                    let v = UInt8(min(max(255 * (1 - depth[i] / far), 0), 255)) // near is white
                    grey[i * 4] = v; grey[i * 4 + 1] = v; grey[i * 4 + 2] = v
                }
                try writePNG(grey, width: w, height: hh, to: fileURL(stem + "-depth.png"))
            }
        }
    }
}

let args = CommandLine.arguments
guard args.count >= 3 else { print(usage); exit(args.count == 2 && args[1] == "help" ? 0 : 1) }
do {
    switch args[1] {
    case "info":
        info(try Capture(fileURL(args[2])))
    case "verify":
        try verify(try Capture(fileURL(args[2])), times: args.dropFirst(3).compactMap { Double($0) })
    case "still":
        guard args.count >= 6, let n = Int(args[3]), let t = Double(args[4]) else { fail("still EDIT.json SHOT T OUT.png") }
        let list = try EditList(url: fileURL(args[2]))
        guard n >= 1 && n <= list.shots.count else { fail("no shot \(n) (the list has \(list.shots.count))") }
        let shots = try ShotRenderer(list: list)
        let shot = list.shots[n - 1]
        var pixels: [UInt8]
        switch shot.type {
        case "title": pixels = titleCard(width: list.width, height: list.height, kicker: shot.kicker, title: shot.title, subtitle: shot.subtitle).pixels
        case "clip": fail("a clip shot has no frames of its own")
        default:
            pixels = try shots.renderFrame(shot, capture: try shots.capture(shot.capture), tau: t)
            if let text = shot.caption {
                caption(width: list.width, height: list.height, text: text, detail: shot.captionDetail).composite(over: &pixels, alpha: 1)
            }
        }
        try writePNG(pixels, width: list.width, height: list.height, to: fileURL(args[5]))
        print("wrote \(args[5])")
    case "render":
        let list = try EditList(url: fileURL(args[2]))
        var only: Set<Int>?
        if let i = args.firstIndex(of: "--only"), i + 1 < args.count {
            only = Set(args[i + 1].split(separator: ",").compactMap { Int($0) })
        }
        let assembleCut = !args.contains("--no-assemble")
        let shotsDir = list.output.appendingPathComponent("shots")
        try FileManager.default.createDirectory(at: shotsDir, withIntermediateDirectories: true)
        let renderer = try ShotRenderer(list: list)
        var files: [URL] = []
        for shot in list.shots {
            let safe = shot.name.replacingOccurrences(of: "[^A-Za-z0-9_-]+", with: "-", options: .regularExpression)
            let url = shotsDir.appendingPathComponent(String(format: "%02d-%@.%@", shot.index, safe, list.codec.ext))
            if shot.type == "clip" || shot.type == "image", let file = shot.file, !FileManager.default.fileExists(atPath: file.path) {
                print("shot \(shot.index) (\(shot.name)): \(file.path) is missing; left out of the cut")
                continue
            }
            files.append(url)
            if let only, !only.contains(shot.index) { continue }
            try renderer.render(shot, to: url)
        }
        if assembleCut {
            let missing = files.filter { !FileManager.default.fileExists(atPath: $0.path) }
            if missing.isEmpty {
                let cut = list.output.appendingPathComponent("trailer.mp4")
                var music = list.music
                if let file = music?.file, !FileManager.default.fileExists(atPath: file.path) {
                    print("music: \(file.path) is missing; the cut has the game's sound only")
                    music = nil
                }
                try assemble(files, to: cut, music: music)
                print("cut: \(cut.path)")
            } else {
                print("not assembling: \(missing.count) shot(s) not rendered yet")
            }
        }
    default:
        print(usage)
        exit(1)
    }
} catch {
    fail("\(error)")
}
