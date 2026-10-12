// SPDX-License-Identifier: GPL-3.0-or-later
// Libretro_PS5SX2: the GS renderer's side of RetroArch's Vulkan device
// (LibretroVulkan.cpp; used by GSDeviceVK.cpp and VKLoader.cpp under LIBRETRO).
//
// RetroArch owns the instance; the core creates the device inside RetroArch's
// negotiation callback, and RetroArch destroys it. The GS thread renders on it and
// hands each finished frame to retro_run, which gives it to RetroArch (set_image): the
// GS thread copies the frame into a free handover image, submits, and publishes it;
// retro_run takes the newest published image. At most one frame waits to be shown:
// the GS thread waits while one does, so the emulation runs at RetroArch's pace.
#pragma once

#define VK_NO_PROTOTYPES
#include "vulkan/vulkan.h"

namespace LibretroGS
{
// The instance and physical device RetroArch created, VK_NULL_HANDLE outside a core's
// negotiation; the device creation then uses them instead of its own.
VkInstance Instance();
VkPhysicalDevice PhysicalDevice();

// Whether RetroArch's driver is RADV (rather than ps5vk), from the GPU's name.
bool DriverIsRADV();

// vkGetInstanceProcAddr for the core: RetroArch's, with vkCreateDevice adding what
// RetroArch requires of the device and the queue calls taking RetroArch's queue lock.
PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* name);

// Handover images: a free one to copy the frame into (-1 when the core is stopping), and
// then the frame published once its copy is submitted. Width and height are the image's.
int AcquireHandoverSlot();
void PublishHandoverSlot(int slot, VkImage image, VkImageView view, VkFormat format, unsigned width, unsigned height);
// Abandons a slot acquired but not published.
void ReleaseHandoverSlot(int slot);
// Slots a GSDeviceVK keeps an image for.
constexpr int HANDOVER_SLOTS = 8;
} // namespace LibretroGS
