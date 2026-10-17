// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "capture/CaptureWriter.h"

#include <lz4.h>

#include <cstring>
#include <filesystem>
#include <system_error>

CaptureWriter::~CaptureWriter()
{
	Finish();
}

bool CaptureWriter::Start(const std::string& dir, int threads, uint64_t max_bytes)
{
	namespace fs = std::filesystem;
	std::error_code ec;
	if (fs::exists(fs::path(dir) / "capture.json", ec))
	{
		std::fprintf(stderr, "Capture: %s already holds a capture\n", dir.c_str());
		return false;
	}
	fs::create_directories(fs::path(dir) / "frames", ec);
	if (!ec)
		fs::create_directories(fs::path(dir) / "ui", ec);
	if (ec)
	{
		std::fprintf(stderr, "Capture: cannot create %s/frames: %s\n", dir.c_str(), ec.message().c_str());
		return false;
	}
	m_poses = std::fopen((fs::path(dir) / "poses.jsonl").c_str(), "wb");
	if (!m_poses)
		return false;
	m_dir = dir;
	m_max_bytes = max_bytes;
	m_stopping = m_line_stopping = false;
	for (int i = 0; i < threads; i++)
		m_workers.emplace_back(&CaptureWriter::WorkerLoop, this);
	m_line_thread = std::thread(&CaptureWriter::LineLoop, this);
	m_started = true;
	return true;
}

void CaptureWriter::Finish()
{
	if (!m_started)
		return;
	{
		std::lock_guard lock(m_mutex);
		m_stopping = true;
	}
	m_cv.notify_all();
	for (std::thread& worker : m_workers)
		worker.join();
	m_workers.clear();
	{
		std::lock_guard lock(m_line_mutex);
		m_line_stopping = true;
	}
	m_line_cv.notify_all();
	m_line_thread.join();
	std::fclose(m_poses);
	m_poses = nullptr;
	m_started = false;
}

void CaptureWriter::Submit(Job job)
{
	if (Full())
	{
		if (job.done)
			job.done();
		return;
	}
	{
		std::lock_guard lock(m_mutex);
		m_jobs.push_back(std::move(job));
	}
	m_cv.notify_one();
}

void CaptureWriter::WriteLine(std::string line)
{
	{
		std::lock_guard lock(m_line_mutex);
		m_lines += line;
		m_lines += '\n';
	}
	m_line_cv.notify_one();
}

size_t CaptureWriter::Backlog()
{
	std::lock_guard lock(m_mutex);
	return m_jobs.size();
}

void CaptureWriter::WorkerLoop()
{
	std::vector<char> buffer; // this thread's compressed planes
	for (;;)
	{
		Job job;
		{
			std::unique_lock lock(m_mutex);
			m_cv.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });
			if (m_jobs.empty())
				return; // stopping, and all written
			job = std::move(m_jobs.front());
			m_jobs.pop_front();
		}
		if (!Full())
		{
			if (job.image_path.empty())
				WriteFrame(job, buffer);
			else
				WriteImage(job, buffer);
		}
		if (job.done)
			job.done();
	}
}

void CaptureWriter::WriteFrame(Job& job, std::vector<char>& buffer)
{
	CaptureFormat::FrameHeader& header = job.header;
	std::memcpy(header.magic, CaptureFormat::MAGIC, sizeof(header.magic));
	header.header_size = sizeof(header);

	// Each plane compressed into the buffer, one after the other; kept as is when it does not shrink.
	size_t bound = 0;
	for (uint32_t i = 0; i < header.plane_count; i++)
		bound += static_cast<size_t>(LZ4_compressBound(static_cast<int>(header.planes[i].raw_size)));
	if (buffer.size() < bound)
		buffer.resize(bound);
	std::array<const char*, CaptureFormat::MAX_PLANES> stored{};
	size_t used = 0;
	for (uint32_t i = 0; i < header.plane_count; i++)
	{
		CaptureFormat::Plane& plane = header.planes[i];
		const int raw = static_cast<int>(plane.raw_size);
		const int packed = LZ4_compress_fast(static_cast<const char*>(job.data[i]), buffer.data() + used, raw,
			LZ4_compressBound(raw), 1);
		if (packed > 0 && packed < raw)
		{
			plane.packed_size = static_cast<uint32_t>(packed);
			stored[i] = buffer.data() + used;
			used += static_cast<size_t>(packed);
		}
		else
		{
			plane.packed_size = plane.raw_size;
			stored[i] = static_cast<const char*>(job.data[i]);
		}
	}

	char name[32];
	std::snprintf(name, sizeof(name), "frames/%06u.bin", header.index);
	const std::string path = m_dir + "/" + name;
	std::FILE* file = std::fopen(path.c_str(), "wb");
	if (!file)
		return;
	size_t total = std::fwrite(&header, 1, sizeof(header), file);
	for (uint32_t i = 0; i < header.plane_count; i++)
		total += std::fwrite(stored[i], 1, header.planes[i].packed_size, file);
	std::fclose(file);

	m_frames.fetch_add(1, std::memory_order_relaxed);
	if (m_bytes.fetch_add(total, std::memory_order_relaxed) + total >= m_max_bytes)
		m_full.store(true, std::memory_order_release);
}

void CaptureWriter::WriteImage(Job& job, std::vector<char>& buffer)
{
	CaptureFormat::UiImageHeader header{};
	std::memcpy(header.magic, CaptureFormat::UI_MAGIC, sizeof(header.magic));
	header.width = job.image_width;
	header.height = job.image_height;
	const int raw = static_cast<int>(job.image.size() * sizeof(uint32_t));
	header.raw_size = static_cast<uint32_t>(raw);
	if (buffer.size() < static_cast<size_t>(LZ4_compressBound(raw)))
		buffer.resize(static_cast<size_t>(LZ4_compressBound(raw)));
	const int packed = LZ4_compress_fast(reinterpret_cast<const char*>(job.image.data()), buffer.data(), raw,
		LZ4_compressBound(raw), 1);
	const bool compressed = packed > 0 && packed < raw;
	header.packed_size = compressed ? static_cast<uint32_t>(packed) : header.raw_size;
	std::FILE* file = std::fopen((m_dir + "/" + job.image_path).c_str(), "wb");
	if (!file)
		return;
	size_t total = std::fwrite(&header, 1, sizeof(header), file);
	total += std::fwrite(compressed ? buffer.data() : reinterpret_cast<const char*>(job.image.data()), 1, header.packed_size, file);
	std::fclose(file);
	m_bytes.fetch_add(total, std::memory_order_relaxed);
}

void CaptureWriter::LineLoop()
{
	std::string lines;
	for (;;)
	{
		{
			std::unique_lock lock(m_line_mutex);
			m_line_cv.wait(lock, [this] { return m_line_stopping || !m_lines.empty(); });
			if (m_lines.empty() && m_line_stopping)
				break;
			lines.swap(m_lines);
		}
		std::fwrite(lines.data(), 1, lines.size(), m_poses);
		lines.clear();
	}
	std::fflush(m_poses);
}
