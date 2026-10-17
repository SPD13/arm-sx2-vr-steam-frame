// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// The Metal shaders, compiled when the renderer starts (no Xcode needed).
//
// The scene behind the window comes back from one eye's picture and depth: the GS shifted every
// draw by the eye's offset times k (GSVolumetric.h), which is exactly what a point d behind the glass
// looks like from that eye, with k = d / (D + d) for an eye D in front of the glass. So each pixel is
// cast from the eye that saw it, through the glass, to its depth d:
//   Volumetric:  k = depth_factor * clamp(1 - Znorm / convergence, 0, 1), d = D k / (1 - k)
//   Volumetric+: u = clamp(Znorm / convergence, 0, 1), d = D0 (1 / u - 1)
// Draws the GS did not shift (a HUD drawn without Z) have the depth of what is under them in the
// depth buffer, but sit on the glass: the other eye's picture tells them apart (it shows them at the
// same place, not shifted).

let shaderSource = """
#include <metal_stdlib>
using namespace metal;

struct Params {
    float4x4 viewProj;
    float4 camPos;    // xyz
    float4 sizes;     // picture width, height (px), screen width, height (m)
    float4 eye;       // the eye the picture is from: x, y, distance (screen frame), depth factor
    float4 eyeOther;  // the other eye
    float4 grid;      // grid width, height (vertices), step (px), edge threshold
    float4 flags;     // flatten, clip to the window, fill layer, fill radius (px)
    float4 depthInfo; // depth width, height, has depth, has the other eye's picture
    float4 stereo;    // mode, convergence, screen distance, farthest depth (m)
    float4 tuning;    // pinned tolerance, alpha, fill margin (cells), 0
};

float depthBehind(float z, constant Params& p) {
    if (z < 0.0) return 0.0;
    float mode = p.stereo.x, conv = p.stereo.y;
    if (mode > 1.5) {
        float u = clamp(z / conv, 0.0, 1.0);
        if (u < 1e-4) return p.stereo.w;
        return min(p.stereo.z * (1.0 / u - 1.0), p.stereo.w);
    }
    if (mode > 0.5) {
        float k = min(p.eye.w * clamp(1.0 - z / conv, 0.0, 1.0), 0.98);
        return min(p.eye.z * k / (1.0 - k), p.stereo.w);
    }
    return 0.0;
}

float3 glassPoint(float2 px, constant Params& p) {
    return float3((px.x / p.sizes.x - 0.5) * p.sizes.z, (0.5 - px.y / p.sizes.y) * p.sizes.w, 0.0);
}

// The point d behind the glass that the eye saw at pixel px.
float3 pointBehind(float2 px, float d, float4 eye, constant Params& p) {
    float3 E = eye.xyz;
    float3 g = glassPoint(px, p);
    return E + (g - E) * ((E.z + d) / E.z);
}

// Where an eye sees P on the glass, in pixels.
float2 pixelOf(float3 P, float4 eye, constant Params& p) {
    float3 E = eye.xyz;
    float3 g = E + (P - E) * (E.z / (E.z - P.z));
    return float2((g.x / p.sizes.z + 0.5) * p.sizes.x, (0.5 - g.y / p.sizes.w) * p.sizes.y);
}

// How well the other eye's picture agrees that the pixel at px is at depth d (mean abs difference
// over a small patch).
float agreement(texture2d<float, access::read> picture, texture2d<float> other, float2 px, float d,
                constant Params& p, int radius = 1) {
    constexpr sampler s(filter::linear, address::clamp_to_edge, coord::pixel);
    float2 q = pixelOf(pointBehind(px, d, p.eye, p), p.eyeOther, p);
    float2 limit = p.sizes.xy - 1.0;
    float e = 0.0;
    for (int j = -radius; j <= radius; j++) {
        for (int i = -radius; i <= radius; i++) {
            float2 o = float2(i, j);
            float3 c = picture.read(uint2(clamp(px + o, float2(0.0), limit))).rgb;
            e += dot(abs(c - other.sample(s, q + o).rgb), float3(1.0));
        }
    }
    float n = float((2 * radius + 1) * (2 * radius + 1));
    return e / (3.0 * n);
}

// Each pixel's depth behind the glass (m), 0 on the glass. The depth image is coarser than the
// picture (the PS2's own resolution), so each pixel takes the depth of one of the texels around it:
// the one whose own colour is closest to the pixel's (a silhouette keeps its object's depth), near it,
// and, with the other eye's picture, the one that eye agrees with. Draws the GS did not shift (their
// depth is whatever was under them) go on the glass when the other eye shows them unshifted.
kernel void reconstruct(texture2d<float, access::read> picture [[texture(0)]],
                        texture2d<float> other [[texture(1)]],
                        texture2d<float, access::read> depth [[texture(2)]],
                        texture2d<float, access::write> out [[texture(3)]],
                        texture2d<float, access::read> otherDepth [[texture(4)]],
                        constant Params& p [[buffer(0)]],
                        uint2 gid [[thread_position_in_grid]]) {
    uint W = uint(p.sizes.x), H = uint(p.sizes.y);
    if (gid.x >= W || gid.y >= H) return;
    float d = 0.0;
    if (p.depthInfo.z > 0.5) {
        float2 dsizeF = p.depthInfo.xy;
        int2 dsize = int2(dsizeF);
        float2 scale = float2(W, H) / dsizeF; // picture pixels per depth texel
        float2 px = float2(gid) + 0.5;
        float2 uv = px / float2(W, H);
        float3 c = picture.read(gid).rgb;
        bool stereo = p.depthInfo.w > 0.5;
        int2 base = int2(floor(uv * dsizeF - 0.5));
        float bestScore = 1e9;
        float best = 0.0;
        for (int j = -1; j <= 2; j++) {
            for (int i = -1; i <= 2; i++) {
                int2 t = clamp(base + int2(i, j), int2(0), dsize - 1);
                float z = depth.read(uint2(t)).r;
                float dc = depthBehind(z, p);
                float2 centre = (float2(t) + 0.5) * scale;
                uint2 cp = uint2(clamp(centre, float2(0.0), float2(W - 1, H - 1)));
                float colour = dot(abs(picture.read(cp).rgb - c), float3(1.0)) / 3.0;
                float2 off = (px - centre) / scale;
                float score = colour + 0.03 * dot(off, off);
                if (stereo) score += 0.25 * agreement(picture, other, px, dc, p);
                if (score < bestScore) {
                    bestScore = score;
                    best = dc;
                }
            }
        }
        d = best;
        if (stereo && d > 0.0) {
            // Only when the other eye is clear about it: a pattern that repeats, or a spot that eye
            // cannot see (hidden behind something), must not put the scene on the glass.
            float2 q = pixelOf(pointBehind(px, d, p.eye, p), p.eyeOther, p);
            uint2 qt = uint2(clamp(q / scale, float2(0.0), dsizeF - 1.0));
            bool hidden = depthBehind(otherDepth.read(qt).r, p) < d - 0.05 * (p.eye.z + d);
            if (!hidden) {
                float eD = agreement(picture, other, px, d, p, 2);
                float eP = agreement(picture, other, px, 0.0, p, 2);
                if (eP < p.tuning.x && eD > eP * 3.0 + 0.06) d = 0.0;
            }
        }
    }
    out.write(float4(d), gid);
}

float depthAtVertex(texture2d<float, access::read> dmap, uint2 g, constant Params& p) {
    uint2 px = min(uint2(float2(g) * p.grid.z), uint2(p.sizes.xy) - 1);
    return dmap.read(px).r;
}

// The relief's triangles: a grid cell across a step in depth is left out (the fill layer behind shows).
kernel void buildIndices(texture2d<float, access::read> dmap [[texture(0)]],
                         device uint* indices [[buffer(1)]],
                         constant Params& p [[buffer(0)]],
                         uint2 gid [[thread_position_in_grid]]) {
    uint gw = uint(p.grid.x), gh = uint(p.grid.y);
    if (gid.x >= gw - 1 || gid.y >= gh - 1) return;
    uint i0 = gid.y * gw + gid.x, i1 = i0 + 1, i2 = i0 + gw, i3 = i2 + 1;
    float d0 = depthAtVertex(dmap, gid, p), d1 = depthAtVertex(dmap, gid + uint2(1, 0), p);
    float d2 = depthAtVertex(dmap, gid + uint2(0, 1), p), d3 = depthAtVertex(dmap, gid + uint2(1, 1), p);
    float lo = min(min(d0, d1), min(d2, d3)), hi = max(max(d0, d1), max(d2, d3));
    bool cut = (hi - lo) > p.grid.w * (p.eye.z + lo);
    uint base = (gid.y * (gw - 1) + gid.x) * 6;
    if (cut) {
        for (uint k = 0; k < 6; k++) indices[base + k] = i0;
    } else {
        indices[base + 0] = i0; indices[base + 1] = i2; indices[base + 2] = i1;
        indices[base + 3] = i1; indices[base + 4] = i2; indices[base + 5] = i3;
    }
}

struct ReliefOut {
    float4 pos [[position]];
    float2 uv;
    float3 world;
    float pushed; // fill layer: 1 where the vertex took the background's depth and colour
};

vertex ReliefOut reliefVS(uint vid [[vertex_id]],
                          constant Params& p [[buffer(0)]],
                          texture2d<float, access::read> dmap [[texture(0)]]) {
    uint gw = uint(p.grid.x);
    int2 size = int2(p.sizes.xy);
    bool fill = p.flags.z > 0.5;
    // The fill layer's grid reaches tuning.z cells past the picture's edges (the edge stretched out).
    int margin = fill ? int(p.tuning.z) : 0;
    int2 g = int2(vid % gw, vid / gw) - margin;
    float2 at = float2(g) * p.grid.z;
    int2 ip = clamp(int2(at), int2(0), size - 1);
    float d = dmap.read(uint2(ip)).r;
    int2 colourAt = ip;
    float pushed = 0.0;
    if (fill) {
        // The fill layer: the nearest background around (a depth clearly behind this one), with its
        // colour, so the gaps the picture cannot see into show the background drawn out, not the edge
        // of what is in front of it.
        int r = int(p.flags.w);
        int st = max(max(int(p.grid.z), 1), r / 8);
        float own = d;
        float closest = 1e9;
        for (int j = -r; j <= r; j += st) {
            for (int i = -r; i <= r; i += st) {
                int2 q = clamp(ip + int2(i, j), int2(0), size - 1);
                float dq = dmap.read(uint2(q)).r;
                float dist2 = float(i * i + j * j);
                if (dq > own + p.grid.w * (p.eye.z + own) && dist2 < closest) {
                    closest = dist2;
                    d = dq;
                    colourAt = q;
                    pushed = 1.0;
                }
            }
        }
        d += 0.01;
    }
    float2 px = at + 0.5;
    float3 P = pointBehind(px, d, p.eye, p);
    if (p.flags.x > 0.5) {
        // Flat: the picture on the glass, as Flat mode shows it (one eye's offset is a few pixels).
        P = glassPoint(px, p);
    }
    ReliefOut out;
    out.pos = p.viewProj * float4(P, 1.0);
    out.uv = (float2(colourAt) + 0.5) / p.sizes.xy;
    out.world = P;
    out.pushed = pushed;
    return out;
}

fragment float4 reliefFS(ReliefOut in [[stage_in]],
                         constant Params& p [[buffer(0)]],
                         texture2d<float> picture [[texture(0)]]) {
    // Fill layer: a long triangle stretched between the background and what is in front of it is a
    // smear (the gap stays dark rather); a small one only seals a crack.
    if (in.pushed > 0.02 && in.pushed < 0.98 && fwidth(in.pushed) < 0.25) discard_fragment();
    if (p.flags.y > 0.5 && in.world.z < -0.002 && p.camPos.z > 0.01) {
        // Only what shows through the window.
        float3 c = p.camPos.xyz;
        float3 dir = in.world - c;
        float2 h = c.xy + dir.xy * (-c.z / dir.z);
        if (abs(h.x) > p.sizes.z * 0.5 || abs(h.y) > p.sizes.w * 0.5) discard_fragment();
    }
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    return float4(picture.sample(s, in.uv).rgb, p.tuning.y);
}

struct MeshIn {
    float3 pos [[attribute(0)]];
    float3 normal [[attribute(1)]];
};

struct MeshUniforms {
    float4x4 viewProj;
    float4x4 model;
    float4 color;  // linear rgb, roughness
    float4 camPos;
};

struct MeshOut {
    float4 pos [[position]];
    float3 normal;
    float3 world;
};

vertex MeshOut meshVS(MeshIn in [[stage_in]], constant MeshUniforms& u [[buffer(1)]]) {
    MeshOut out;
    float4 w = u.model * float4(in.pos, 1.0);
    out.pos = u.viewProj * w;
    out.world = w.xyz;
    out.normal = (u.model * float4(in.normal, 0.0)).xyz;
    return out;
}

float3 srgbEncode(float3 c) {
    c = max(c, 0.0);
    return select(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, c < 0.0031308);
}

fragment float4 meshFS(MeshOut in [[stage_in]], constant MeshUniforms& u [[buffer(1)]]) {
    float3 n = normalize(in.normal);
    float3 v = normalize(u.camPos.xyz - in.world);
    if (dot(n, v) < 0.0) n = -n;
    float3 key = normalize(float3(0.4, 0.9, 0.7));
    float3 back = normalize(float3(-0.7, 0.3, -0.5));
    float3 base = u.color.rgb;
    float diffuse = max(dot(n, key), 0.0) * 0.85 + max(dot(n, back), 0.0) * 0.3 + 0.18;
    float3 h = normalize(key + v);
    float shine = mix(80.0, 6.0, u.color.a);
    float spec = pow(max(dot(n, h), 0.0), shine) * mix(0.6, 0.12, u.color.a);
    float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0) * 0.35;
    float3 c = base * diffuse + spec + rim * float3(0.45, 0.55, 0.75);
    return float4(srgbEncode(c), 1.0);
}

struct FxIn {
    float3 pos [[attribute(0)]];
    float2 uv [[attribute(1)]];
    float4 color [[attribute(2)]];
};

struct FxUniforms {
    float4x4 viewProj;
    float4 camPos;
    float4 floorInfo; // floor height, grid spacing, fade distance, 0
};

struct FxOut {
    float4 pos [[position]];
    float2 uv;
    float4 color;
    float3 world;
};

vertex FxOut fxVS(FxIn in [[stage_in]], constant FxUniforms& u [[buffer(1)]]) {
    FxOut out;
    out.pos = u.viewProj * float4(in.pos, 1.0);
    out.uv = in.uv;
    out.color = in.color;
    out.world = in.pos;
    return out;
}

// Beams: uv.y across (soft edges); dots and flashes: uv from the centre (round).
fragment float4 beamFS(FxOut in [[stage_in]]) {
    float across = 1.0 - abs(in.uv.y * 2.0 - 1.0);
    float glow = pow(across, 3.0) + pow(across, 24.0) * 1.5;
    return float4(in.color.rgb * glow * in.color.a, 1.0);
}

fragment float4 dotFS(FxOut in [[stage_in]]) {
    float r = length(in.uv * 2.0 - 1.0);
    float glow = pow(saturate(1.0 - r), 2.0) + pow(saturate(1.0 - r), 12.0) * 2.0;
    return float4(in.color.rgb * glow * in.color.a, 1.0);
}

fragment float4 solidFS(FxOut in [[stage_in]]) {
    return in.color;
}

// A faint grid on the floor, fading out with distance.
fragment float4 floorFS(FxOut in [[stage_in]], constant FxUniforms& u [[buffer(1)]]) {
    float2 q = in.world.xz / u.floorInfo.y;
    float2 g = abs(fract(q - 0.5) - 0.5) / fwidth(q);
    float line = 1.0 - min(min(g.x, g.y), 1.0);
    float fade = saturate(1.0 - length(in.world.xz - float2(0.0, 1.0)) / u.floorInfo.z);
    float a = line * fade * in.color.a;
    return float4(in.color.rgb * a, a);
}
"""
