// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The edit list (tools/trailer/README.md): the shots of the trailer, in order, rendered one by one.

import Foundation
import simd

struct Keyframe {
    var t: Double
    var position: SIMD3<Float>
    var target: SIMD3<Float>
}

struct Shot {
    var index = 0
    var type = "portal" // portal, head, compare, wiggle, title, clip
    var name = ""
    var capture: URL?
    var start: Double = 0 // seconds into the capture
    var duration: Double = 4
    var speed: Double = 1
    var camera = "dolly" // a preset, or keyframes
    var keyframes: [Keyframe] = []
    var fovY: Float = 45 // degrees
    var ease = true
    var smooth: Double = 0.35 // head: seconds of smoothing on each side
    var hz: Double = 4 // wiggle: eye swaps per second
    var options = SceneOptions()
    var caption: String?
    var captionDetail: String?
    var captionAt: Double = 0.4
    var kicker: String?
    var title = ""
    var subtitle: String?
    var file: URL? // clip
    var fadeIn: Double = 0.25, fadeOut: Double = 0.25
    var volume: Double = 1
    var labels = ["Flat", "Volumetric"] // compare
    var zoom: Double = 1.06 // image: how far it pushes in
    var focus = SIMD2<Double>(0.5, 0.5) // image: toward where (0..1 of the frame)
}

struct EditList {
    var base = URL(fileURLWithPath: ".")
    var capture: URL?
    var output = URL(fileURLWithPath: "out")
    var width = 1920, height = 1080
    var fps: Double = 60
    var codec = Codec.prores
    var audioOffset: Double = 0
    var music: AudioSource?
    var shots: [Shot] = []
    var gunModel = URL(fileURLWithPath: "lightgun.glb")

    init(url: URL) throws {
        let base = url.deletingLastPathComponent()
        self.base = base
        let data = try Data(contentsOf: url)
        guard let json = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw TrailerError("\(url.path): not a JSON object")
        }
        func path(_ any: Any?) -> URL? {
            guard let s = any as? String else { return nil }
            let expanded = (s as NSString).expandingTildeInPath
            return expanded.hasPrefix("/") ? URL(fileURLWithPath: expanded) : base.appendingPathComponent(expanded)
        }
        capture = path(json["capture"])
        output = path(json["output"]) ?? base.appendingPathComponent("out")
        if let size = json["size"] as? [Int], size.count == 2 { width = size[0]; height = size[1] }
        fps = json["fps"] as? Double ?? fps
        codec = Codec(rawValue: json["codec"] as? String ?? "prores") ?? .prores
        if let audio = json["audio"] as? [String: Any] { audioOffset = audio["offset"] as? Double ?? 0 }
        if let music = json["music"] as? [String: Any], let file = path(music["file"]) {
            self.music = AudioSource(file: file, start: music["start"] as? Double ?? 0, volume: music["volume"] as? Double ?? 0.6)
        }
        let repo = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../../../../assets/lightgun.glb").standardized
        gunModel = path(json["gun_model"]) ?? repo

        for (i, any) in ((json["shots"] as? [[String: Any]]) ?? []).enumerated() {
            var s = Shot()
            s.index = i + 1
            s.type = any["type"] as? String ?? "portal"
            s.name = any["name"] as? String ?? s.type
            s.capture = path(any["capture"]) ?? capture
            s.start = any["start"] as? Double ?? 0
            s.duration = any["duration"] as? Double ?? (s.type == "clip" ? 0 : 4) // a clip: all of it
            s.speed = any["speed"] as? Double ?? 1
            s.fovY = Float(any["fov"] as? Double ?? 45)
            s.ease = any["ease"] as? Bool ?? true
            s.smooth = any["smooth"] as? Double ?? s.smooth
            s.hz = any["hz"] as? Double ?? s.hz
            s.caption = any["caption"] as? String
            s.captionDetail = any["detail"] as? String
            s.captionAt = any["caption_at"] as? Double ?? s.captionAt
            s.kicker = any["kicker"] as? String
            s.title = any["title"] as? String ?? ""
            s.subtitle = any["subtitle"] as? String
            s.file = path(any["file"])
            s.fadeIn = any["fade_in"] as? Double ?? any["fade"] as? Double ?? s.fadeIn
            s.fadeOut = any["fade_out"] as? Double ?? any["fade"] as? Double ?? s.fadeOut
            s.volume = any["volume"] as? Double ?? 1
            if let labels = any["labels"] as? [String], labels.count == 2 { s.labels = labels }
            s.zoom = any["zoom"] as? Double ?? s.zoom
            if let f = any["focus"] as? [Double], f.count == 2 { s.focus = SIMD2(f[0], f[1]) }
            if let camera = any["camera"] as? String {
                s.camera = camera
            } else if let keys = any["camera"] as? [[String: Any]] {
                s.camera = "keyframes"
                s.keyframes = keys.enumerated().map { j, k in
                    let p = (k["pos"] as? [Double]) ?? [0, 0, 2]
                    let l = (k["look"] as? [Double]) ?? [0, 0, 0]
                    return Keyframe(t: k["t"] as? Double ?? Double(j) / Double(max(keys.count - 1, 1)),
                                    position: SIMD3(Float(p[0]), Float(p[1]), Float(p[2])),
                                    target: SIMD3(Float(l[0]), Float(l[1]), Float(l[2])))
                }
            }
            if s.type == "reveal" {
                s.options.clip = false
                s.options.glass = true
                if any["camera"] == nil { s.camera = "reveal" }
            }
            var o = s.options
            o.clip = any["clip"] as? Bool ?? o.clip
            o.fill = any["fill"] as? Bool ?? o.fill
            o.outline = any["outline"] as? Bool ?? o.outline
            o.floor = any["floor"] as? Bool ?? o.floor
            o.glass = any["glass"] as? Bool ?? o.glass
            o.guns = any["guns"] as? Bool ?? o.guns
            o.flatten = any["flat"] as? Bool ?? o.flatten
            o.step = Float(any["step"] as? Double ?? Double(o.step))
            o.edge = Float(any["edge"] as? Double ?? Double(o.edge))
            o.eye = any["eye"] as? Int ?? o.eye
            o.ui = any["ui"] as? Bool ?? o.ui
            o.floorHeight = Float(any["floor_height"] as? Double ?? Double(o.floorHeight))
            s.options = o
            shots.append(s)
        }
    }
}

final class ShotRenderer {
    let list: EditList
    let renderer: Renderer
    private var captures: [URL: Capture] = [:]

    init(list: EditList) throws {
        self.list = list
        renderer = try Renderer(width: list.width, height: list.height, gunModel: list.gunModel)
        if renderer.gun == nil { print("warning: no gun model at \(list.gunModel.path); guns are not drawn") }
    }

    func capture(_ url: URL?) throws -> Capture {
        guard let url else { throw TrailerError("the shot names no capture") }
        if let c = captures[url] { return c }
        let c = try Capture(url)
        captures[url] = c
        return c
    }

    // MARK: Cameras

    // A preset's path, in the screen's frame, for a screen `w` meters wide.
    static func preset(_ name: String, screenWidth w: Float) -> [Keyframe]? {
        func k(_ t: Double, _ p: SIMD3<Float>, _ l: SIMD3<Float>) -> Keyframe { Keyframe(t: t, position: p * w, target: l * w) }
        func orbit(_ a0: Float, _ a1: Float, radius: Float, centre: SIMD3<Float>, height: Float) -> [Keyframe] {
            (0...8).map { i in
                let t = Float(i) / 8, a = (a0 + (a1 - a0) * t) * .pi / 180
                return k(Double(t), centre + SIMD3(sin(a) * radius, height, cos(a) * radius), centre)
            }
        }
        switch name {
        case "front": return [k(0, SIMD3(0, 0, 1.15), SIMD3(0, 0, 0)), k(1, SIMD3(0, 0, 1.15), SIMD3(0, 0, 0))]
        case "dolly": return [k(0, SIMD3(-0.28, 0.03, 0.9), SIMD3(0, 0, -0.25)), k(1, SIMD3(0.28, 0.03, 0.9), SIMD3(0, 0, -0.25))]
        case "dolly-back": return [k(0, SIMD3(0.28, 0.03, 0.9), SIMD3(0, 0, -0.25)), k(1, SIMD3(-0.28, 0.03, 0.9), SIMD3(0, 0, -0.25))]
        case "push": return [k(0, SIMD3(0, 0.02, 1.25), SIMD3(0, 0, -0.4)), k(1, SIMD3(0, 0, 0.45), SIMD3(0, 0, -0.6))]
        case "lean": return [k(0, SIMD3(0.3, 0.05, 0.7), SIMD3(0, 0, -0.6)), k(0.5, SIMD3(0, 0.02, 0.62), SIMD3(0, 0, -0.6)),
                             k(1, SIMD3(-0.3, -0.02, 0.7), SIMD3(0, 0, -0.6))]
        case "orbit": return orbit(-28, 28, radius: 1.05, centre: SIMD3(0, 0, -0.15), height: 0.04)
        case "reveal": return orbit(0, 35, radius: 1.15, centre: SIMD3(0, 0, -0.45), height: 0.1)
        // The menus: the screen with the panel and the buttons beside it; the buttons up close.
        case "menu": return [k(0, SIMD3(0.1, 0.03, 0.95), SIMD3(0.06, 0, 0)), k(1, SIMD3(0.03, 0.02, 0.8), SIMD3(0.05, 0, 0))]
        case "buttons": return [k(0, SIMD3(0.36, 0.06, 0.62), SIMD3(0.5, -0.02, 0)), k(1, SIMD3(0.42, 0.03, 0.5), SIMD3(0.52, -0.03, 0))]
        default: return nil
        }
    }

    static func along(_ keys: [Keyframe], _ t: Double) -> (SIMD3<Float>, SIMD3<Float>) {
        guard keys.count > 1 else { return (keys[0].position, keys[0].target) }
        var i = 0
        while i < keys.count - 2 && t > keys[i + 1].t { i += 1 }
        let a = keys[i], b = keys[i + 1]
        let u = Float(min(max((t - a.t) / max(b.t - a.t, 1e-6), 0), 1))
        let p0 = keys[max(i - 1, 0)], p3 = keys[min(i + 2, keys.count - 1)]
        return (catmullRom(p0.position, a.position, b.position, p3.position, u),
                catmullRom(p0.target, a.target, b.target, p3.target, u))
    }

    // The player's own view, smoothed: the eyes' midpoint and the head's direction, averaged over a
    // window around the moment.
    func headCamera(_ capture: Capture, at time: Double, toScreen: Pose, shot: Shot) -> Camera? {
        var position = SIMD3<Float>(repeating: 0)
        var rotation = SIMD4<Float>(repeating: 0)
        var total: Float = 0
        var reference: simd_quatf?
        let n = 16
        for i in -n...n {
            let dt = shot.smooth * Double(i) / Double(n)
            guard let s = capture.pose(at: time + dt), let head = s.head else { continue }
            let w = Float(exp(-2 * pow(Double(i) / Double(n), 2)))
            let eyes = s.eyes.map { ($0[0].position + $0[1].position) * 0.5 } ?? head.position
            position += toScreen.apply(eyes) * w
            var q = (toScreen * head).rotation.vector
            if let reference, simd_dot(reference.vector, q) < 0 { q = -q }
            if reference == nil { reference = simd_quatf(vector: q) }
            rotation += q * w
            total += w
        }
        guard total > 0 else { return nil }
        let pose = Pose(position / total, simd_normalize(simd_quatf(vector: rotation / total)))
        let proj = perspective(fovY: shot.fovY * .pi / 180, aspect: renderer.aspect, near: 0.03, far: 300)
        return Camera(position: pose.position, viewProj: proj * viewFromPose(pose))
    }

    // Over the shoulder of the first gun: behind and above it, looking down its beam. Fixed for the
    // shot (from where the gun was, on average), easing in a little.
    private var gunCameras: [Int: [Keyframe]] = [:]
    func gunKeyframes(_ shot: Shot, capture: Capture) -> [Keyframe]? {
        if let keys = gunCameras[shot.index] { return keys }
        var gun = SIMD3<Float>(repeating: 0), aim = SIMD3<Float>(repeating: 0)
        var n: Float = 0
        for i in 0...10 {
            let t = shot.start + shot.duration * shot.speed * Double(i) / 10
            guard let s = capture.pose(at: t), let g = s.guns.first(where: { $0.visible }) else { continue }
            let toScreen = capture.screenPose(at: t).inverse
            gun += toScreen.apply(g.pose.position)
            aim += toScreen.apply(g.beamEnd)
            n += 1
        }
        guard n > 0 else { return nil }
        gun /= n
        aim /= n
        let target = gun + (aim - gun) * 0.7
        let keys = [Keyframe(t: 0, position: gun + SIMD3(0.30, 0.17, 0.45), target: target),
                    Keyframe(t: 1, position: gun + SIMD3(0.17, 0.11, 0.30), target: target)]
        gunCameras[shot.index] = keys
        return keys
    }

    // MARK: Rendering

    func render(_ shot: Shot, to url: URL, preview: Bool = false) throws {
        print(shot.duration > 0 ? String(format: "shot %d (%@): %.1f s -> %@", shot.index, shot.name, shot.duration, url.lastPathComponent) :
              String(format: "shot %d (%@): the whole clip -> %@", shot.index, shot.name, url.lastPathComponent))
        switch shot.type {
        case "clip":
            guard let file = shot.file else { throw TrailerError("clip shot \(shot.index) has no file") }
            var captionFile: URL?
            if let text = shot.caption {
                let bitmap = caption(width: list.width, height: list.height, text: text, detail: shot.captionDetail)
                captionFile = url.deletingPathExtension().appendingPathExtension("caption.png")
                try writePNG(bitmap.pixels, width: list.width, height: list.height, to: captionFile!, alpha: true)
            }
            try conformClip(file, to: url, width: list.width, height: list.height, fps: list.fps, start: shot.start,
                            duration: shot.duration > 0 ? shot.duration : nil, codec: list.codec, fadeIn: shot.fadeIn,
                            fadeOut: shot.fadeOut, volume: shot.volume, caption: captionFile, captionAt: shot.captionAt)
            if let captionFile { try? FileManager.default.removeItem(at: captionFile) }
            return
        case "image":
            guard let file = shot.file else { throw TrailerError("image shot \(shot.index) has no file") }
            var captionFile: URL?
            if let text = shot.caption {
                let bitmap = caption(width: list.width, height: list.height, text: text, detail: shot.captionDetail)
                captionFile = url.deletingPathExtension().appendingPathExtension("caption.png")
                try writePNG(bitmap.pixels, width: list.width, height: list.height, to: captionFile!, alpha: true)
            }
            try imageShot(file, to: url, width: list.width, height: list.height, fps: list.fps, duration: shot.duration,
                          zoom: shot.zoom, focus: shot.focus, codec: list.codec, fadeIn: shot.fadeIn, fadeOut: shot.fadeOut,
                          caption: captionFile, captionAt: shot.captionAt)
            if let captionFile { try? FileManager.default.removeItem(at: captionFile) }
            return
        case "title":
            let card = titleCard(width: list.width, height: list.height, kicker: shot.kicker, title: shot.title, subtitle: shot.subtitle)
            let writer = try VideoWriter(url: url, width: list.width, height: list.height, fps: list.fps, duration: shot.duration,
                                         codec: list.codec, audio: nil, fadeIn: shot.fadeIn, fadeOut: shot.fadeOut)
            let frames = Int((shot.duration * list.fps).rounded())
            for _ in 0..<frames { writer.write(card.pixels) }
            try writer.finish()
            return
        default:
            break
        }

        let capture = try capture(shot.capture)
        var audio: AudioSource?
        if let wav = capture.audio, abs(shot.speed - 1) < 1e-6 {
            audio = AudioSource(file: wav, start: shot.start - capture.audioStart + list.audioOffset, volume: shot.volume)
        }
        let writer = try VideoWriter(url: url, width: list.width, height: list.height, fps: list.fps, duration: shot.duration,
                                     codec: list.codec, audio: audio, fadeIn: shot.fadeIn, fadeOut: shot.fadeOut)
        let frames = Int((shot.duration * list.fps).rounded())
        let captionBitmap = shot.caption.map { caption(width: list.width, height: list.height, text: $0, detail: shot.captionDetail) }
        let labels = shot.type == "compare" ? shot.labels.enumerated().map { i, label in
            caption(width: list.width, height: list.height, text: label, detail: nil,
                    at: CGPoint(x: CGFloat(list.width) * (i == 0 ? 0.04 : 0.54), y: CGFloat(list.height) * 0.88), size: 40)
        } : []
        let started = Date()
        for n in 0..<frames {
            let tau = Double(n) / list.fps
            var pixels = try renderFrame(shot, capture: capture, tau: tau)
            for label in labels { label.composite(over: &pixels, alpha: 1) }
            if let captionBitmap {
                let a = (tau - shot.captionAt) / 0.35
                let b = (shot.duration - shot.fadeOut - 0.1 - tau) / 0.35
                captionBitmap.composite(over: &pixels, alpha: Float(min(max(min(a, b), 0), 1)))
            }
            writer.write(pixels)
            if n % 60 == 59 {
                let rate = Double(n + 1) / Date().timeIntervalSince(started)
                print(String(format: "  %d/%d frames (%.1f fps)", n + 1, frames, rate))
            }
        }
        try writer.finish()
    }

    // One output frame of a capture shot, `tau` seconds into it.
    func renderFrame(_ shot: Shot, capture: Capture, tau: Double) throws -> [UInt8] {
        let time = shot.start + tau * shot.speed
        let index = capture.frameIndex(at: time)
        let frame = try capture.frame(index)
        let pose = capture.pose(at: time)
        let toScreen = capture.screenPose(at: time).inverse
        var flash = [false, false]
        if let pose {
            let before = capture.pose(at: time - 0.06)
            for g in 0..<2 { flash[g] = pose.guns[g].fire && !(before?.guns[g].fire ?? false) }
        }
        var scene = Renderer.Scene(frame: frame, frameIndex: index, pose: pose, toScreen: toScreen, flash: flash)
        scene.showPicture = frame.header.time <= time + 0.25 // not a frame from later on (before the game started)
        scene.capture = capture

        if shot.type == "wiggle" {
            let eye = Int(tau * shot.hz * 2) % 2
            return wiggleFrame(frame, eye: eye)
        }
        let progress = shot.ease ? smoothstep(tau / max(shot.duration, 1e-6)) : tau / max(shot.duration, 1e-6)
        let camera: Camera
        if shot.type == "head", let head = headCamera(capture, at: time, toScreen: toScreen, shot: shot) {
            camera = head
        } else {
            let fallback = ShotRenderer.preset("dolly", screenWidth: frame.header.screenWidth)!
            let keys = shot.camera == "keyframes" ? shot.keyframes :
                shot.camera == "gun" ? (gunKeyframes(shot, capture: capture) ?? fallback) :
                (ShotRenderer.preset(shot.camera, screenWidth: frame.header.screenWidth) ?? fallback)
            let (position, target) = ShotRenderer.along(keys, progress)
            camera = Camera.look(from: position, at: target, fovY: shot.fovY * .pi / 180, aspect: renderer.aspect)
        }
        if shot.type == "compare" {
            var flat = shot.options
            flat.flatten = true
            let left = try renderer.render(scene, camera: camera, options: flat)
            var right = try renderer.render(scene, camera: camera, options: shot.options)
            let w = list.width, half = w / 2
            for y in 0..<list.height {
                let row = y * w * 4
                right.withUnsafeMutableBufferPointer { dst in
                    left.withUnsafeBufferPointer { src in
                        for x in 0..<half {
                            let i = row + x * 4
                            dst[i] = src[i]; dst[i + 1] = src[i + 1]; dst[i + 2] = src[i + 2]
                        }
                        for x in (half - 2)..<(half + 2) {
                            let i = row + x * 4
                            dst[i] = 230; dst[i + 1] = 230; dst[i + 2] = 230
                        }
                    }
                }
            }
            return right
        }
        return try renderer.render(scene, camera: camera, options: shot.options)
    }

    // One eye's picture as captured, scaled to fit the frame (no rebuilding: what that eye saw).
    func wiggleFrame(_ frame: DecodedFrame, eye: Int) -> [UInt8] {
        let src = eye == 1 ? (frame.right ?? frame.left) : frame.left
        let h = frame.header
        let r = h.drawRect
        let sx0 = Int(r.x), sy0 = Int(r.y), sw = max(Int(r.z - r.x), 1), sh = max(Int(r.w - r.y), 1)
        let W = list.width, H = list.height
        let scale = min(Double(W) / Double(sw), Double(H) / Double(sh))
        let dw = Int(Double(sw) * scale), dh = Int(Double(sh) * scale)
        let ox = (W - dw) / 2, oy = (H - dh) / 2
        var out = [UInt8](repeating: 0, count: W * H * 4)
        for i in stride(from: 0, to: out.count, by: 4) { out[i] = 5; out[i + 1] = 4; out[i + 2] = 3; out[i + 3] = 255 }
        src.withUnsafeBufferPointer { s in
            out.withUnsafeMutableBufferPointer { d in
                for y in 0..<dh {
                    let fy = (Double(y) + 0.5) / scale - 0.5
                    let y0 = min(max(Int(fy), 0), sh - 1), y1 = min(y0 + 1, sh - 1)
                    let wy = Float(fy - Double(Int(fy)))
                    for x in 0..<dw {
                        let fx = (Double(x) + 0.5) / scale - 0.5
                        let x0 = min(max(Int(fx), 0), sw - 1), x1 = min(x0 + 1, sw - 1)
                        let wx = Float(fx - Double(Int(fx)))
                        let o = ((oy + y) * W + ox + x) * 4
                        for c in 0..<3 {
                            func at(_ xx: Int, _ yy: Int) -> Float { Float(s[((sy0 + yy) * h.width + sx0 + xx) * 4 + c]) }
                            let v = (at(x0, y0) * (1 - wx) + at(x1, y0) * wx) * (1 - wy) + (at(x0, y1) * (1 - wx) + at(x1, y1) * wx) * wy
                            d[o + 2 - c] = UInt8(min(max(v, 0), 255)) // RGBA in, BGRA out
                        }
                    }
                }
            }
        }
        return out
    }
}
