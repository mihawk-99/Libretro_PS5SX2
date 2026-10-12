// SPDX-License-Identifier: GPL-3.0-or-later
// Libretro_PS5SX2: what the core's own files share (libretro.cpp, LibretroHost.cpp,
// LibretroAudio.cpp and GSDeviceVK's LIBRETRO present).
#pragma once

#include <string>

enum LrLogLevel
{
	LR_LOG_DEBUG = 0,
	LR_LOG_INFO,
	LR_LOG_WARN,
	LR_LOG_ERROR
};

// RetroArch's log, and a message on its on-screen display.
void lr_logf(LrLogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void lr_osd(const std::string& message, float seconds);

// Run on the CPU thread at each vsync (Host::PumpMessagesOnCPUThread): the controllers'
// state, and the requests retro_run leaves for the emulation (reset, a disc change).
void lr_cpu_thread_pump();

// What SPU2 produced since the last call, as interleaved 16-bit stereo (LibretroAudio.cpp).
unsigned lr_audio_read(short* out, unsigned max_frames);
