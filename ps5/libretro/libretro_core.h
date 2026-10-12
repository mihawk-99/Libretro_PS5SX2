// SPDX-License-Identifier: GPL-3.0-or-later
// Libretro_PS5SX2: between libretro.cpp and the Vulkan glue (LibretroVulkan.cpp).
#pragma once

#include "libretro.h"

// The context negotiation (create_device starts the emulator: open_gs boots the VM, whose
// GS creates the device) and RetroArch's render interface.
bool lr_vk_negotiate(retro_environment_t environ_cb);
void lr_vk_set_open_gs(bool (*open_gs)());
bool lr_vk_context_reset(retro_environment_t environ_cb);
void lr_vk_context_destroy();
void lr_vk_forget_device();

// retro_run: hands RetroArch the newest frame the GS finished, waiting up to wait_ms for
// one; false when there was none (the caller repeats the last frame).
bool lr_vk_present(retro_video_refresh_t video_cb, int wait_ms);
// Stops (true) the handover, waking a GS thread waiting for a slot, or starts it again.
void lr_vk_stop_handover(bool stopping);
// Wakes a GS thread waiting for a slot after lr_frames_unpaced changed.
void lr_vk_wake_handover();

// True while the GS must not wait for retro_run (fast forward, or retro_run waiting on the
// emulation itself: a save state, a reset); libretro.cpp.
bool lr_frames_unpaced();
