// SPDX-License-Identifier: GPL-3.0-or-later
// Libretro_PS5SX2: the core options (libretro.cpp). Kept in a file of their own, as libretro
// cores keep libretro_core_options.h, so PS5 RetroArch's WebUI catalog reads them
// (its tools/generate-core-metadata.py).
#pragma once

#include "libretro.h"

#include <cstddef>

static retro_core_option_v2_category option_cats_us[] = {
	{"video", "Video", "Resolution and picture."},
	{"system", "System", "BIOS and start-up."},
	{nullptr, nullptr, nullptr},
};

static retro_core_option_v2_definition option_defs_us[] = {
	{"ps5sx2_upscale", "Internal Resolution", nullptr,
		"How many times the PS2's resolution the hardware renderer draws at.", nullptr, "video",
		{{"1", "Native (PS2)"}, {"2", "2x (~720p)"}, {"3", "3x (~1080p)"}, {"4", "4x (~1440p)"}, {nullptr, nullptr}}, "2"},
	{"ps5sx2_aspect", "Aspect Ratio", nullptr, "The shape of the picture.", nullptr, "video",
		{{"4:3", "4:3"}, {"16:9", "16:9"}, {nullptr, nullptr}}, "4:3"},
	{"ps5sx2_renderer", "Renderer", nullptr, "Hardware draws on the GPU; Software is slower and more accurate.", nullptr, "video",
		{{"hardware", "Hardware (Vulkan)"}, {"software", "Software"}, {nullptr, nullptr}}, "hardware"},
	{"ps5sx2_fast_boot", "Fast Boot", nullptr, "Skip the BIOS intro and start the game directly.", nullptr, "system",
		{{"enabled", "On"}, {"disabled", "Off"}, {nullptr, nullptr}}, "enabled"},
	{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {{nullptr, nullptr}}, nullptr},
};

static retro_core_options_v2 options_us = {option_cats_us, option_defs_us};
