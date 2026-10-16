// SPDX-FileCopyrightText: 2026 ARMSX2 VR contributors
// SPDX-License-Identifier: GPL-3.0+

#include "OverlayLayer.h"

#include "common/Console.h"

#include <cstring>

OverlayLayer::~OverlayLayer()
{
	Destroy();
}

uint32_t OverlayLayer::FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties) const
{
	VkPhysicalDeviceMemoryProperties mem{};
	vkGetPhysicalDeviceMemoryProperties(m_gpu, &mem);
	for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
	{
		if ((type_bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & properties) == properties)
			return i;
	}
	return UINT32_MAX;
}

bool OverlayLayer::Create(XrApp& xr, VkDevice device, VkPhysicalDevice gpu, VkQueue queue, uint32_t queue_family_index,
	int width, int height)
{
	m_xr = &xr;
	m_device = device;
	m_gpu = gpu;
	m_queue = queue;
	m_width = width;
	m_height = height;

	// The canvas holds sRGB-encoded colour, as the GS output does (see ScreenLayer).
	uint32_t count = 0;
	xrEnumerateSwapchainFormats(xr.Session(), 0, &count, nullptr);
	std::vector<int64_t> formats(count);
	xrEnumerateSwapchainFormats(xr.Session(), count, &count, formats.data());
	int64_t format = 0;
	for (const int64_t f : formats)
	{
		if (f == VK_FORMAT_R8G8B8A8_SRGB || (!format && f == VK_FORMAT_R8G8B8A8_UNORM))
			format = f;
	}
	if (!format)
	{
		Console.Error("XR: the runtime offers no RGBA8 swapchain format for the menu.");
		return false;
	}

	XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
	// Colour attachment too: a released image has to be in COLOR_ATTACHMENT_OPTIMAL.
	info.usageFlags =
		XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	info.format = format;
	m_format = static_cast<VkFormat>(format);
	info.sampleCount = 1;
	info.width = static_cast<uint32_t>(width);
	info.height = static_cast<uint32_t>(height);
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	{
		std::lock_guard lock(GraphicsQueueMutex());
		if (XR_FAILED(xrCreateSwapchain(xr.Session(), &info, &m_swapchain)))
		{
			Console.ErrorFmt("XR: xrCreateSwapchain {}x{} failed for the menu", width, height);
			m_swapchain = XR_NULL_HANDLE;
			return false;
		}
	}
	xrEnumerateSwapchainImages(m_swapchain, 0, &count, nullptr);
	m_images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
	xrEnumerateSwapchainImages(m_swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(m_images.data()));

	VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = queue_family_index;
	if (vkCreateCommandPool(device, &pool_info, nullptr, &m_pool) != VK_SUCCESS)
		return false;
	VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	alloc.commandPool = m_pool;
	alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc.commandBufferCount = NUM_SLOTS;
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

bool OverlayLayer::EnsureStaging(VkDeviceSize size)
{
	if (size <= m_staging_size)
		return true;
	vkWaitForFences(m_device, NUM_SLOTS, m_fences.data(), VK_TRUE, UINT64_MAX);
	for (Staging& staging : m_staging)
	{
		if (staging.buffer)
			vkDestroyBuffer(m_device, staging.buffer, nullptr);
		if (staging.memory)
			vkFreeMemory(m_device, staging.memory, nullptr);
		staging = {};
	}
	m_staging_size = 0;
	for (Staging& staging : m_staging)
	{
		VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		buffer_info.size = size;
		buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(m_device, &buffer_info, nullptr, &staging.buffer) != VK_SUCCESS)
			return false;
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(m_device, staging.buffer, &req);
		VkMemoryAllocateInfo mem_alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mem_alloc.allocationSize = req.size;
		mem_alloc.memoryTypeIndex =
			FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (mem_alloc.memoryTypeIndex == UINT32_MAX ||
			vkAllocateMemory(m_device, &mem_alloc, nullptr, &staging.memory) != VK_SUCCESS)
			return false;
		vkBindBufferMemory(m_device, staging.buffer, staging.memory, 0);
		if (vkMapMemory(m_device, staging.memory, 0, VK_WHOLE_SIZE, 0, &staging.mapped) != VK_SUCCESS)
			return false;
	}
	m_staging_size = size;
	return true;
}

bool OverlayLayer::CreateSource(int width, int height)
{
	DestroySource();
	VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = m_format; // the swapchain's: blits copy the canvas's bytes as they are
	info.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(m_device, &info, nullptr, &m_source) != VK_SUCCESS)
	{
		m_source = VK_NULL_HANDLE;
		return false;
	}
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(m_device, m_source, &req);
	VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &alloc, nullptr, &m_source_memory) != VK_SUCCESS)
	{
		m_source_memory = VK_NULL_HANDLE;
		DestroySource();
		return false;
	}
	vkBindImageMemory(m_device, m_source, m_source_memory, 0);
	m_source_width = width;
	m_source_height = height;
	return true;
}

void OverlayLayer::DestroySource()
{
	if (m_source || m_source_memory)
		vkWaitForFences(m_device, NUM_SLOTS, m_fences.data(), VK_TRUE, UINT64_MAX);
	if (m_source)
		vkDestroyImage(m_device, m_source, nullptr);
	if (m_source_memory)
		vkFreeMemory(m_device, m_source_memory, nullptr);
	m_source = VK_NULL_HANDLE;
	m_source_memory = VK_NULL_HANDLE;
	m_source_width = m_source_height = 0;
	m_source_valid = false;
}

void OverlayLayer::Destroy()
{
	if (!m_device)
		return;
	{
		std::lock_guard lock(GraphicsQueueMutex());
		vkQueueWaitIdle(m_queue);
		if (m_swapchain)
			xrDestroySwapchain(m_swapchain);
	}
	m_swapchain = XR_NULL_HANDLE;
	m_images.clear();
	m_has_image = false;
	DestroySource();
	for (Staging& staging : m_staging)
	{
		if (staging.buffer)
			vkDestroyBuffer(m_device, staging.buffer, nullptr);
		if (staging.memory)
			vkFreeMemory(m_device, staging.memory, nullptr);
		staging = {};
	}
	m_staging_size = 0;
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

bool OverlayLayer::Begin(size_t* slot, uint32_t* index)
{
	*slot = m_slot;
	m_slot = (m_slot + 1) % NUM_SLOTS;
	vkWaitForFences(m_device, 1, &m_fences[*slot], VK_TRUE, UINT64_MAX);
	{
		std::lock_guard lock(GraphicsQueueMutex());
		if (XR_FAILED(xrAcquireSwapchainImage(m_swapchain, nullptr, index)))
			return false;
	}
	XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wait.timeout = XR_INFINITE_DURATION;
	if (XR_FAILED(xrWaitSwapchainImage(m_swapchain, &wait)))
		return false;

	vkResetFences(m_device, 1, &m_fences[*slot]);
	VkCommandBuffer cmd = m_cmd[*slot];
	vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &begin);
	return true;
}

void OverlayLayer::End(size_t slot, VkImage image)
{
	VkCommandBuffer cmd = m_cmd[slot];
	// The layout OpenXR expects a released colour image in.
	VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
		1, &barrier);
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

// From UNDEFINED (its old content is not needed) to TRANSFER_DST_OPTIMAL.
static void ToTransferDst(VkCommandBuffer cmd, VkImage image)
{
	VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	barrier.srcAccessMask = 0;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
		1, &barrier);
}

void OverlayLayer::Upload(const Canvas& canvas)
{
	if (!m_swapchain || canvas.Width() != m_width || canvas.Height() != m_height)
		return;
	const VkDeviceSize bytes = static_cast<VkDeviceSize>(m_width) * static_cast<VkDeviceSize>(m_height) * 4;
	if (!EnsureStaging(bytes))
		return;
	size_t slot;
	uint32_t index = 0;
	// The slot's last copy is done once Begin has waited for its fence.
	if (!Begin(&slot, &index))
		return;
	std::memcpy(m_staging[slot].mapped, canvas.Pixels(), static_cast<size_t>(bytes));
	VkCommandBuffer cmd = m_cmd[slot];
	const VkImage image = m_images[index].image;
	ToTransferDst(cmd, image);
	VkBufferImageCopy region{};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height), 1};
	vkCmdCopyBufferToImage(cmd, m_staging[slot].buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	End(slot, image);
}

void OverlayLayer::Compose(const Canvas* source, const std::vector<Blit>& blits)
{
	if (!m_swapchain)
		return;
	if (source && (source->Width() != m_source_width || source->Height() != m_source_height))
	{
		const VkDeviceSize bytes = static_cast<VkDeviceSize>(source->Width()) * static_cast<VkDeviceSize>(source->Height()) * 4;
		if (!EnsureStaging(bytes) || !CreateSource(source->Width(), source->Height()))
			return;
	}
	if (!m_source || (!source && !m_source_valid))
		return;
	size_t slot;
	uint32_t index = 0;
	if (!Begin(&slot, &index))
		return;
	VkCommandBuffer cmd = m_cmd[slot];
	VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if (source)
	{
		// After the blits of earlier submissions on this queue have read it.
		std::memcpy(m_staging[slot].mapped, source->Pixels(),
			static_cast<size_t>(m_source_width) * static_cast<size_t>(m_source_height) * 4);
		barrier.image = m_source;
		barrier.srcAccessMask = m_source_valid ? VK_ACCESS_TRANSFER_READ_BIT : 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
			1, &barrier);
		VkBufferImageCopy region{};
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {static_cast<uint32_t>(m_source_width), static_cast<uint32_t>(m_source_height), 1};
		vkCmdCopyBufferToImage(cmd, m_staging[slot].buffer, m_source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
			1, &barrier);
		m_source_valid = true;
	}

	// Transparent, then the parts over it.
	const VkImage image = m_images[index].image;
	ToTransferDst(cmd, image);
	const VkClearColorValue clear{};
	const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
	barrier.image = image;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
		1, &barrier);
	std::vector<VkImageBlit> regions;
	regions.reserve(blits.size());
	for (const Blit& b : blits)
	{
		VkImageBlit r{};
		r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		r.srcOffsets[0] = {b.from.offset.x, b.from.offset.y, 0};
		r.srcOffsets[1] = {b.from.offset.x + b.from.extent.width, b.from.offset.y + b.from.extent.height, 1};
		r.dstOffsets[0] = {b.to.offset.x, b.to.offset.y, 0};
		r.dstOffsets[1] = {b.to.offset.x + b.to.extent.width, b.to.offset.y + b.to.extent.height, 1};
		regions.push_back(r);
	}
	if (!regions.empty())
	{
		vkCmdBlitImage(cmd, m_source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			static_cast<uint32_t>(regions.size()), regions.data(), VK_FILTER_LINEAR);
	}
	End(slot, image);
}

const XrCompositionLayerBaseHeader* OverlayLayer::Layer()
{
	Part whole{{{0, 0}, {m_width, m_height}}, m_pose, m_size};
	Layer(whole);
	m_layer = whole.layer;
	return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_layer);
}

const XrCompositionLayerBaseHeader* OverlayLayer::Layer(Part& part)
{
	XrCompositionLayerQuad& layer = part.layer;
	layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
	// Premultiplied alpha (the canvas's), blended over the layers before it.
	layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	layer.space = m_xr->LocalSpace();
	layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	layer.subImage.swapchain = m_swapchain;
	layer.subImage.imageRect = part.rect;
	layer.pose = part.pose;
	layer.size = part.size;
	return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
}
