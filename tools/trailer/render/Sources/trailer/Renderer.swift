// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// Draws the scene of one captured moment from any camera, in the screen's frame: the game's picture
// rebuilt in depth behind the window (Shaders.swift), the guns with their beams, a faint floor and
// the window's edge, over the dark void the headset shows.

import Foundation
import Metal
import simd

struct Params {
    var viewProj = matrix_identity_float4x4
    var camPos = SIMD4<Float>(repeating: 0)
    var sizes = SIMD4<Float>(repeating: 0)
    var eye = SIMD4<Float>(repeating: 0)
    var eyeOther = SIMD4<Float>(repeating: 0)
    var grid = SIMD4<Float>(repeating: 0)
    var flags = SIMD4<Float>(repeating: 0)
    var depthInfo = SIMD4<Float>(repeating: 0)
    var stereo = SIMD4<Float>(repeating: 0)
    var tuning = SIMD4<Float>(repeating: 0)
}

struct MeshUniforms {
    var viewProj = matrix_identity_float4x4
    var model = matrix_identity_float4x4
    var color = SIMD4<Float>(repeating: 0)
    var camPos = SIMD4<Float>(repeating: 0)
}

struct FxUniforms {
    var viewProj = matrix_identity_float4x4
    var camPos = SIMD4<Float>(repeating: 0)
    var floorInfo = SIMD4<Float>(repeating: 0)
}

struct FxVertex {
    var x, y, z, u, v, r, g, b, a: Float
    init(_ p: SIMD3<Float>, _ uv: SIMD2<Float>, _ c: SIMD4<Float>) {
        x = p.x; y = p.y; z = p.z; u = uv.x; v = uv.y; r = c.x; g = c.y; b = c.z; a = c.w
    }
}

struct Camera {
    var position: SIMD3<Float>
    var viewProj: simd_float4x4

    static func look(from position: SIMD3<Float>, at target: SIMD3<Float>, fovY: Float, aspect: Float) -> Camera {
        Camera(position: position,
               viewProj: perspective(fovY: fovY, aspect: aspect, near: 0.03, far: 300) * lookAt(eye: position, target: target))
    }
}

struct SceneOptions {
    var clip = true // only what shows through the window
    var fill = true // fill the gaps the picture cannot see into
    var flatten = false // the picture flat on the glass (Flat mode)
    var outline = true // the window's edge
    var floor = true
    var glass = false // the glass, lit (side views)
    var guns = true
    var step: Float = 1 // the relief's grid, in pixels
    var edge: Float = 0.04 // depth step (relative) where the relief is cut
    var eye = 0 // the eye whose picture is rebuilt
    var farthest: Float = 60 // meters
    var floorHeight: Float = -1.35 // below the screen's centre
    var beamWidth: Float = 0.006
    var pinTolerance: Float = 0.015 // the other eye's mismatch below which a pixel may be on the glass
    var ui = true // the menus, as they showed
}

final class Renderer {
    let device: MTLDevice
    let queue: MTLCommandQueue
    let width: Int, height: Int
    private let reconstructPipeline: MTLComputePipelineState
    private let indicesPipeline: MTLComputePipelineState
    private let reliefPipeline: MTLRenderPipelineState
    private let meshPipeline: MTLRenderPipelineState
    private let beamPipeline: MTLRenderPipelineState
    private let dotPipeline: MTLRenderPipelineState
    private let solidPipeline: MTLRenderPipelineState
    private let floorPipeline: MTLRenderPipelineState
    private let uiPipeline: MTLRenderPipelineState
    private var uiTextures: [Int: MTLTexture] = [:]
    private let depthWrite: MTLDepthStencilState
    private let depthRead: MTLDepthStencilState
    private let msaaColor: MTLTexture
    private let msaaDepth: MTLTexture
    let output: MTLTexture
    let gun: GunModel?

    // The rebuilt frame: its pictures and depth on the GPU, cached across output frames.
    private struct Reconstruction {
        var key: String
        var picture: MTLTexture
        var other: MTLTexture
        var depth: MTLTexture
        var dmap: MTLTexture
        var indices: MTLBuffer
        var fillIndices: MTLBuffer
        var indexCount: Int
        var fillIndexCount: Int
        var params: Params
    }
    private var reconstruction: Reconstruction?
    // The fill layer reaches this many grid cells past the picture's edges.
    static let fillMargin = 24
    private var fillIndexCache: (key: String, buffer: MTLBuffer)?

    init(width: Int, height: Int, gunModel: URL?) throws {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else {
            throw TrailerError("no Metal device")
        }
        self.device = device
        self.queue = queue
        self.width = width
        self.height = height
        let library = try device.makeLibrary(source: shaderSource, options: nil)
        func fn(_ name: String) throws -> MTLFunction {
            guard let f = library.makeFunction(name: name) else { throw TrailerError("no shader \(name)") }
            return f
        }
        reconstructPipeline = try device.makeComputePipelineState(function: fn("reconstruct"))
        indicesPipeline = try device.makeComputePipelineState(function: fn("buildIndices"))

        func pipeline(_ vs: String, _ fs: String, vertex: MTLVertexDescriptor?, blend: (MTLBlendFactor, MTLBlendFactor)?) throws -> MTLRenderPipelineState {
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = try fn(vs)
            d.fragmentFunction = try fn(fs)
            d.vertexDescriptor = vertex
            d.rasterSampleCount = 4
            d.colorAttachments[0].pixelFormat = .bgra8Unorm
            d.depthAttachmentPixelFormat = .depth32Float
            if let (src, dst) = blend {
                d.colorAttachments[0].isBlendingEnabled = true
                d.colorAttachments[0].sourceRGBBlendFactor = src
                d.colorAttachments[0].destinationRGBBlendFactor = dst
                d.colorAttachments[0].sourceAlphaBlendFactor = .one
                d.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
            }
            return try device.makeRenderPipelineState(descriptor: d)
        }
        let meshVertex = MTLVertexDescriptor()
        meshVertex.attributes[0].format = .float3
        meshVertex.attributes[1].format = .float3
        meshVertex.attributes[1].offset = 12
        meshVertex.layouts[0].stride = 24
        let fxVertex = MTLVertexDescriptor()
        fxVertex.attributes[0].format = .float3
        fxVertex.attributes[1].format = .float2
        fxVertex.attributes[1].offset = 12
        fxVertex.attributes[2].format = .float4
        fxVertex.attributes[2].offset = 20
        fxVertex.layouts[0].stride = MemoryLayout<FxVertex>.stride
        reliefPipeline = try pipeline("reliefVS", "reliefFS", vertex: nil, blend: (.sourceAlpha, .oneMinusSourceAlpha))
        meshPipeline = try pipeline("meshVS", "meshFS", vertex: meshVertex, blend: nil)
        beamPipeline = try pipeline("fxVS", "beamFS", vertex: fxVertex, blend: (.one, .one))
        dotPipeline = try pipeline("fxVS", "dotFS", vertex: fxVertex, blend: (.one, .one))
        solidPipeline = try pipeline("fxVS", "solidFS", vertex: fxVertex, blend: (.sourceAlpha, .oneMinusSourceAlpha))
        floorPipeline = try pipeline("fxVS", "floorFS", vertex: fxVertex, blend: (.one, .oneMinusSourceAlpha))
        uiPipeline = try pipeline("fxVS", "uiFS", vertex: fxVertex, blend: (.one, .oneMinusSourceAlpha))

        let ds = MTLDepthStencilDescriptor()
        ds.depthCompareFunction = .less
        ds.isDepthWriteEnabled = true
        depthWrite = device.makeDepthStencilState(descriptor: ds)!
        ds.isDepthWriteEnabled = false
        depthRead = device.makeDepthStencilState(descriptor: ds)!

        let color = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false)
        color.textureType = .type2DMultisample
        color.sampleCount = 4
        color.usage = .renderTarget
        color.storageMode = .private
        msaaColor = device.makeTexture(descriptor: color)!
        let depth = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .depth32Float, width: width, height: height, mipmapped: false)
        depth.textureType = .type2DMultisample
        depth.sampleCount = 4
        depth.usage = .renderTarget
        depth.storageMode = .private
        msaaDepth = device.makeTexture(descriptor: depth)!
        let out = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false)
        out.usage = [.renderTarget, .shaderRead]
        out.storageMode = .shared
        output = device.makeTexture(descriptor: out)!

        if let gunModel {
            gun = try? GunModel(url: gunModel, device: device)
        } else {
            gun = nil
        }
    }

    var aspect: Float { Float(width) / Float(height) }

    // MARK: The frame, rebuilt

    private func texture(_ format: MTLPixelFormat, _ w: Int, _ h: Int, _ bytes: UnsafeRawPointer?, _ bytesPerPixel: Int,
                         usage: MTLTextureUsage = .shaderRead, shared: Bool = false) -> MTLTexture {
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: max(w, 1), height: max(h, 1), mipmapped: false)
        d.usage = usage
        d.storageMode = bytes == nil && !shared ? .private : .shared
        let t = device.makeTexture(descriptor: d)!
        if let bytes {
            t.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: bytes, bytesPerRow: w * bytesPerPixel)
        }
        return t
    }

    private func prepare(_ frame: DecodedFrame, frameIndex: Int, options: SceneOptions) throws -> Reconstruction {
        let h = frame.header
        let eye = (options.eye == 1 && frame.right != nil) ? 1 : 0
        let key = "\(frameIndex)/\(eye)/\(options.step)/\(options.edge)/\(options.farthest)/\(options.pinTolerance)"
        if let reconstruction, reconstruction.key == key { return reconstruction }

        let w = h.width, ht = h.height
        let mine = eye == 0 ? frame.left : frame.right!
        let theirs = eye == 0 ? (frame.right ?? frame.left) : frame.left
        let picture = mine.withUnsafeBytes { texture(.rgba8Unorm, w, ht, $0.baseAddress, 4) }
        let other = theirs.withUnsafeBytes { texture(.rgba8Unorm, w, ht, $0.baseAddress, 4) }
        let depthPlane = eye == 0 ? frame.depthLeft : (frame.depthRight ?? frame.depthLeft)
        let depth: MTLTexture
        if let depthPlane, h.depthWidth > 0 {
            depth = depthPlane.withUnsafeBytes { texture(.r32Float, h.depthWidth, h.depthHeight, $0.baseAddress, 4) }
        } else {
            depth = texture(.r32Float, 1, 1, [Float(-1)], 4)
        }
        // The other eye's depth (where something nearer hides a point from it).
        let otherPlane = eye == 0 ? frame.depthRight : frame.depthLeft
        let otherDepth = otherPlane.map { plane in
            plane.withUnsafeBytes { texture(.r32Float, h.depthWidth, h.depthHeight, $0.baseAddress, 4) }
        } ?? depth
        let dmap = texture(.r32Float, w, ht, nil, 4, usage: [.shaderRead, .shaderWrite], shared: true)
        // The HUD mask, rows padded to four bytes.
        let hudStride = (w + 3) & ~3
        let hud: MTLTexture
        if let mask = frame.hudMask, mask.count >= hudStride * ht {
            let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r8Unorm, width: w, height: ht, mipmapped: false)
            d.usage = .shaderRead
            d.storageMode = .shared
            hud = device.makeTexture(descriptor: d)!
            mask.withUnsafeBytes { raw in
                // 0/1 bytes: scaled so the shader reads 1.0 on the HUD.
                var scaled = [UInt8](repeating: 0, count: hudStride * ht)
                for i in 0..<scaled.count where raw[i] != 0 { scaled[i] = 255 }
                hud.replace(region: MTLRegionMake2D(0, 0, w, ht), mipmapLevel: 0, withBytes: scaled, bytesPerRow: hudStride)
            }
        } else {
            hud = texture(.r8Unorm, 1, 1, [UInt8(0)], 1)
        }

        var p = Params()
        p.sizes = SIMD4(Float(w), Float(ht), h.screenWidth, h.screenHeight)
        p.eye = h.eye[eye]
        p.eyeOther = h.eye[1 - eye]
        if p.eye.z < 0.05 { p.eye = SIMD4(0, 0, h.screenDistance, p.eye.w) } // flat: no eye given
        if p.eyeOther.z < 0.05 { p.eyeOther = p.eye }
        let step = max(options.step, 1)
        let gw = Int(ceil(Float(w - 1) / step)) + 1, gh = Int(ceil(Float(ht - 1) / step)) + 1
        p.grid = SIMD4(Float(gw), Float(gh), step, options.edge)
        p.depthInfo = SIMD4(Float(max(h.depthWidth, 1)), Float(max(h.depthHeight, 1)),
                            depthPlane != nil && h.depthWidth > 0 ? 1 : 0,
                            frame.right != nil && h.mode != .flat ? 1 : 0)
        p.stereo = SIMD4(Float(h.mode.rawValue), h.convergence, h.screenDistance, options.farthest)
        p.tuning = SIMD4(options.pinTolerance, 1, 0, frame.hudMask != nil ? 1 : 0)

        let count = (gw - 1) * (gh - 1) * 6
        guard let indices = device.makeBuffer(length: count * 4, options: .storageModePrivate) else {
            throw TrailerError("out of GPU memory")
        }
        let margin = Renderer.fillMargin
        let fillCount = (gw - 1 + 2 * margin) * (gh - 1 + 2 * margin) * 6
        let fillKey = "\(gw)x\(gh)"
        let fillIndices: MTLBuffer
        let needFill = fillIndexCache?.key != fillKey
        if needFill {
            fillIndices = device.makeBuffer(length: fillCount * 4, options: .storageModePrivate)!
            fillIndexCache = (fillKey, fillIndices)
        } else {
            fillIndices = fillIndexCache!.buffer
        }

        let cmd = queue.makeCommandBuffer()!
        let enc = cmd.makeComputeCommandEncoder()!
        enc.setComputePipelineState(reconstructPipeline)
        enc.setTexture(picture, index: 0)
        enc.setTexture(other, index: 1)
        enc.setTexture(depth, index: 2)
        enc.setTexture(dmap, index: 3)
        enc.setTexture(otherDepth, index: 4)
        enc.setTexture(hud, index: 5)
        enc.setBytes(&p, length: MemoryLayout<Params>.stride, index: 0)
        enc.dispatchThreads(MTLSize(width: w, height: ht, depth: 1), threadsPerThreadgroup: MTLSize(width: 16, height: 16, depth: 1))
        enc.setComputePipelineState(indicesPipeline)
        enc.setTexture(dmap, index: 0)
        enc.setBuffer(indices, offset: 0, index: 1)
        enc.setBytes(&p, length: MemoryLayout<Params>.stride, index: 0)
        enc.dispatchThreads(MTLSize(width: gw, height: gh, depth: 1), threadsPerThreadgroup: MTLSize(width: 16, height: 16, depth: 1))
        if needFill {
            var all = p
            all.grid.x += Float(2 * margin)
            all.grid.y += Float(2 * margin)
            all.grid.w = 1e9 // never cut
            enc.setBuffer(fillIndices, offset: 0, index: 1)
            enc.setBytes(&all, length: MemoryLayout<Params>.stride, index: 0)
            enc.dispatchThreads(MTLSize(width: gw + 2 * margin, height: gh + 2 * margin, depth: 1),
                                threadsPerThreadgroup: MTLSize(width: 16, height: 16, depth: 1))
        }
        enc.endEncoding()
        cmd.commit()
        cmd.waitUntilCompleted()

        let r = Reconstruction(key: key, picture: picture, other: other, depth: depth, dmap: dmap, indices: indices,
                               fillIndices: fillIndices, indexCount: count, fillIndexCount: fillCount, params: p)
        reconstruction = r
        return r
    }

    // The rebuilt depth (m behind the glass) of the last frame prepared, for debugging.
    func lastDepthMap() -> (width: Int, height: Int, depth: [Float])? {
        guard let r = reconstruction else { return nil }
        let w = r.dmap.width, h = r.dmap.height
        var out = [Float](repeating: 0, count: w * h)
        r.dmap.getBytes(&out, bytesPerRow: w * 4, from: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0)
        return (w, h, out)
    }

    // MARK: Drawing

    struct Scene {
        var frame: DecodedFrame
        var frameIndex: Int
        var pose: PoseSample? // the headset's state at that moment
        var toScreen: Pose // LOCAL space into the screen's frame
        var flash: [Bool] = [false, false] // a shot just fired, per gun
        var showPicture = true // false before the game's first frame (the game list, no game yet)
        var capture: Capture? // for the menus' images
    }

    private func uiTexture(_ id: Int, _ capture: Capture) -> MTLTexture? {
        if let t = uiTextures[id] { return t }
        guard let image = capture.uiImage(id) else { return nil }
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: image.width, height: image.height, mipmapped: false)
        d.usage = .shaderRead
        d.storageMode = .shared
        guard let t = device.makeTexture(descriptor: d) else { return nil }
        image.pixels.withUnsafeBytes {
            t.replace(region: MTLRegionMake2D(0, 0, image.width, image.height), mipmapLevel: 0, withBytes: $0.baseAddress!,
                      bytesPerRow: image.width * 4)
        }
        if uiTextures.count > 16 { uiTextures.removeAll() }
        uiTextures[id] = t
        return t
    }

    // Renders into `output` and returns its pixels (BGRA, top row first).
    func render(_ scene: Scene, camera: Camera, options: SceneOptions, readback: Bool = true) throws -> [UInt8] {
        var prepared = options
        if options.flatten { prepared.edge = 1e9 } // flat on the glass: nothing to cut
        let r = try prepare(scene.frame, frameIndex: scene.frameIndex, options: prepared)
        let cmd = queue.makeCommandBuffer()!
        encode(cmd, scene: scene, reconstruction: r, camera: camera, options: options)
        cmd.commit()
        cmd.waitUntilCompleted()
        guard readback else { return [] }
        var bytes = [UInt8](repeating: 0, count: width * height * 4)
        output.getBytes(&bytes, bytesPerRow: width * 4, from: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0)
        return bytes
    }

    private func encode(_ cmd: MTLCommandBuffer, scene: Scene, reconstruction r: Reconstruction, camera: Camera, options: SceneOptions) {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = msaaColor
        pass.colorAttachments[0].resolveTexture = output
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].storeAction = .multisampleResolve
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.012, green: 0.014, blue: 0.02, alpha: 1)
        pass.depthAttachment.texture = msaaDepth
        pass.depthAttachment.loadAction = .clear
        pass.depthAttachment.storeAction = .dontCare
        pass.depthAttachment.clearDepth = 1
        let enc = cmd.makeRenderCommandEncoder(descriptor: pass)!
        var fx = FxUniforms(viewProj: camera.viewProj, camPos: SIMD4(camera.position, 1),
                            floorInfo: SIMD4(options.floorHeight, 0.5, 9, 0))
        let sw = r.params.sizes.z, sh = r.params.sizes.w

        // The floor, faint.
        if options.floor {
            let y = options.floorHeight, s: Float = 12
            let c = SIMD4<Float>(0.35, 0.42, 0.55, 0.55)
            drawFx(enc, floorPipeline, &fx, [
                FxVertex(SIMD3(-s, y, -s), .zero, c), FxVertex(SIMD3(s, y, -s), .zero, c), FxVertex(SIMD3(-s, y, s), .zero, c),
                FxVertex(SIMD3(s, y, -s), .zero, c), FxVertex(SIMD3(s, y, s), .zero, c), FxVertex(SIMD3(-s, y, s), .zero, c),
            ], depth: depthRead)
        }

        // The picture in depth: the fill layer first (behind), then the relief.
        var p = r.params
        p.viewProj = camera.viewProj
        p.camPos = SIMD4(camera.position, 1)
        p.flags = SIMD4(options.flatten ? 1 : 0, options.clip ? 1 : 0, 0, 0)
        enc.setRenderPipelineState(reliefPipeline)
        enc.setDepthStencilState(depthWrite)
        enc.setVertexTexture(r.dmap, index: 0)
        enc.setFragmentTexture(r.picture, index: 0)
        if !scene.showPicture {
            p.tuning.y = 0 // nothing on the screen yet
        }
        if options.fill && !options.flatten && scene.showPicture {
            var fill = p
            fill.flags.z = 1
            fill.flags.w = max(48, 8 * options.step) // pixels: about the largest parallax of a camera move
            fill.grid.x += Float(2 * Renderer.fillMargin)
            fill.grid.y += Float(2 * Renderer.fillMargin)
            fill.tuning.z = Float(Renderer.fillMargin)
            enc.setVertexBytes(&fill, length: MemoryLayout<Params>.stride, index: 0)
            enc.setFragmentBytes(&fill, length: MemoryLayout<Params>.stride, index: 0)
            enc.drawIndexedPrimitives(type: .triangle, indexCount: r.fillIndexCount, indexType: .uint32, indexBuffer: r.fillIndices, indexBufferOffset: 0)
        }
        if scene.showPicture {
            enc.setVertexBytes(&p, length: MemoryLayout<Params>.stride, index: 0)
            enc.setFragmentBytes(&p, length: MemoryLayout<Params>.stride, index: 0)
            enc.drawIndexedPrimitives(type: .triangle, indexCount: r.indexCount, indexType: .uint32, indexBuffer: r.indices, indexBufferOffset: 0)
        }

        // The window's edge.
        if options.outline {
            let b: Float = 0.006, z: Float = 0.0005
            let c = SIMD4<Float>(0.32, 0.35, 0.42, 1)
            var v: [FxVertex] = []
            func rect(_ x0: Float, _ y0: Float, _ x1: Float, _ y1: Float) {
                v += [FxVertex(SIMD3(x0, y0, z), .zero, c), FxVertex(SIMD3(x1, y0, z), .zero, c), FxVertex(SIMD3(x0, y1, z), .zero, c),
                      FxVertex(SIMD3(x1, y0, z), .zero, c), FxVertex(SIMD3(x1, y1, z), .zero, c), FxVertex(SIMD3(x0, y1, z), .zero, c)]
            }
            rect(-sw / 2 - b, sh / 2, sw / 2 + b, sh / 2 + b)
            rect(-sw / 2 - b, -sh / 2 - b, sw / 2 + b, -sh / 2)
            rect(-sw / 2 - b, -sh / 2, -sw / 2, sh / 2)
            rect(sw / 2, -sh / 2, sw / 2 + b, sh / 2)
            drawFx(enc, solidPipeline, &fx, v, depth: depthWrite)
        }

        // The guns, their beams and dots.
        var beams: [FxVertex] = [], dots: [FxVertex] = []
        if options.guns, let gun, let pose = scene.pose, pose.gunMode {
            enc.setRenderPipelineState(meshPipeline)
            enc.setDepthStencilState(depthWrite)
            for (g, sample) in pose.guns.enumerated() where sample.visible {
                let gunPose = scene.toScreen * sample.pose
                for draw in gun.draws {
                    let model = modelMatrix(gunPose * gun.partPose(draw, gun: sample))
                    let color = draw.material == "shell" ? srgbToLinear(sample.color) : draw.color
                    var u = MeshUniforms(viewProj: camera.viewProj, model: model, color: SIMD4(color, draw.roughness),
                                         camPos: SIMD4(camera.position, 1))
                    enc.setVertexBuffer(draw.vertexBuffer, offset: 0, index: 0)
                    enc.setVertexBytes(&u, length: MemoryLayout<MeshUniforms>.stride, index: 1)
                    enc.setFragmentBytes(&u, length: MemoryLayout<MeshUniforms>.stride, index: 1)
                    enc.drawIndexedPrimitives(type: .triangle, indexCount: draw.indexCount, indexType: .uint32,
                                              indexBuffer: draw.indexBuffer, indexBufferOffset: 0)
                }
                let muzzle = gunPose.apply(gun.muzzle)
                let end = scene.toScreen.apply(sample.beamEnd)
                let beamColor = SIMD4(sample.beamColor, 1)
                if sample.beam {
                    beams += ribbon(from: muzzle, to: end, width: options.beamWidth, camera: camera.position, color: beamColor)
                }
                if sample.dot {
                    let radius = 0.012 + 0.006 * simd_distance(camera.position, end)
                    dots += billboard(at: end, radius: radius, camera: camera.position, color: beamColor)
                }
                if g < scene.flash.count && scene.flash[g] {
                    dots += billboard(at: muzzle, radius: 0.07, camera: camera.position, color: SIMD4(1, 0.75, 0.35, 1))
                }
            }
        }
        // The menus: the panel and the pieces around the screen, as the headset layered them (over
        // the screen, under nearer guns).
        if options.ui, let pose = scene.pose, let capture = scene.capture {
            for quad in pose.ui {
                guard let texture = uiTexture(quad.image, capture) else { continue }
                let placed = quad.onScreen ? quad.pose : scene.toScreen * quad.pose
                let lift = SIMD3<Float>(0, 0, quad.onScreen ? 0.003 : 0)
                let c = placed.position + lift
                let right = placed.rotation.act(SIMD3(quad.size.x * 0.5, 0, 0))
                let up = placed.rotation.act(SIMD3(0, quad.size.y * 0.5, 0))
                let tw = Float(texture.width), th = Float(texture.height)
                let rect = quad.rect.z < 0 ? SIMD4(0, 0, tw, th) : quad.rect
                let u0 = rect.x / tw, v0 = rect.y / th, u1 = (rect.x + rect.z) / tw, v1 = (rect.y + rect.w) / th
                let white = SIMD4<Float>(1, 1, 1, 1)
                let tl = c - right + up, tr = c + right + up, bl = c - right - up, br = c + right - up
                enc.setFragmentTexture(texture, index: 0)
                drawFx(enc, uiPipeline, &fx, [
                    FxVertex(tl, SIMD2(u0, v0), white), FxVertex(tr, SIMD2(u1, v0), white), FxVertex(bl, SIMD2(u0, v1), white),
                    FxVertex(tr, SIMD2(u1, v0), white), FxVertex(br, SIMD2(u1, v1), white), FxVertex(bl, SIMD2(u0, v1), white),
                ], depth: depthRead)
            }
        }
        if !beams.isEmpty { drawFx(enc, beamPipeline, &fx, beams, depth: depthRead) }
        if !dots.isEmpty { drawFx(enc, dotPipeline, &fx, dots, depth: depthRead) }

        // The glass, for views from the side.
        if options.glass {
            let c = SIMD4<Float>(0.55, 0.7, 1.0, 0.07), z: Float = 0.001
            drawFx(enc, solidPipeline, &fx, [
                FxVertex(SIMD3(-sw / 2, -sh / 2, z), .zero, c), FxVertex(SIMD3(sw / 2, -sh / 2, z), .zero, c),
                FxVertex(SIMD3(-sw / 2, sh / 2, z), .zero, c), FxVertex(SIMD3(sw / 2, -sh / 2, z), .zero, c),
                FxVertex(SIMD3(sw / 2, sh / 2, z), .zero, c), FxVertex(SIMD3(-sw / 2, sh / 2, z), .zero, c),
            ], depth: depthRead)
        }
        enc.endEncoding()
    }

    private func drawFx(_ enc: MTLRenderCommandEncoder, _ pipeline: MTLRenderPipelineState, _ u: inout FxUniforms,
                        _ vertices: [FxVertex], depth: MTLDepthStencilState) {
        guard !vertices.isEmpty else { return }
        enc.setRenderPipelineState(pipeline)
        enc.setDepthStencilState(depth)
        let bytes = vertices.count * MemoryLayout<FxVertex>.stride
        if bytes <= 4096 {
            vertices.withUnsafeBytes { enc.setVertexBytes($0.baseAddress!, length: bytes, index: 0) }
        } else {
            enc.setVertexBuffer(device.makeBuffer(bytes: vertices, length: bytes), offset: 0, index: 0)
        }
        enc.setVertexBytes(&u, length: MemoryLayout<FxUniforms>.stride, index: 1)
        enc.setFragmentBytes(&u, length: MemoryLayout<FxUniforms>.stride, index: 1)
        enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: vertices.count)
    }

    private func ribbon(from a: SIMD3<Float>, to b: SIMD3<Float>, width: Float, camera: SIMD3<Float>, color: SIMD4<Float>) -> [FxVertex] {
        let dir = b - a
        var side = simd_cross(dir, camera - a)
        guard simd_length(side) > 1e-6 else { return [] }
        side = simd_normalize(side) * width * 0.5
        let a0 = a - side, a1 = a + side, b0 = b - side, b1 = b + side
        return [FxVertex(a0, SIMD2(0, 0), color), FxVertex(a1, SIMD2(0, 1), color), FxVertex(b0, SIMD2(1, 0), color),
                FxVertex(a1, SIMD2(0, 1), color), FxVertex(b1, SIMD2(1, 1), color), FxVertex(b0, SIMD2(1, 0), color)]
    }

    private func billboard(at c: SIMD3<Float>, radius: Float, camera: SIMD3<Float>, color: SIMD4<Float>) -> [FxVertex] {
        let toCam = simd_normalize(camera - c)
        let up0 = abs(toCam.y) > 0.95 ? SIMD3<Float>(1, 0, 0) : SIMD3<Float>(0, 1, 0)
        let right = simd_normalize(simd_cross(up0, toCam)) * radius
        let up = simd_normalize(simd_cross(toCam, right)) * radius
        let p00 = c - right - up, p10 = c + right - up, p01 = c - right + up, p11 = c + right + up
        return [FxVertex(p00, SIMD2(0, 0), color), FxVertex(p10, SIMD2(1, 0), color), FxVertex(p01, SIMD2(0, 1), color),
                FxVertex(p10, SIMD2(1, 0), color), FxVertex(p11, SIMD2(1, 1), color), FxVertex(p01, SIMD2(0, 1), color)]
    }
}
