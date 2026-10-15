// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "ScreenLayer.h"

#include "common/Console.h"

ScreenLayer::~ScreenLayer()
{
	Destroy();
}

bool ScreenLayer::Create(XrApp& xr, VkDevice device, VkQueue queue, uint32_t queue_family_index)
{
	m_xr = &xr;
	m_device = device;
	m_queue = queue;

	uint32_t count = 0;
	xrEnumerateSwapchainFormats(xr.Session(), 0, &count, nullptr);
	std::vector<int64_t> formats(count);
	xrEnumerateSwapchainFormats(xr.Session(), count, &count, formats.data());

	// The GS output is RGBA8_UNORM holding sRGB-encoded values. Copying those bits raw into an
	// R8G8B8A8_SRGB swapchain image (same texel size, so vkCmdCopyImage allows it) makes the
	// compositor decode them correctly; a blit would convert and apply the curve twice.
	m_format = 0;
	for (const int64_t f : formats)
	{
		if (f == VK_FORMAT_R8G8B8A8_SRGB)
		{
			m_format = f;
			m_srgb = true;
		}
	}
	if (!m_format)
	{
		for (const int64_t f : formats)
		{
			if (f == VK_FORMAT_R8G8B8A8_UNORM)
				m_format = f;
		}
		if (!m_format)
		{
			Console.Error("XR: the runtime offers no RGBA8 swapchain format.");
			return false;
		}
		Console.Warning("XR: no R8G8B8A8_SRGB swapchain; colours will look washed out.");
	}

	VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = queue_family_index;
	if (vkCreateCommandPool(device, &pool_info, nullptr, &m_pool) != VK_SUCCESS)
		return false;
	VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	alloc.commandPool = m_pool;
	alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc.commandBufferCount = NUM_CMD;
	if (vkAllocateCommandBuffers(device, &alloc, m_cmd.data()) != VK_SUCCESS)
		return false;
	VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	for (VkFence& fence : m_fences)
	{
		if (vkCreateFence(device, &fence_info, nullptr, &fence) != VK_SUCCESS)
			return false;
	}
	return true;
}

void ScreenLayer::Destroy()
{
	if (!m_device)
		return;
	{
		std::lock_guard lock(GraphicsQueueMutex());
		vkQueueWaitIdle(m_queue);
	}
	DestroySwapchain();
	for (VkFence& fence : m_fences)
	{
		if (fence)
			vkDestroyFence(m_device, fence, nullptr);
		fence = VK_NULL_HANDLE;
	}
	if (m_pool)
		vkDestroyCommandPool(m_device, m_pool, nullptr);
	m_pool = VK_NULL_HANDLE;
	m_device = VK_NULL_HANDLE;
}

bool ScreenLayer::CreateSwapchain(uint32_t width, uint32_t height)
{
	XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
	info.usageFlags =
		XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	info.format = m_format;
	info.sampleCount = 1;
	info.width = width;
	info.height = height;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	{
		std::lock_guard lock(GraphicsQueueMutex());
		if (XR_FAILED(xrCreateSwapchain(m_xr->Session(), &info, &m_swapchain)))
		{
			Console.ErrorFmt("XR: xrCreateSwapchain {}x{} failed", width, height);
			m_swapchain = XR_NULL_HANDLE;
			return false;
		}
	}
	uint32_t count = 0;
	xrEnumerateSwapchainImages(m_swapchain, 0, &count, nullptr);
	m_images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
	xrEnumerateSwapchainImages(
		m_swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(m_images.data()));
	m_width_px = width;
	m_height_px = height;
	m_has_image = false;
	Console.WriteLnFmt("XR: screen swapchain {}x{} ({} images)", width, height, count);
	return true;
}

bool ScreenLayer::AcquireArmed()
{
	uint32_t index = 0;
	{
		std::lock_guard lock(GraphicsQueueMutex());
		if (XR_FAILED(xrAcquireSwapchainImage(m_swapchain, nullptr, &index)))
			return false;
	}
	XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wait.timeout = XR_INFINITE_DURATION;
	if (XR_FAILED(xrWaitSwapchainImage(m_swapchain, &wait)))
		return false; // still acquired: released with the next Update or with the swapchain
	m_armed = true;
	m_armed_given = false;
	m_armed_index = index;
	m_armed_id = s_next_armed_id++;
	return true;
}

void ScreenLayer::DisarmGiven()
{
	if (!m_armed_given)
		return;
	VKLibretro::DisarmPresentCopy(m_armed_eye);
	m_armed_given = false;
}

void ScreenLayer::Arm(int eye)
{
	if (!m_swapchain || (!m_armed && !AcquireArmed()))
		return;
	if (m_armed_given && m_armed_eye == eye)
		return;
	DisarmGiven();
	VKLibretro::PresentCopyTarget target;
	target.image = m_images[m_armed_index].image;
	target.width = m_width_px;
	target.height = m_height_px;
	target.id = m_armed_id;
	VKLibretro::ArmPresentCopy(eye, target);
	m_armed_given = true;
	m_armed_eye = eye;
}

bool ScreenLayer::ReleaseArmed(uint64_t copied_id)
{
	if (!m_armed || copied_id == 0 || copied_id != m_armed_id)
		return false;
	// The GS submitted the copy before publishing the frame, and took the target off the eye.
	m_armed = false;
	m_armed_given = false;
	XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	{
		std::lock_guard lock(GraphicsQueueMutex());
		xrReleaseSwapchainImage(m_swapchain, &release);
	}
	m_has_image = true;
	return true;
}

void ScreenLayer::DestroySwapchain()
{
	if (!m_swapchain)
		return;
	DisarmGiven();
	m_armed = false;
	std::lock_guard lock(GraphicsQueueMutex());
	vkQueueWaitIdle(m_queue);
	xrDestroySwapchain(m_swapchain);
	m_swapchain = XR_NULL_HANDLE;
	m_images.clear();
	m_has_image = false;
}

void ScreenLayer::Update(const VKLibretro::Frame& frame)
{
	if (!frame.image || !frame.width || !frame.height)
		return;
	if (frame.width != m_width_px || frame.height != m_height_px || !m_swapchain)
	{
		DestroySwapchain();
		if (!CreateSwapchain(frame.width, frame.height))
			return;
	}

	// The image acquired for the present copy, if the GS did not copy into it; taken back first if
	// it was handed over, so the GS cannot write it after it is released below.
	DisarmGiven();
	if (!m_armed && !AcquireArmed())
		return;
	const uint32_t index = m_armed_index;
	m_armed = false;

	const size_t slot = m_cmd_index;
	m_cmd_index = (m_cmd_index + 1) % NUM_CMD;
	vkWaitForFences(m_device, 1, &m_fences[slot], VK_TRUE, UINT64_MAX);
	vkResetFences(m_device, 1, &m_fences[slot]);
	VkCommandBuffer cmd = m_cmd[slot];
	vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &begin);

	const VkImage dst = m_images[index].image;
	const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	const auto barrier = [](VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
							 VkAccessFlags dst_access, const VkImageSubresourceRange& r) {
		VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		b.srcAccessMask = src_access;
		b.dstAccessMask = dst_access;
		b.oldLayout = from;
		b.newLayout = to;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = image;
		b.subresourceRange = r;
		return b;
	};

	// The GS submitted the present that rendered frame.image earlier on this queue, so a barrier
	// over all commands orders the copy after it. The image goes back to the layout the GS tracks.
	VkImageMemoryBarrier before[2] = {
		barrier(frame.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, range),
		barrier(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
			range),
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
		nullptr, 2, before);

	VkImageCopy region{};
	region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.extent = {frame.width, frame.height, 1};
	vkCmdCopyImage(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &region);

	VkImageMemoryBarrier after[2] = {
		barrier(frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, range),
		barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, range),
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
		nullptr, 2, after);
	vkEndCommandBuffer(cmd);

	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	{
		std::lock_guard lock(GraphicsQueueMutex());
		vkQueueSubmit(m_queue, 1, &submit, m_fences[slot]);
		xrReleaseSwapchainImage(m_swapchain, &release);
	}
	m_has_image = true;
}

const XrCompositionLayerBaseHeader* ScreenLayer::Layer()
{
	m_layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
	m_layer.space = m_xr->LocalSpace();
	m_layer.eyeVisibility = m_eyes;
	m_layer.subImage.swapchain = m_swapchain;
	m_layer.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(m_width_px), static_cast<int32_t>(m_height_px)}};
	m_layer.pose = m_pose;
	m_layer.size = {m_width, m_width * static_cast<float>(m_height_px) / static_cast<float>(m_width_px)};
	return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_layer);
}
