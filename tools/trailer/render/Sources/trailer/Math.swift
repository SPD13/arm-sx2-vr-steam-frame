// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// Matrices for Metal (clip z 0..1, column-major), in the screen's frame: x right, y up, z toward the
// viewer, the origin at the screen's centre, meters.

import simd

func perspective(fovY: Float, aspect: Float, near: Float, far: Float) -> simd_float4x4 {
    let y = 1 / tan(fovY * 0.5)
    let x = y / aspect
    let z = far / (near - far)
    return simd_float4x4(columns: (
        SIMD4(x, 0, 0, 0),
        SIMD4(0, y, 0, 0),
        SIMD4(0, 0, z, -1),
        SIMD4(0, 0, z * near, 0)
    ))
}

func lookAt(eye: SIMD3<Float>, target: SIMD3<Float>, up: SIMD3<Float> = SIMD3(0, 1, 0)) -> simd_float4x4 {
    let f = simd_normalize(target - eye)
    let s = simd_normalize(simd_cross(f, up))
    let u = simd_cross(s, f)
    return simd_float4x4(columns: (
        SIMD4(s.x, u.x, -f.x, 0),
        SIMD4(s.y, u.y, -f.y, 0),
        SIMD4(s.z, u.z, -f.z, 0),
        SIMD4(-simd_dot(s, eye), -simd_dot(u, eye), simd_dot(f, eye), 1)
    ))
}

func viewFromPose(_ pose: Pose) -> simd_float4x4 {
    let inv = pose.inverse
    let m = simd_float3x3(inv.rotation)
    return simd_float4x4(columns: (
        SIMD4(m.columns.0, 0),
        SIMD4(m.columns.1, 0),
        SIMD4(m.columns.2, 0),
        SIMD4(inv.position, 1)
    ))
}

func modelMatrix(_ pose: Pose, scale: SIMD3<Float> = SIMD3(1, 1, 1)) -> simd_float4x4 {
    let m = simd_float3x3(pose.rotation)
    return simd_float4x4(columns: (
        SIMD4(m.columns.0 * scale.x, 0),
        SIMD4(m.columns.1 * scale.y, 0),
        SIMD4(m.columns.2 * scale.z, 0),
        SIMD4(pose.position, 1)
    ))
}

// A view and projection from an eye through the screen's rectangle (the glass is the image plane):
// what that eye saw through the window, pixel for pixel with the captured picture.
func windowViewProjection(eye: SIMD3<Float>, screen: SIMD2<Float>, near: Float = 0.05, far: Float = 300) -> simd_float4x4 {
    let view = simd_float4x4(columns: (
        SIMD4(1, 0, 0, 0), SIMD4(0, 1, 0, 0), SIMD4(0, 0, 1, 0), SIMD4(-eye.x, -eye.y, -eye.z, 1)))
    let d = max(eye.z, 0.01)
    let l = (-screen.x / 2 - eye.x) * near / d, r = (screen.x / 2 - eye.x) * near / d
    let b = (-screen.y / 2 - eye.y) * near / d, t = (screen.y / 2 - eye.y) * near / d
    let z = far / (near - far)
    let proj = simd_float4x4(columns: (
        SIMD4(2 * near / (r - l), 0, 0, 0),
        SIMD4(0, 2 * near / (t - b), 0, 0),
        SIMD4((r + l) / (r - l), (t + b) / (t - b), z, -1),
        SIMD4(0, 0, z * near, 0)
    ))
    return proj * view
}

func smoothstep(_ t: Double) -> Double {
    let x = min(max(t, 0), 1)
    return x * x * (3 - 2 * x)
}

func catmullRom(_ p0: SIMD3<Float>, _ p1: SIMD3<Float>, _ p2: SIMD3<Float>, _ p3: SIMD3<Float>, _ t: Float) -> SIMD3<Float> {
    let t2 = t * t, t3 = t2 * t
    let a = 2 * p1
    let b = (p2 - p0) * t
    let c = (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2
    let d = (-p0 + 3 * p1 - 3 * p2 + p3) * t3
    return 0.5 * (a + b + c + d)
}
