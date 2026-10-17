// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// Reads a capture made on the headset with --capture (host/capture/CaptureFormat.h has the files).

import Compression
import Foundation
import simd

enum PlaneKind: UInt32 {
    case colorLeft = 1, colorRight = 2, depthLeft = 3, depthRight = 4
}

enum Mode3D: UInt32 {
    case flat = 0, volumetric = 1, volumetricPlus = 2

    var name: String {
        switch self {
        case .flat: return "Flat"
        case .volumetric: return "Volumetric"
        case .volumetricPlus: return "Volumetric+"
        }
    }
}

struct FrameHeader {
    static let size = 208

    var index: UInt32 = 0
    var time: Double = 0 // seconds since the capture started
    var realtimeNs: Int64 = 0
    var width = 0, height = 0
    var depthWidth = 0, depthHeight = 0
    var droppedBefore = 0
    var drawRect = SIMD4<Float>(0, 0, 0, 0)
    var mode = Mode3D.flat
    var convergence: Float = 0.12, stereoDepth: Float = 3
    var screenWidth: Float = 2.4, screenHeight: Float = 1.8, screenDistance: Float = 2.2
    // Per eye: x right, y up, distance in front of the glass (m, the screen's frame), depth factor.
    var eye = [SIMD4<Float>](repeating: .zero, count: 2)
    var distanceRatio = SIMD2<Float>(1, 1)
    struct Plane {
        var kind: UInt32
        var rawSize: Int
        var packedSize: Int
        var offset: Int // in the file
    }
    var planes: [Plane] = []

    init() {}

    init?(data: Data) {
        guard data.count >= FrameHeader.size, data.prefix(8) == Data("PS2VCAP1".utf8) else { return nil }
        func u32(_ o: Int) -> UInt32 { data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: o, as: UInt32.self) } }
        func i64(_ o: Int) -> Int64 { data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: o, as: Int64.self) } }
        func f32(_ o: Int) -> Float { data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: o, as: Float.self) } }
        let headerSize = Int(u32(8))
        index = u32(12)
        time = Double(i64(16)) / 1e9
        realtimeNs = i64(24)
        width = Int(u32(32)); height = Int(u32(36))
        depthWidth = Int(u32(40)); depthHeight = Int(u32(44))
        droppedBefore = Int(u32(52))
        drawRect = SIMD4(f32(56), f32(60), f32(64), f32(68))
        mode = Mode3D(rawValue: u32(72)) ?? .flat
        convergence = f32(76); stereoDepth = f32(80)
        screenWidth = f32(84); screenHeight = f32(88); screenDistance = f32(92)
        for e in 0..<2 {
            eye[e] = SIMD4(f32(96 + e * 16), f32(100 + e * 16), f32(104 + e * 16), f32(108 + e * 16))
        }
        distanceRatio = SIMD2(f32(128), f32(132))
        let count = min(Int(u32(136)), 4)
        var offset = headerSize
        for p in 0..<count {
            let base = 144 + p * 16
            let plane = Plane(kind: u32(base), rawSize: Int(u32(base + 4)), packedSize: Int(u32(base + 8)), offset: offset)
            offset += plane.packedSize
            planes.append(plane)
        }
    }
}

// A frame's pictures and depth, decoded.
final class DecodedFrame {
    let header: FrameHeader
    var left: [UInt8] = [] // RGBA8, sRGB-encoded
    var right: [UInt8]? // nil in flat
    var depthLeft: [Float]? // normalised Z, -1 outside the picture
    var depthRight: [Float]?

    init(header: FrameHeader) { self.header = header }
}

struct Pose {
    var position = SIMD3<Float>(0, 0, 0)
    var rotation = simd_quatf(ix: 0, iy: 0, iz: 0, r: 1)

    init() {}
    init(_ position: SIMD3<Float>, _ rotation: simd_quatf) {
        self.position = position
        self.rotation = rotation
    }
    init?(_ any: Any?) {
        guard let a = any as? [Double], a.count == 7 else { return nil }
        position = SIMD3(Float(a[0]), Float(a[1]), Float(a[2]))
        rotation = simd_normalize(simd_quatf(ix: Float(a[3]), iy: Float(a[4]), iz: Float(a[5]), r: Float(a[6])))
    }

    var inverse: Pose {
        let r = rotation.inverse
        return Pose(r.act(-position), r)
    }
    static func * (a: Pose, b: Pose) -> Pose {
        Pose(a.position + a.rotation.act(b.position), a.rotation * b.rotation)
    }
    func apply(_ p: SIMD3<Float>) -> SIMD3<Float> { position + rotation.act(p) }

    static func mix(_ a: Pose, _ b: Pose, _ t: Float) -> Pose {
        Pose(simd_mix(a.position, b.position, SIMD3(repeating: t)), simd_slerp(a.rotation, b.rotation, t))
    }
}

struct GunSample {
    var visible = false
    var pose = Pose()
    var color = SIMD3<Float>(0.55, 0.55, 0.57) // sRGB
    var trigger: Float = 0, pressLeft: Float = 0, pressRight: Float = 0
    var beam = false, dot = false
    var beamEnd = SIMD3<Float>(0, 0, 0)
    var beamColor = SIMD3<Float>(1, 0.2, 0.2)
    var fire = false
}

struct PoseSample {
    var time: Double = 0
    var head: Pose?
    var eyes: [Pose]? // left, right
    var eyeFov: [SIMD4<Float>] = [] // left, right, up, down (radians)
    var screen: Pose?
    var screenSize = SIMD2<Float>(2.4, 1.8)
    var mode = Mode3D.flat
    var passthrough = false
    var menu = false
    var gunMode = false
    var guns = [GunSample(), GunSample()]
}

func parseHexColor(_ any: Any?) -> SIMD3<Float>? {
    guard let s = any as? String, let v = UInt32(s, radix: 16) else { return nil }
    return SIMD3(Float((v >> 16) & 0xFF), Float((v >> 8) & 0xFF), Float(v & 0xFF)) / 255
}

func vec3(_ any: Any?) -> SIMD3<Float>? {
    guard let a = any as? [Double], a.count == 3 else { return nil }
    return SIMD3(Float(a[0]), Float(a[1]), Float(a[2]))
}

final class Capture {
    let dir: URL
    private(set) var frames: [(url: URL, header: FrameHeader)] = []
    private(set) var poses: [PoseSample] = []
    private(set) var events: [(time: Double, name: String, value: String)] = []
    private(set) var summary: [String: Any] = [:]
    // The game's sound, and where it starts on the capture's clock (seconds).
    private(set) var audio: URL?
    private(set) var audioStart: Double = 0

    private var cache: (index: Int, frame: DecodedFrame)?

    init(_ dir: URL) throws {
        self.dir = dir
        if let data = try? Data(contentsOf: dir.appendingPathComponent("capture.json")),
           let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            summary = json
        } else {
            throw TrailerError("\(dir.path) has no capture.json: not a capture folder")
        }
        let frameDir = dir.appendingPathComponent("frames")
        let names = try FileManager.default.contentsOfDirectory(atPath: frameDir.path).filter { $0.hasSuffix(".bin") }.sorted()
        for name in names {
            let url = frameDir.appendingPathComponent(name)
            guard let handle = try? FileHandle(forReadingFrom: url) else { continue }
            let data = handle.readData(ofLength: FrameHeader.size)
            try? handle.close()
            if let header = FrameHeader(data: data) {
                frames.append((url, header))
            }
        }
        frames.sort { $0.header.time < $1.header.time }
        if frames.isEmpty {
            throw TrailerError("\(frameDir.path) holds no frames")
        }

        if let text = try? String(contentsOf: dir.appendingPathComponent("poses.jsonl"), encoding: .utf8) {
            for line in text.split(separator: "\n") {
                guard let json = try? JSONSerialization.jsonObject(with: Data(line.utf8)) as? [String: Any],
                      let t = json["t"] as? Double else { continue }
                let time = t / 1e9
                if let name = json["event"] as? String {
                    events.append((time, name, json["value"] as? String ?? ""))
                    continue
                }
                var s = PoseSample()
                s.time = time
                s.head = Pose(json["head"])
                if let eyes = json["eyes"] as? [[String: Any]], eyes.count == 2,
                   let l = Pose(eyes[0]["p"]), let r = Pose(eyes[1]["p"]) {
                    s.eyes = [l, r]
                    s.eyeFov = eyes.map { e in
                        let f = (e["fov"] as? [Double]) ?? [-0.8, 0.8, 0.8, -0.8]
                        return SIMD4(Float(f[0]), Float(f[1]), Float(f[2]), Float(f[3]))
                    }
                }
                s.screen = Pose(json["screen"])
                if let w = json["sw"] as? Double, let h = json["sh"] as? Double {
                    s.screenSize = SIMD2(Float(w), Float(h))
                }
                s.mode = Mode3D(rawValue: UInt32((json["mode"] as? Int) ?? 0)) ?? .flat
                s.passthrough = (json["pt"] as? Int ?? 0) != 0
                s.menu = (json["menu"] as? Int ?? 0) != 0
                s.gunMode = (json["gun"] as? Int ?? 0) != 0
                if let guns = json["guns"] as? [[String: Any]] {
                    for (g, gun) in guns.prefix(2).enumerated() {
                        var sample = GunSample()
                        sample.visible = (gun["v"] as? Int ?? 0) != 0
                        if sample.visible, let pose = Pose(gun["p"]) {
                            sample.pose = pose
                            sample.color = parseHexColor(gun["c"]) ?? sample.color
                            sample.trigger = Float(gun["tr"] as? Double ?? 0)
                            sample.pressLeft = Float(gun["bl"] as? Double ?? 0)
                            sample.pressRight = Float(gun["br"] as? Double ?? 0)
                            sample.beam = (gun["beam"] as? Int ?? 0) != 0
                            sample.dot = (gun["dot"] as? Int ?? 0) != 0
                            sample.beamEnd = vec3(gun["end"]) ?? .zero
                            sample.beamColor = parseHexColor(gun["bc"]) ?? sample.beamColor
                            sample.fire = (gun["fire"] as? Int ?? 0) != 0
                        } else {
                            sample.visible = false
                        }
                        s.guns[g] = sample
                    }
                }
                poses.append(s)
            }
            poses.sort { $0.time < $1.time }
        }

        let wav = dir.appendingPathComponent("audio.wav")
        if FileManager.default.fileExists(atPath: wav.path) {
            audio = wav
            if let data = try? Data(contentsOf: dir.appendingPathComponent("audio.json")),
               let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
               let audioRt = (json["started_realtime_ns"] as? NSNumber)?.int64Value,
               let captureRt = (summary["started_realtime_ns"] as? NSNumber)?.int64Value {
                audioStart = Double(audioRt - captureRt) / 1e9
            }
        }
    }

    var duration: Double { frames.last!.header.time }

    // The newest frame shown at `time` (seconds on the capture's clock).
    func frameIndex(at time: Double) -> Int {
        var lo = 0, hi = frames.count - 1
        if time <= frames[0].header.time { return 0 }
        while lo < hi {
            let mid = (lo + hi + 1) / 2
            if frames[mid].header.time <= time { lo = mid } else { hi = mid - 1 }
        }
        return lo
    }

    func frame(_ index: Int) throws -> DecodedFrame {
        if let cache, cache.index == index { return cache.frame }
        let (url, header) = frames[index]
        let data = try Data(contentsOf: url)
        let frame = DecodedFrame(header: header)
        for plane in header.planes {
            let bytes = try Capture.decode(data, plane)
            switch PlaneKind(rawValue: plane.kind) {
            case .colorLeft: frame.left = bytes
            case .colorRight: frame.right = bytes
            case .depthLeft: frame.depthLeft = bytes.withUnsafeBytes { Array($0.bindMemory(to: Float.self)) }
            case .depthRight: frame.depthRight = bytes.withUnsafeBytes { Array($0.bindMemory(to: Float.self)) }
            case .none: break
            }
        }
        cache = (index, frame)
        return frame
    }

    private static func decode(_ data: Data, _ plane: FrameHeader.Plane) throws -> [UInt8] {
        guard plane.offset + plane.packedSize <= data.count else { throw TrailerError("truncated frame file") }
        if plane.packedSize == plane.rawSize {
            return [UInt8](data[plane.offset..<(plane.offset + plane.rawSize)])
        }
        var out = [UInt8](repeating: 0, count: plane.rawSize)
        let written = data.withUnsafeBytes { (src: UnsafeRawBufferPointer) -> Int in
            out.withUnsafeMutableBytes { dst in
                compression_decode_buffer(dst.baseAddress!.assumingMemoryBound(to: UInt8.self), plane.rawSize,
                                          src.baseAddress!.advanced(by: plane.offset).assumingMemoryBound(to: UInt8.self),
                                          plane.packedSize, nil, COMPRESSION_LZ4_RAW)
            }
        }
        guard written == plane.rawSize else { throw TrailerError("bad LZ4 plane (\(written) of \(plane.rawSize) bytes)") }
        return out
    }

    // The headset's state at `time`, between the two records around it.
    func pose(at time: Double) -> PoseSample? {
        guard !poses.isEmpty else { return nil }
        var lo = 0, hi = poses.count - 1
        if time <= poses[0].time { return poses[0] }
        if time >= poses[hi].time { return poses[hi] }
        while hi - lo > 1 {
            let mid = (lo + hi) / 2
            if poses[mid].time <= time { lo = mid } else { hi = mid }
        }
        let a = poses[lo], b = poses[hi]
        let t = Float((time - a.time) / max(b.time - a.time, 1e-9))
        var s = a
        s.time = time
        if let ha = a.head, let hb = b.head { s.head = Pose.mix(ha, hb, t) }
        if let ea = a.eyes, let eb = b.eyes { s.eyes = [Pose.mix(ea[0], eb[0], t), Pose.mix(ea[1], eb[1], t)] }
        if let sa = a.screen, let sb = b.screen, simd_distance(sa.position, sb.position) < 0.05 {
            s.screen = Pose.mix(sa, sb, t)
        }
        for g in 0..<2 where a.guns[g].visible && b.guns[g].visible {
            s.guns[g].pose = Pose.mix(a.guns[g].pose, b.guns[g].pose, t)
            s.guns[g].beamEnd = simd_mix(a.guns[g].beamEnd, b.guns[g].beamEnd, SIMD3(repeating: t))
            s.guns[g].trigger = a.guns[g].trigger + (b.guns[g].trigger - a.guns[g].trigger) * t
        }
        return s
    }

    // The screen's pose at `time` (the last placement), for moving the room into the screen's frame.
    func screenPose(at time: Double) -> Pose {
        if let s = pose(at: time)?.screen { return s }
        let h = frames[frameIndex(at: time)].header
        return Pose(SIMD3(0, 1.6, -h.screenDistance), simd_quatf(ix: 0, iy: 0, iz: 0, r: 1))
    }
}

struct TrailerError: Error, CustomStringConvertible {
    let description: String
    init(_ description: String) { self.description = description }
}
