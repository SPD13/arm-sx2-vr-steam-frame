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

	const VkDeviceSize size = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4;
	for (Staging& staging : m_staging)
	{
		VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		buffer_info.size = size;
		buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(device, &buffer_info, nullptr, &staging.buffer) != VK_SUCCESS)
			return false;
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(device, staging.buffer, &req);
		VkMemoryAllocateInfo mem_alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mem_alloc.allocationSize = req.size;
		mem_alloc.memoryTypeIndex =
			FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (mem_alloc.memoryTypeIndex == UINT32_MAX ||
			vkAllocateMemory(device, &mem_alloc, nullptr, &staging.memory) != VK_SUCCESS)
			return false;
		vkBindBufferMemory(device, staging.buffer, staging.memory, 0);
		if (vkMapMemory(device, staging.memory, 0, VK_WHOLE_SIZE, 0, &staging.mapped) != VK_SUCCESS)
			return false;
	}
	return true;
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
	for (Staging& staging : m_staging)
	{
		if (staging.buffer)
			vkDestroyBuffer(m_device, staging.buffer, nullptr);
		if (staging.memory)
			vkFreeMemory(m_device, staging.memory, nullptr);
		staging = {};
	}
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

void OverlayLayer::Upload(const Canvas& canvas)
{
	if (!m_swapchain || canvas.Width() != m_width || canvas.Height() != m_height)
		return;

	const size_t slot = m_slot;
	m_slot = (m_slot + 1) % NUM_SLOTS;
	vkWaitForFences(m_device, 1, &m_fences[slot], VK_TRUE, UINT64_MAX);
	std::memcpy(m_staging[slot].mapped, canvas.Pixels(), static_cast<size_t>(m_width) * static_cast<size_t>(m_height) * 4);

	uint32_t index = 0;
	{
		std::lock_guard lock(GraphicsQueueMutex());
		if (XR_FAILED(xrAcquireSwapchainImage(m_swapchain, nullptr, &index)))
			return;
	}
	XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wait.timeout = XR_INFINITE_DURATION;
	if (XR_FAILED(xrWaitSwapchainImage(m_swapchain, &wait)))
		return;

	vkResetFences(m_device, 1, &m_fences[slot]);
	VkCommandBuffer cmd = m_cmd[slot];
	vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &begin);

	const VkImage image = m_images[index].image;
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

	VkBufferImageCopy region{};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height), 1};
	vkCmdCopyBufferToImage(cmd, m_staging[slot].buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	// The layout OpenXR expects a released colour image in.
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
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
