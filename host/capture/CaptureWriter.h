// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

// Writes the trailer capture's files (CaptureFormat.h) off the frame loop: worker threads compress
// each frame's planes with LZ4 and write them to a file of its own, and a thread of its own appends
// the pose lines. No Vulkan here, so that tools/trailer can test it on a computer.

#pragma once

#include "capture/CaptureFormat.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class CaptureWriter
{
public:
	~CaptureWriter();

	// Creates dir/frames (dir must not hold a capture already). max_bytes: stop writing frames past it.
	bool Start(const std::string& dir, int threads, uint64_t max_bytes);
	// Writes what is queued and stops the threads.
	void Finish();

	struct Job
	{
		CaptureFormat::FrameHeader header{}; // the planes' kinds and raw sizes filled in; packed sizes are the writer's
		std::array<const void*, CaptureFormat::MAX_PLANES> data{}; // each plane's bytes, valid until done runs
		std::function<void()> done; // the planes may be reused (called on a worker thread)
		// A menu image instead of a frame: written to image_path (relative to the capture's folder).
		std::string image_path;
		std::vector<uint32_t> image; // RGBA8
		uint32_t image_width = 0, image_height = 0;
	};
	// Queues a frame; never blocks. Once the size limit is reached, frames are not written (done still runs).
	void Submit(Job job);
	// Queues a line for poses.jsonl (a newline is added).
	void WriteLine(std::string line);

	bool Full() const { return m_full.load(std::memory_order_acquire); }
	uint64_t BytesWritten() const { return m_bytes.load(std::memory_order_relaxed); }
	uint32_t FramesWritten() const { return m_frames.load(std::memory_order_relaxed); }
	// Frames queued but not written yet.
	size_t Backlog();

private:
	void WorkerLoop();
	void LineLoop();
	void WriteFrame(Job& job, std::vector<char>& buffer);
	void WriteImage(Job& job, std::vector<char>& buffer);

	std::string m_dir;
	uint64_t m_max_bytes = 0;
	bool m_started = false;

	std::mutex m_mutex;
	std::condition_variable m_cv;
	std::deque<Job> m_jobs;
	bool m_stopping = false;
	std::vector<std::thread> m_workers;

	std::mutex m_line_mutex;
	std::condition_variable m_line_cv;
	std::string m_lines;
	bool m_line_stopping = false;
	std::thread m_line_thread;
	std::FILE* m_poses = nullptr;

	std::atomic<uint64_t> m_bytes{0};
	std::atomic<uint32_t> m_frames{0};
	std::atomic<bool> m_full{false};
};
