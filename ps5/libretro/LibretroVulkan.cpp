// SPDX-License-Identifier: GPL-3.0-or-later
// Libretro_PS5SX2: the GS on RetroArch's Vulkan device (see libretro_gs.h).

#include "libretro_gs.h"
#include "libretro_host.h"
#include "libretro_core.h"

#include "libretro.h"
#include "libretro_vulkan.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
// What RetroArch passed to create_device.
VkInstance s_instance = VK_NULL_HANDLE;
VkPhysicalDevice s_gpu = VK_NULL_HANDLE;
PFN_vkGetInstanceProcAddr s_frontend_gipa = nullptr;
std::vector<const char*> s_required_extensions;
std::vector<const char*> s_required_layers;
VkPhysicalDeviceFeatures s_required_features = {};

// The device the GS created, captured as vkCreateDevice returns it.
VkDevice s_device = VK_NULL_HANDLE;
uint32_t s_queue_family = 0;

PFN_vkCreateDevice s_create_device = nullptr;
PFN_vkGetDeviceProcAddr s_get_device_proc_addr = nullptr;
PFN_vkQueueSubmit s_queue_submit = nullptr;
PFN_vkQueueSubmit2 s_queue_submit2 = nullptr;
PFN_vkQueueWaitIdle s_queue_wait_idle = nullptr;
PFN_vkDeviceWaitIdle s_device_wait_idle = nullptr;

// RetroArch's interface, from context_reset on; until then (the GS's start-up work runs
// while RetroArch waits in create_device) the core's own lock stands in.
retro_hw_render_interface_vulkan* s_iface = nullptr;
std::mutex s_queue_mutex;

// What the hand-over costs, logged every two seconds from retro_run (the time spent waiting for the queue
// RetroArch shares, the time in the submits that hold it, the GS waiting for a free slot, retro_run waiting
// for a frame and for RetroArch's last use of an image): to tell which side holds the other up.
struct Stats
{
	std::atomic<uint64_t> lock_wait_ns{0}, lock_wait_max_ns{0}, lock_calls{0};
	std::atomic<uint64_t> submit_ns{0}, submit_max_ns{0};
	std::atomic<uint64_t> slot_wait_ns{0}, slot_wait_max_ns{0}, published{0};
	std::atomic<uint64_t> frame_wait_ns{0}, sync_wait_ns{0}, sync_wait_max_ns{0}, dupes{0}, taken{0};
};
Stats s_stats;
inline uint64_t now_ns()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline void note_max(std::atomic<uint64_t>& max, uint64_t value)
{
	uint64_t seen = max.load(std::memory_order_relaxed);
	while (value > seen && !max.compare_exchange_weak(seen, value, std::memory_order_relaxed))
	{
	}
}

struct QueueLock
{
	retro_hw_render_interface_vulkan* iface;
	QueueLock()
		: iface(s_iface)
	{
		const uint64_t t0 = now_ns();
		if (iface)
			iface->lock_queue(iface->handle);
		else
			s_queue_mutex.lock();
		const uint64_t waited = now_ns() - t0;
		s_stats.lock_wait_ns += waited;
		s_stats.lock_calls++;
		note_max(s_stats.lock_wait_max_ns, waited);
	}
	~QueueLock()
	{
		if (iface)
			iface->unlock_queue(iface->handle);
		else
			s_queue_mutex.unlock();
	}
};

void add_unique(std::vector<const char*>& list, const char* name)
{
	for (const char* have : list)
		if (!std::strcmp(have, name))
			return;
	list.push_back(name);
}

VKAPI_ATTR VkResult VKAPI_CALL create_device(VkPhysicalDevice gpu, const VkDeviceCreateInfo* info,
	const VkAllocationCallbacks* allocator, VkDevice* device)
{
	VkDeviceCreateInfo merged = *info;
	std::vector<const char*> extensions(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);
	std::vector<const char*> layers(info->ppEnabledLayerNames, info->ppEnabledLayerNames + info->enabledLayerCount);
	for (const char* name : s_required_extensions)
		add_unique(extensions, name);
	for (const char* name : s_required_layers)
		add_unique(layers, name);
	merged.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	merged.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
	merged.enabledLayerCount = static_cast<uint32_t>(layers.size());
	merged.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();

	// RetroArch's required features, on top of the GS's: in pEnabledFeatures, or in a
	// VkPhysicalDeviceFeatures2 on the pNext chain when the GS passes its features there.
	VkPhysicalDeviceFeatures features = {};
	VkPhysicalDeviceFeatures* target = nullptr;
	if (info->pEnabledFeatures)
	{
		features = *info->pEnabledFeatures;
		merged.pEnabledFeatures = &features;
		target = &features;
	}
	else
	{
		for (auto* next = static_cast<VkBaseOutStructure*>(const_cast<void*>(info->pNext)); next; next = next->pNext)
			if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
				target = &reinterpret_cast<VkPhysicalDeviceFeatures2*>(next)->features;
		if (!target)
		{
			merged.pEnabledFeatures = &features;
			target = &features;
		}
	}
	const VkBool32* required = reinterpret_cast<const VkBool32*>(&s_required_features);
	VkBool32* enabled = reinterpret_cast<VkBool32*>(target);
	for (size_t i = 0; i < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); i++)
		if (required[i])
			enabled[i] = VK_TRUE;

	const VkResult res = s_create_device(gpu, &merged, allocator, device);
	if (res == VK_SUCCESS)
	{
		s_device = *device;
		s_queue_family = info->queueCreateInfoCount ? info->pQueueCreateInfos[0].queueFamilyIndex : 0;
		lr_logf(LR_LOG_INFO, "vulkan: the GS created the device (%u extensions, %u of them RetroArch's)",
			merged.enabledExtensionCount, static_cast<unsigned>(s_required_extensions.size()));
	}
	else
		lr_logf(LR_LOG_ERROR, "vulkan: vkCreateDevice failed (%d)", static_cast<int>(res));
	return res;
}

VKAPI_ATTR VkResult VKAPI_CALL queue_submit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence)
{
	QueueLock lock;
	const uint64_t t0 = now_ns();
	const VkResult res = s_queue_submit(queue, count, submits, fence);
	const uint64_t took = now_ns() - t0;
	s_stats.submit_ns += took;
	note_max(s_stats.submit_max_ns, took);
	return res;
}

VKAPI_ATTR VkResult VKAPI_CALL queue_submit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence)
{
	QueueLock lock;
	return s_queue_submit2(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL queue_wait_idle(VkQueue queue)
{
	QueueLock lock;
	return s_queue_wait_idle(queue);
}

VKAPI_ATTR VkResult VKAPI_CALL device_wait_idle(VkDevice device)
{
	QueueLock lock;
	return s_device_wait_idle(device);
}

PFN_vkVoidFunction wrap(const char* name, PFN_vkVoidFunction fn);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_device_proc_addr(VkDevice device, const char* name)
{
	return wrap(name, s_get_device_proc_addr(device, name));
}

PFN_vkVoidFunction wrap(const char* name, PFN_vkVoidFunction fn)
{
	if (!fn)
		return fn;
#define LR_WRAP(vkname, ours, saved) \
	if (!std::strcmp(name, #vkname)) \
	{ \
		saved = reinterpret_cast<PFN_##vkname>(fn); \
		return reinterpret_cast<PFN_vkVoidFunction>(ours); \
	}
	LR_WRAP(vkCreateDevice, create_device, s_create_device)
	LR_WRAP(vkGetDeviceProcAddr, get_device_proc_addr, s_get_device_proc_addr)
	LR_WRAP(vkQueueSubmit, queue_submit, s_queue_submit)
	LR_WRAP(vkQueueWaitIdle, queue_wait_idle, s_queue_wait_idle)
	LR_WRAP(vkDeviceWaitIdle, device_wait_idle, s_device_wait_idle)
	if (!std::strcmp(name, "vkQueueSubmit2") || !std::strcmp(name, "vkQueueSubmit2KHR"))
	{
		s_queue_submit2 = reinterpret_cast<PFN_vkQueueSubmit2>(fn);
		return reinterpret_cast<PFN_vkVoidFunction>(queue_submit2);
	}
#undef LR_WRAP
	return fn;
}

// The handover images (libretro_gs.h).
enum class SlotState
{
	Free,
	Writing,
	Waiting, // published, not yet given to RetroArch
	Shown, // given to RetroArch at sync index `sync`
};

struct Slot
{
	SlotState state = SlotState::Free;
	unsigned sync = 0;
	unsigned vsyncs = 1; // emulated vsyncs from the frame published before this one to this
	uint64_t period_ns = 0; // the time the GS took to make it, without what it waited for a free slot (0: not known)
	unsigned width = 0, height = 0;
	retro_vulkan_image image = {};
};

std::atomic<unsigned> s_vsync_count{0};
// The GS thread's own bookkeeping of when a frame was ready and what it waited for a slot: the period of the frames the
// GS makes, apart from the pace RetroArch sets by taking them (a frame is published once the last was taken).
uint64_t s_ready_ns = 0, s_wait_ns = 0, s_prev_ready_ns = 0, s_prev_wait_ns = 0;
retro_environment_t s_environ = nullptr;
std::mutex s_slots_mutex;
std::condition_variable s_slots_cv;
Slot s_slots[LibretroGS::HANDOVER_SLOTS];
int s_waiting = -1; // the slot published and not yet shown
bool s_stopping = false;
} // namespace

VkInstance LibretroGS::Instance()
{
	return s_instance;
}

VkPhysicalDevice LibretroGS::PhysicalDevice()
{
	return s_gpu;
}

bool LibretroGS::DriverIsRADV()
{
	static int radv = -1;
	if (radv < 0 && s_frontend_gipa && s_instance != VK_NULL_HANDLE && s_gpu != VK_NULL_HANDLE)
	{
		const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
			s_frontend_gipa(s_instance, "vkGetPhysicalDeviceProperties"));
		VkPhysicalDeviceProperties p = {};
		if (properties)
			properties(s_gpu, &p);
		radv = std::strstr(p.deviceName, "RADV") != nullptr;
		lr_logf(LR_LOG_INFO, "vulkan: %s (%s)", p.deviceName, radv ? "RADV" : "ps5vk");
	}
	return radv > 0;
}

PFN_vkVoidFunction VKAPI_CALL LibretroGS::GetInstanceProcAddr(VkInstance instance, const char* name)
{
	return s_frontend_gipa ? wrap(name, s_frontend_gipa(instance, name)) : nullptr;
}

int LibretroGS::AcquireHandoverSlot()
{
	std::unique_lock<std::mutex> lock(s_slots_mutex);
	const uint64_t wait_start = now_ns();
	s_ready_ns = wait_start;
	for (;;)
	{
		if (s_stopping)
			return -1;
		// The pace: while a frame waits to be shown, the next waits for it.
		if (s_waiting < 0 || lr_frames_unpaced())
		{
			for (int i = 0; i < HANDOVER_SLOTS; i++)
			{
				if (s_slots[i].state == SlotState::Free)
				{
					s_slots[i].state = SlotState::Writing;
					const uint64_t waited = now_ns() - wait_start;
					s_wait_ns = waited;
					s_stats.slot_wait_ns += waited;
					note_max(s_stats.slot_wait_max_ns, waited);
					return i;
				}
			}
		}
		s_slots_cv.wait(lock);
	}
}

void LibretroGS::ReleaseHandoverSlot(int slot)
{
	std::lock_guard<std::mutex> lock(s_slots_mutex);
	s_slots[slot].state = SlotState::Free;
	s_slots_cv.notify_all();
}

void LibretroGS::NoteVsync()
{
	s_vsync_count.fetch_add(1, std::memory_order_relaxed);
}

void LibretroGS::PublishHandoverSlot(int slot, VkImage image, VkImageView view, VkFormat format, unsigned width, unsigned height)
{
	std::lock_guard<std::mutex> lock(s_slots_mutex);
	static unsigned last_width = 0, last_height = 0;
	if (width != last_width || height != last_height)
	{
		lr_logf(LR_LOG_INFO, "video: frames of %ux%u", width, height);
		last_width = width;
		last_height = height;
	}
	Slot& s = s_slots[slot];
	s.width = width;
	s.height = height;
	s.image = {};
	s.image.image_view = view;
	s.image.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	s.image.create_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, image, VK_IMAGE_VIEW_TYPE_2D, format,
		{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
			VK_COMPONENT_SWIZZLE_IDENTITY},
		{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	// The vsyncs this frame covers: those since the last one published, and those of a frame it replaces.
	s.vsyncs = std::max(1u, s_vsync_count.exchange(0, std::memory_order_relaxed)) + (s_waiting >= 0 ? s_slots[s_waiting].vsyncs : 0);
	{
		const uint64_t gap = s_prev_ready_ns && s_ready_ns > s_prev_ready_ns ? s_ready_ns - s_prev_ready_ns : 0;
		s.period_ns = gap > s_prev_wait_ns ? gap - s_prev_wait_ns : 0;
		if (s_waiting >= 0)
			s.period_ns += s_slots[s_waiting].period_ns;
		s_prev_ready_ns = s_ready_ns;
		s_prev_wait_ns = s_wait_ns;
	}
	// A frame still waiting (unpaced: fast forward, a save state) is replaced by this one.
	if (s_waiting >= 0)
		s_slots[s_waiting].state = SlotState::Free;
	s.state = SlotState::Waiting;
	s_stats.published++;
	s_waiting = slot;
	s_slots_cv.notify_all();
}

// retro_run's side (libretro_core.h).
bool lr_vk_present(retro_video_refresh_t video_cb, int wait_ms)
{
	retro_hw_render_interface_vulkan* iface = s_iface;
	if (!iface)
		return false;
	int slot;
	{
		std::unique_lock<std::mutex> lock(s_slots_mutex);
		const uint64_t t0 = now_ns();
		if (s_waiting < 0 && wait_ms > 0)
			s_slots_cv.wait_for(lock, std::chrono::milliseconds(wait_ms), [] { return s_waiting >= 0 || s_stopping; });
		s_stats.frame_wait_ns += now_ns() - t0;
		slot = s_waiting;
		if (slot < 0)
		{
			s_stats.dupes++;
			return false;
		}
	}

	// The index RetroArch is about to use: once it has finished with what that index
	// showed last time, that image is free again.
	const uint64_t sync_t0 = now_ns();
	iface->wait_sync_index(iface->handle);
	{
		const uint64_t waited = now_ns() - sync_t0;
		s_stats.sync_wait_ns += waited;
		note_max(s_stats.sync_wait_max_ns, waited);
	}
	s_stats.taken++;
	{
		static uint64_t last_log = 0;
		const uint64_t now = now_ns();
		if (!last_log)
			last_log = now;
		if (now - last_log >= 2000000000ull)
		{
			const double sec = double(now - last_log) / 1e9;
			auto ms = [](std::atomic<uint64_t>& v) { return double(v.exchange(0)) / 1e6; };
			const uint64_t calls = s_stats.lock_calls.exchange(0);
			lr_logf(LR_LOG_INFO,
				"handover %.1fs: taken %llu dupes %llu published %llu | queue lock: %llu calls, waited %.1f ms (max %.2f), submits held it %.1f ms (max %.2f) | GS waited %.1f ms for a slot (max %.2f) | retro_run waited %.1f ms for a frame, %.1f ms (max %.2f) for the sync index",
				sec, (unsigned long long)s_stats.taken.exchange(0), (unsigned long long)s_stats.dupes.exchange(0),
				(unsigned long long)s_stats.published.exchange(0), (unsigned long long)calls, ms(s_stats.lock_wait_ns),
				double(s_stats.lock_wait_max_ns.exchange(0)) / 1e6, ms(s_stats.submit_ns), double(s_stats.submit_max_ns.exchange(0)) / 1e6,
				ms(s_stats.slot_wait_ns), double(s_stats.slot_wait_max_ns.exchange(0)) / 1e6, ms(s_stats.frame_wait_ns),
				ms(s_stats.sync_wait_ns), double(s_stats.sync_wait_max_ns.exchange(0)) / 1e6);
			last_log = now;
		}
	}
	const unsigned sync = iface->get_sync_index(iface->handle);

	std::lock_guard<std::mutex> lock(s_slots_mutex);
	if (s_waiting != slot)
		slot = s_waiting; // replaced meanwhile by a newer frame
	if (slot < 0)
		return false;
	for (Slot& s : s_slots)
		if (s.state == SlotState::Shown && s.sync == sync)
			s.state = SlotState::Free;
	Slot& s = s_slots[slot];
	s.state = SlotState::Shown;
	s.sync = sync;
	s_waiting = -1;
	iface->set_image(iface->handle, &s.image, 0, nullptr, iface->queue_index);
	// Private to the PS5 port of RetroArch: how many emulated vsyncs this frame covers (unknown elsewhere: ignored).
	if (s_environ)
	{
		// {the vsyncs the frame covers, the microseconds the GS took to make it (0: not known)}
		unsigned info[2] = {s.vsyncs, static_cast<unsigned>(std::min<uint64_t>(s.period_ns / 1000, 2000000))};
		s_environ(0x25002, info);
	}
	video_cb(RETRO_HW_FRAME_BUFFER_VALID, s.width, s.height, 0);
	s_slots_cv.notify_all();
	return true;
}

void lr_vk_stop_handover(bool stopping)
{
	std::lock_guard<std::mutex> lock(s_slots_mutex);
	s_stopping = stopping;
	if (stopping)
	{
		for (Slot& s : s_slots)
			s = Slot();
		s_waiting = -1;
	}
	s_slots_cv.notify_all();
}

void lr_vk_wake_handover()
{
	std::lock_guard<std::mutex> lock(s_slots_mutex);
	s_slots_cv.notify_all();
}

// The negotiation (libretro_core.h).
static bool (*s_open_gs)() = nullptr;

void lr_vk_set_open_gs(bool (*open_gs)())
{
	s_open_gs = open_gs;
}

static bool lr_create_device(retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu,
	VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char** required_device_extensions,
	unsigned num_required_device_extensions, const char** required_device_layers, unsigned num_required_device_layers,
	const VkPhysicalDeviceFeatures* required_features)
{
	s_instance = instance;
	s_gpu = gpu;
	s_frontend_gipa = get_instance_proc_addr;
	s_required_extensions.assign(required_device_extensions, required_device_extensions + num_required_device_extensions);
	s_required_layers.assign(required_device_layers, required_device_layers + num_required_device_layers);
	s_required_features = required_features ? *required_features : VkPhysicalDeviceFeatures{};
	s_device = VK_NULL_HANDLE;
	lr_vk_stop_handover(false);

	// The emulator starts here: its GS creates the device on the frontend's instance.
	if (!s_open_gs || !s_open_gs() || s_device == VK_NULL_HANDLE)
	{
		lr_logf(LR_LOG_ERROR, "vulkan: the GS did not create a device");
		return false;
	}

	auto get_device_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(
		reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_instance_proc_addr(instance, "vkGetDeviceProcAddr"))(s_device, "vkGetDeviceQueue"));
	VkQueue queue = VK_NULL_HANDLE;
	get_device_queue(s_device, s_queue_family, 0, &queue);

	if (gpu == VK_NULL_HANDLE)
		gpu = s_gpu;
	context->gpu = s_gpu;
	context->device = s_device;
	context->queue = queue;
	context->queue_family_index = s_queue_family;
	context->presentation_queue = queue;
	context->presentation_queue_family_index = s_queue_family;
	return true;
}

static void lr_destroy_device()
{
}

static const VkApplicationInfo* lr_application_info()
{
	static VkApplicationInfo info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	info.pApplicationName = "PS5SX2";
	info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	info.pEngineName = "PCSX2";
	info.engineVersion = VK_MAKE_VERSION(2, 0, 0);
	info.apiVersion = VK_API_VERSION_1_1;
	return &info;
}

static retro_hw_render_context_negotiation_interface_vulkan s_negotiation = {
	RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
	RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION,
	lr_application_info,
	lr_create_device,
	lr_destroy_device,
};

bool lr_vk_negotiate(retro_environment_t environ_cb)
{
	s_environ = environ_cb;
	// Version 1: create_device, which hands over the frontend's required features.
	s_negotiation.interface_version = 1;
	return environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, &s_negotiation);
}

bool lr_vk_context_reset(retro_environment_t environ_cb)
{
	retro_hw_render_interface_vulkan* iface = nullptr;
	if (!environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, reinterpret_cast<void**>(&iface)) || !iface ||
		iface->interface_version != RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION)
	{
		lr_logf(LR_LOG_ERROR, "vulkan: RetroArch gave no usable render interface");
		return false;
	}
	// Taken under the core's lock, so a submit holding it finishes before RetroArch's lock
	// takes over.
	std::lock_guard<std::mutex> lock(s_queue_mutex);
	s_iface = iface;
	return true;
}

void lr_vk_context_destroy()
{
	retro_hw_render_interface_vulkan* iface = s_iface;
	if (iface)
	{
		// RetroArch replays the last image it was given; it is about to go.
		iface->set_image(iface->handle, nullptr, 0, nullptr, iface->queue_index);
		iface->wait_sync_index(iface->handle);
	}
	std::lock_guard<std::mutex> lock(s_queue_mutex);
	s_iface = nullptr;
}

void lr_vk_forget_device()
{
	s_device = VK_NULL_HANDLE;
	s_instance = VK_NULL_HANDLE;
	s_gpu = VK_NULL_HANDLE;
}
