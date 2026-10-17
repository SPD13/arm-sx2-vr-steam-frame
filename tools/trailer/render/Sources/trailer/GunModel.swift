// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The light gun's model, assets/lightgun.glb (tools/lightgun/README.md has what it holds): its parts
// as the host draws them (host/gun/GunModel.h), the shell in the gun's colour.

import Foundation
import Metal
import simd

final class GunModel {
    enum Part: String, CaseIterable {
        case body, trigger, button_left, button_right
    }

    struct Draw {
        var part: Part
        var node: SIMD3<Float> // the part's node translation in the model
        var vertexBuffer: MTLBuffer
        var indexBuffer: MTLBuffer
        var indexCount: Int
        var material: String
        var color: SIMD3<Float> // linear
        var roughness: Float
    }

    private(set) var draws: [Draw] = []
    private(set) var muzzle = SIMD3<Float>(0, 0.0905, -0.2066)

    init(url: URL, device: MTLDevice) throws {
        let data = try Data(contentsOf: url)
        func u32(_ o: Int) -> UInt32 { data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: o, as: UInt32.self) } }
        guard data.count > 20, u32(0) == 0x4654_6C67 else { throw TrailerError("\(url.path) is not a .glb") }
        let jsonLength = Int(u32(12))
        let jsonData = data.subdata(in: 20..<(20 + jsonLength))
        let binStart = 20 + jsonLength + 8
        guard let json = try JSONSerialization.jsonObject(with: jsonData) as? [String: Any],
              let nodes = json["nodes"] as? [[String: Any]],
              let meshes = json["meshes"] as? [[String: Any]],
              let accessors = json["accessors"] as? [[String: Any]],
              let views = json["bufferViews"] as? [[String: Any]] else {
            throw TrailerError("\(url.path): unexpected glTF")
        }
        let materials = json["materials"] as? [[String: Any]] ?? []

        func accessorBytes(_ index: Int) -> (offset: Int, count: Int, type: Int) {
            let a = accessors[index]
            let view = views[a["bufferView"] as! Int]
            let offset = binStart + (view["byteOffset"] as? Int ?? 0) + (a["byteOffset"] as? Int ?? 0)
            return (offset, a["count"] as! Int, a["componentType"] as! Int)
        }
        func floats3(_ index: Int) -> [SIMD3<Float>] {
            let (offset, count, _) = accessorBytes(index)
            return (0..<count).map { i in
                data.withUnsafeBytes { raw in
                    SIMD3(raw.loadUnaligned(fromByteOffset: offset + i * 12, as: Float.self),
                          raw.loadUnaligned(fromByteOffset: offset + i * 12 + 4, as: Float.self),
                          raw.loadUnaligned(fromByteOffset: offset + i * 12 + 8, as: Float.self))
                }
            }
        }
        func indices(_ index: Int) -> [UInt32] {
            let (offset, count, type) = accessorBytes(index)
            return (0..<count).map { i in
                data.withUnsafeBytes { raw in
                    type == 5125 ? raw.loadUnaligned(fromByteOffset: offset + i * 4, as: UInt32.self) :
                        UInt32(raw.loadUnaligned(fromByteOffset: offset + i * 2, as: UInt16.self))
                }
            }
        }

        for node in nodes {
            guard let name = node["name"] as? String else { continue }
            let t = (node["translation"] as? [Double]).map { SIMD3(Float($0[0]), Float($0[1]), Float($0[2])) } ?? .zero
            if name == "muzzle" {
                muzzle = t
                continue
            }
            guard let part = Part(rawValue: name), let meshIndex = node["mesh"] as? Int,
                  let primitives = meshes[meshIndex]["primitives"] as? [[String: Any]] else { continue }
            for primitive in primitives {
                guard let attributes = primitive["attributes"] as? [String: Int],
                      let positionIndex = attributes["POSITION"], let normalIndex = attributes["NORMAL"],
                      let indexIndex = primitive["indices"] as? Int else { continue }
                let positions = floats3(positionIndex), normals = floats3(normalIndex)
                var interleaved = [Float]()
                interleaved.reserveCapacity(positions.count * 6)
                for i in 0..<positions.count {
                    interleaved += [positions[i].x, positions[i].y, positions[i].z, normals[i].x, normals[i].y, normals[i].z]
                }
                let idx = indices(indexIndex)
                var materialName = "shell", color = SIMD3<Float>(0.05, 0.05, 0.05), roughness: Float = 0.5
                if let m = primitive["material"] as? Int, m < materials.count {
                    materialName = materials[m]["name"] as? String ?? materialName
                    if let pbr = materials[m]["pbrMetallicRoughness"] as? [String: Any] {
                        if let f = pbr["baseColorFactor"] as? [Double] { color = SIMD3(Float(f[0]), Float(f[1]), Float(f[2])) }
                        roughness = Float(pbr["roughnessFactor"] as? Double ?? 0.5)
                    }
                }
                guard let vb = device.makeBuffer(bytes: interleaved, length: interleaved.count * 4),
                      let ib = device.makeBuffer(bytes: idx, length: idx.count * 4) else { continue }
                draws.append(Draw(part: part, node: t, vertexBuffer: vb, indexBuffer: ib, indexCount: idx.count,
                                  material: materialName, color: color, roughness: roughness))
            }
        }
        if draws.isEmpty { throw TrailerError("\(url.path): no gun parts") }
    }

    // A part's pose in the model, moved as the gun's state says (host/gun/GunModel.h).
    func partPose(_ draw: Draw, gun: GunSample) -> Pose {
        var pose = Pose(draw.node, simd_quatf(ix: 0, iy: 0, iz: 0, r: 1))
        switch draw.part {
        case .trigger:
            pose.rotation = simd_quatf(angle: -12 * .pi / 180 * gun.trigger, axis: SIMD3(1, 0, 0))
        case .button_left:
            pose.position.x += 0.001 * gun.pressLeft
        case .button_right:
            pose.position.x -= 0.001 * gun.pressRight
        case .body:
            break
        }
        return pose
    }
}

func srgbToLinear(_ c: SIMD3<Float>) -> SIMD3<Float> {
    func f(_ v: Float) -> Float { v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4) }
    return SIMD3(f(c.x), f(c.y), f(c.z))
}
