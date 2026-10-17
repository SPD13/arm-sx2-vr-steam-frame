// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// Shots go to ffmpeg as raw frames (one file per shot, with its sound), then into one cut.

import Foundation

struct AudioSource {
    var file: URL?
    var start: Double = 0 // seconds into the file (negative: silence first)
    var volume: Double = 1
}

func ffmpegPath() throws -> String {
    for path in ["/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg"] where FileManager.default.isExecutableFile(atPath: path) {
        return path
    }
    throw TrailerError("ffmpeg not found (brew install ffmpeg)")
}

func runFfmpeg(_ args: [String]) throws {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: try ffmpegPath())
    p.arguments = ["-hide_banner", "-loglevel", "error", "-y"] + args
    try p.run()
    p.waitUntilExit()
    if p.terminationStatus != 0 { throw TrailerError("ffmpeg failed: \(args.joined(separator: " "))") }
}

enum Codec: String {
    case prores, h264

    var videoArgs: [String] {
        switch self {
        case .prores: return ["-c:v", "prores_ks", "-profile:v", "3", "-pix_fmt", "yuv422p10le"]
        case .h264: return ["-c:v", "libx264", "-preset", "slow", "-crf", "14", "-pix_fmt", "yuv420p"]
        }
    }
    var audioArgs: [String] {
        switch self {
        case .prores: return ["-c:a", "pcm_s16le"]
        case .h264: return ["-c:a", "aac", "-b:a", "256k"]
        }
    }
    var ext: String { self == .prores ? "mov" : "mp4" }
}

final class VideoWriter {
    private let process = Process()
    private let pipe = Pipe()
    let url: URL
    let frameBytes: Int

    init(url: URL, width: Int, height: Int, fps: Double, duration: Double, codec: Codec, audio: AudioSource?,
         fadeIn: Double, fadeOut: Double) throws {
        self.url = url
        frameBytes = width * height * 4
        var args = ["-hide_banner", "-loglevel", "error", "-y",
                    "-f", "rawvideo", "-pix_fmt", "bgra", "-s", "\(width)x\(height)", "-r", "\(fps)", "-i", "-"]
        var audioFilters: [String] = []
        if let audio, let file = audio.file {
            if audio.start >= 0 {
                args += ["-ss", String(format: "%.4f", audio.start), "-i", file.path]
            } else {
                args += ["-i", file.path]
                let ms = Int(-audio.start * 1000)
                audioFilters.append("adelay=\(ms)|\(ms)")
            }
            audioFilters.append("volume=\(audio.volume)")
        } else {
            args += ["-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo"]
        }
        audioFilters += ["aresample=48000", "apad"]
        var videoFilters: [String] = []
        if fadeIn > 0 {
            videoFilters.append("fade=t=in:st=0:d=\(fadeIn)")
            audioFilters.append("afade=t=in:st=0:d=\(fadeIn)")
        }
        if fadeOut > 0 {
            videoFilters.append(String(format: "fade=t=out:st=%.4f:d=%.4f", max(duration - fadeOut, 0), fadeOut))
            audioFilters.append(String(format: "afade=t=out:st=%.4f:d=%.4f", max(duration - fadeOut, 0), fadeOut))
        }
        if !videoFilters.isEmpty { args += ["-vf", videoFilters.joined(separator: ",")] }
        args += ["-af", audioFilters.joined(separator: ","), "-map", "0:v", "-map", "1:a", "-ac", "2",
                 "-t", String(format: "%.4f", duration)]
        args += codec.videoArgs + codec.audioArgs + [url.path]
        process.executableURL = URL(fileURLWithPath: try ffmpegPath())
        process.arguments = args
        process.standardInput = pipe
        try process.run()
    }

    func write(_ bytes: [UInt8]) {
        bytes.withUnsafeBytes { pipe.fileHandleForWriting.write(Data($0)) }
    }

    func finish() throws {
        try pipe.fileHandleForWriting.close()
        process.waitUntilExit()
        if process.terminationStatus != 0 { throw TrailerError("ffmpeg failed writing \(url.path)") }
    }
}

// An outside video (a screen recording) as a shot like the others: the same size, rate and sound.
func conformClip(_ input: URL, to url: URL, width: Int, height: Int, fps: Double, start: Double, duration: Double?,
                 codec: Codec, fadeIn: Double, fadeOut: Double, volume: Double, caption: URL? = nil, captionAt: Double = 0.4) throws {
    let probe = Process()
    probe.executableURL = URL(fileURLWithPath: try ffmpegPath().replacingOccurrences(of: "ffmpeg", with: "ffprobe"))
    probe.arguments = ["-v", "error", "-select_streams", "a", "-show_entries", "stream=index", "-of", "csv=p=0", input.path]
    let out = Pipe()
    probe.standardOutput = out
    try probe.run()
    probe.waitUntilExit()
    let hasAudio = !(String(data: out.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? "").isEmpty
    let length: Double
    if let duration { length = duration } else { length = try videoDuration(input) - start }

    var args = ["-ss", "\(start)", "-i", input.path]
    if !hasAudio { args += ["-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo"] }
    let captionInput = hasAudio ? 1 : 2
    if let caption { args += ["-loop", "1", "-i", caption.path] }
    var v = "scale=\(width):\(height):force_original_aspect_ratio=decrease,pad=\(width):\(height):(ow-iw)/2:(oh-ih)/2:color=0x030305,setsar=1,fps=\(fps),format=yuv420p"
    var a = "aresample=48000,volume=\(volume),apad"
    if let _ = caption {
        // The caption fades in and out over the picture, as on the other shots.
        let out = max(length - fadeOut - 0.45, captionAt + 0.4)
        v = "[0:v]\(v)[base];[\(captionInput):v]format=rgba,fade=t=in:st=\(captionAt):d=0.35:alpha=1," +
            String(format: "fade=t=out:st=%.3f:d=0.35:alpha=1[cap];[base][cap]overlay=shortest=1:format=auto,format=yuv420p", out)
    }
    if fadeIn > 0 { v += ",fade=t=in:st=0:d=\(fadeIn)"; a += ",afade=t=in:st=0:d=\(fadeIn)" }
    if fadeOut > 0 {
        v += String(format: ",fade=t=out:st=%.4f:d=%.4f", max(length - fadeOut, 0), fadeOut)
        a += String(format: ",afade=t=out:st=%.4f:d=%.4f", max(length - fadeOut, 0), fadeOut)
    }
    if caption != nil {
        args += ["-filter_complex", v + "[vout]", "-map", "[vout]"]
    } else {
        args += ["-vf", v, "-map", "0:v:0"]
    }
    args += ["-af", a, "-map", hasAudio ? "0:a:0" : "1:a", "-ac", "2", "-t", String(format: "%.4f", length)]
    args += codec.videoArgs + codec.audioArgs + [url.path]
    try runFfmpeg(args)
}

// A still (a screenshot of the Wi-Fi page or the installer) as a shot: fitted into the frame on a dark
// backdrop, pushing in slowly toward focus (0..1 of the frame), with a caption like the other shots.
func imageShot(_ input: URL, to url: URL, width: Int, height: Int, fps: Double, duration: Double, zoom: Double,
               focus: SIMD2<Double>, codec: Codec, fadeIn: Double, fadeOut: Double, caption: URL?, captionAt: Double) throws {
    let frames = max(Int((duration * fps).rounded()), 1)
    // Zoomed on a 4x canvas, so the slow push does not step from pixel to pixel.
    let bw = width * 4, bh = height * 4
    // With a caption the picture keeps to the right, the caption has the lower left to itself.
    let fit = caption != nil ? "scale=\(bw * 62 / 100):\(bh * 88 / 100):force_original_aspect_ratio=decrease,pad=\(bw):\(bh):ow-iw-\(bw * 5 / 100):(oh-ih)/2" :
        "scale=\(bw * 92 / 100):\(bh * 92 / 100):force_original_aspect_ratio=decrease,pad=\(bw):\(bh):(ow-iw)/2:(oh-ih)/2"
    var v = "[0:v]\(fit):color=0x0b0d14," +
        String(format: "zoompan=z='1+%.4f*on/%d':x='(iw-iw/zoom)*%.3f':y='(ih-ih/zoom)*%.3f':d=1:s=%dx%d:fps=%.3f,setsar=1,format=yuv420p",
               zoom - 1, max(frames - 1, 1), focus.x, focus.y, width, height, fps)
    if fadeIn > 0 { v += ",fade=t=in:st=0:d=\(fadeIn)" }
    if fadeOut > 0 { v += String(format: ",fade=t=out:st=%.4f:d=%.4f", max(duration - fadeOut, 0), fadeOut) }
    var args = ["-loop", "1", "-framerate", "\(fps)", "-i", input.path, "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo"]
    if let caption {
        args += ["-loop", "1", "-i", caption.path]
        let out = max(duration - fadeOut - 0.45, captionAt + 0.4)
        v += "[base];[2:v]format=rgba,fade=t=in:st=\(captionAt):d=0.35:alpha=1," +
            String(format: "fade=t=out:st=%.3f:d=0.35:alpha=1[cap];[base][cap]overlay=shortest=1:format=auto,format=yuv420p", out)
    }
    args += ["-filter_complex", v + "[vout]", "-map", "[vout]", "-map", "1:a", "-ac", "2",
             "-t", String(format: "%.4f", duration)]
    args += codec.videoArgs + codec.audioArgs + [url.path]
    try runFfmpeg(args)
}

func videoDuration(_ url: URL) throws -> Double {
    let probe = Process()
    probe.executableURL = URL(fileURLWithPath: try ffmpegPath().replacingOccurrences(of: "ffmpeg", with: "ffprobe"))
    probe.arguments = ["-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", url.path]
    let out = Pipe()
    probe.standardOutput = out
    try probe.run()
    probe.waitUntilExit()
    let text = String(data: out.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
    guard let d = Double(text.trimmingCharacters(in: .whitespacesAndNewlines)) else { throw TrailerError("cannot read the length of \(url.path)") }
    return d
}

// The shots one after the other, with music under them if given.
func assemble(_ shots: [URL], to url: URL, music: AudioSource?) throws {
    var args: [String] = []
    for shot in shots { args += ["-i", shot.path] }
    // Every shot square-pixelled and at the same rate, then one after the other.
    var graph = shots.indices.map { "[\($0):v]setsar=1,format=yuv420p[v\($0)];" }.joined()
    graph += shots.indices.map { "[v\($0)][\($0):a]" }.joined() + "concat=n=\(shots.count):v=1:a=1[v][a]"
    var audioOut = "[a]"
    if let music, let file = music.file {
        args += ["-ss", "\(max(music.start, 0))", "-i", file.path]
        graph += ";[\(shots.count):a]aresample=48000,volume=\(music.volume)[m];[a][m]amix=inputs=2:duration=first:normalize=0[mix]"
        audioOut = "[mix]"
    }
    args += ["-filter_complex", graph, "-map", "[v]", "-map", audioOut,
             "-c:v", "libx264", "-preset", "slow", "-crf", "16", "-pix_fmt", "yuv420p", "-movflags", "+faststart",
             "-c:a", "aac", "-b:a", "256k", url.path]
    try runFfmpeg(args)
}
