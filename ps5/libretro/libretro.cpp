// SPDX-License-Identifier: GPL-3.0-or-later
// Libretro_PS5SX2: PS5SX2's emulator as a libretro core for PS5 RetroArch.
//
// Threads. The emulation keeps PCSX2's own: the CPU thread (EE, IOP, VUs) runs
// VMManager::Execute, and the GS thread (MTGS) renders. RetroArch's thread calls the
// entry points below. RetroArch creates the Vulkan instance and asks the core for a device
// (create_device): that is where the emulator boots, since its GS creates the device.
// The CPU thread then waits for RetroArch's context_reset before the game runs. Each
// finished frame is handed from the GS thread to retro_run (LibretroVulkan.cpp), and the
// GS waits while a frame is still unshown, so the game runs at RetroArch's pace. Work the
// frontend needs done on the emulation (reset, closing the GS) runs on the CPU thread
// between its frames, with the pacing off meanwhile so neither side waits on the other.
//
// Files. The BIOS and memory cards are LRPS2's, shared: <system>/pcsx2/bios and
// <system>/pcsx2/memcards. The core's own data is <system>/ps5sx2: PCSX2's game
// database in resources/, the shader cache in cache/.

#include "libretro.h"
#include "libretro_core.h"
#include "libretro_gs.h"
#include "libretro_host.h"

#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/HostSys.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/Threading.h"
#include "pcsx2/CDVD/CDVDcommon.h"
#include "pcsx2/Config.h"
#include "pcsx2/Host.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/Memory.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/SIO/Pad/PadDualshock2.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/ps2/BiosTools.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#define LR_EXPORT extern "C" __attribute__((visibility("default")))

extern "C" int g_orbis_code_direct; // Memory.cpp: the recompilers' code in direct memory made executable

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;
static retro_set_rumble_state_t rumble_cb;

// Logging (libretro_host.h).
void lr_logf(LrLogLevel level, const char* fmt, ...)
{
	char line[1024];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	static const retro_log_level levels[] = {RETRO_LOG_DEBUG, RETRO_LOG_INFO, RETRO_LOG_WARN, RETRO_LOG_ERROR};
	if (log_cb)
		log_cb(levels[level], "[PS5SX2] %s\n", line);
	else
		std::fprintf(stderr, "[PS5SX2] %s\n", line);
}

void lr_osd(const std::string& message, float seconds)
{
	if (!environ_cb || message.empty())
		return;
	retro_message_ext msg = {};
	msg.msg = message.c_str();
	msg.duration = static_cast<unsigned>(seconds * 1000.0f);
	msg.priority = 1;
	msg.level = RETRO_LOG_INFO;
	msg.target = RETRO_MESSAGE_TARGET_ALL;
	msg.type = RETRO_MESSAGE_TYPE_NOTIFICATION;
	msg.progress = -1;
	environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg);
}

// Core options.
static retro_core_option_v2_category s_option_categories[] = {
	{"video", "Video", "Resolution and picture."},
	{"system", "System", "BIOS and start-up."},
	{nullptr, nullptr, nullptr},
};

static retro_core_option_v2_definition s_option_definitions[] = {
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

static retro_core_options_v2 s_options = {s_option_categories, s_option_definitions};

static std::string option(const char* key, const char* fallback)
{
	retro_variable var = {key, nullptr};
	if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		return var.value;
	return fallback;
}

// Folders.
static std::string s_system_dir;
static std::string s_core_dir; // <system>/ps5sx2
static std::string s_bios_dir; // <system>/pcsx2/bios, shared with LRPS2

static void find_folders()
{
	const char* dir = nullptr;
	s_system_dir = (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir) ? dir : ".";
	s_core_dir = Path::Combine(s_system_dir, "ps5sx2");
	s_bios_dir = Path::Combine(s_system_dir, "pcsx2/bios");
}

// The BIOS: the one matching the game's region when there are several, else any.
static std::string pick_bios()
{
	FileSystem::FindResultsArray files;
	FileSystem::FindFiles(s_bios_dir.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &files);
	std::string any, usa;
	for (const FILESYSTEM_FIND_DATA& fd : files)
	{
		u32 version = 0, region = 0;
		std::string description, zone;
		if (fd.Size < 4 * 1024 * 1024 || !IsBIOS(fd.FileName.c_str(), version, description, region, zone))
			continue;
		const std::string name(Path::GetFileName(fd.FileName));
		lr_logf(LR_LOG_INFO, "BIOS: %s (%s)", name.c_str(), description.c_str());
		if (any.empty())
			any = name;
		if (usa.empty() && zone == "USA")
			usa = name;
	}
	return usa.empty() ? any : usa;
}

// Settings: PCSX2's defaults, the PS5 port's choices for the recompilers, and the options.
static MemorySettingsInterface s_settings;

static void apply_options(SettingsInterface& si)
{
	const int upscale = std::clamp(std::atoi(option("ps5sx2_upscale", "2").c_str()), 1, 4);
	si.SetFloatValue("EmuCore/GS", "upscale_multiplier", static_cast<float>(upscale));
	const bool software = option("ps5sx2_renderer", "hardware") == "software";
	si.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(software ? GSRendererType::SW : GSRendererType::VK));
	si.SetBoolValue("EmuCore", "EnableFastBoot", option("ps5sx2_fast_boot", "enabled") == "enabled");
}

static void build_settings(const std::string& bios)
{
	s_settings.Clear();
	VMManager::SetDefaultSettings(s_settings, true, true, true, false, false);
	s_settings.SetStringValue("Folders", "Bios", s_bios_dir.c_str());
	s_settings.SetStringValue("Folders", "MemoryCards", Path::Combine(s_system_dir, "pcsx2/memcards").c_str());
	s_settings.SetStringValue("Filenames", "BIOS", bios.c_str());
	// The PS5 port's: recompilers on, fastmem off (needs the elevated app's memory), the EE
	// cache off, the speed hacks it ships with, no MTVU (the VU thread).
	s_settings.SetBoolValue("EmuCore/CPU/Recompiler", "EnableEE", true);
	s_settings.SetBoolValue("EmuCore/CPU/Recompiler", "EnableIOP", true);
	s_settings.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU0", true);
	s_settings.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU1", true);
	s_settings.SetBoolValue("EmuCore/CPU/Recompiler", "EnableFastmem", false);
	s_settings.SetBoolValue("EmuCore/CPU/Recompiler", "EnableEECache", false);
	s_settings.SetBoolValue("EmuCore/Speedhacks", "WaitLoop", true);
	s_settings.SetBoolValue("EmuCore/Speedhacks", "IntcStat", true);
	s_settings.SetBoolValue("EmuCore/Speedhacks", "vu1Instant", true);
	s_settings.SetBoolValue("EmuCore/Speedhacks", "vuFlagHack", true);
	s_settings.SetBoolValue("EmuCore/Speedhacks", "vuThread", false);
	s_settings.SetIntValue("EmuCore/GS", "extrathreads", 2);
	// RetroArch paces the frames and does achievements.
	s_settings.SetBoolValue("EmuCore/GS", "VsyncEnable", false);
	s_settings.SetBoolValue("Achievements", "Enabled", false);
	s_settings.SetStringValue("Pad1", "Type", "DualShock2");
	s_settings.SetStringValue("Pad2", "Type", "DualShock2");
	apply_options(s_settings);
}

// The emulation thread.
enum class Boot
{
	None,
	Running,
	Failed,
};

static Threading::Thread s_cpu_thread;
static std::string s_game_path;
static std::mutex s_cpu_mutex;
static std::condition_variable s_cpu_cv;
static Boot s_boot = Boot::None;
static bool s_go = false; // context_reset happened: the game may run
static bool s_stop = false;
static std::deque<std::function<void()>> s_cpu_tasks;
static std::atomic<int> s_unpaced{0};
static bool s_gs_closed = false;

bool lr_frames_unpaced()
{
	return s_unpaced.load(std::memory_order_acquire) > 0;
}

static bool run_cpu_tasks()
{
	bool ran = false;
	for (;;)
	{
		std::function<void()> task;
		{
			std::lock_guard<std::mutex> lock(s_cpu_mutex);
			if (s_cpu_tasks.empty())
				break;
			task = std::move(s_cpu_tasks.front());
			s_cpu_tasks.pop_front();
		}
		task();
		ran = true;
	}
	return ran;
}

// Runs task on the CPU thread and waits for it: at its next vsync while the game runs, at
// once while it waits.
static void run_on_cpu_and_wait(std::function<void()> task)
{
	if (!s_cpu_thread.Joinable())
	{
		task();
		return;
	}
	s_unpaced.fetch_add(1, std::memory_order_acq_rel);
	lr_vk_wake_handover();
	std::mutex done_mutex;
	std::condition_variable done_cv;
	bool done = false;
	{
		std::lock_guard<std::mutex> lock(s_cpu_mutex);
		s_cpu_tasks.push_back([&]() {
			task();
			std::lock_guard<std::mutex> done_lock(done_mutex);
			done = true;
			done_cv.notify_all();
		});
		s_cpu_cv.notify_all();
	}
	{
		std::unique_lock<std::mutex> lock(done_mutex);
		done_cv.wait(lock, [&] { return done; });
	}
	s_unpaced.fetch_sub(1, std::memory_order_acq_rel);
}

// Controllers, read in retro_run and applied by the CPU thread at its vsync.
static constexpr unsigned PORTS = 2;
static float s_pad_state[PORTS][PadDualshock2::Inputs::LENGTH];
static std::mutex s_pad_mutex;

void lr_cpu_thread_pump()
{
	{
		std::lock_guard<std::mutex> lock(s_pad_mutex);
		for (unsigned port = 0; port < PORTS; port++)
			for (u32 i = 0; i < PadDualshock2::Inputs::LENGTH; i++)
				Pad::SetControllerState(port, i, s_pad_state[port][i]);
	}
	run_cpu_tasks();
}

static void cpu_thread_main()
{
	Error err;
	g_orbis_code_direct = 1;
	if (!PageFaultHandler::Install(&err))
		lr_logf(LR_LOG_WARN, "page fault handler: %s", err.GetDescription().c_str());

	Boot result = Boot::Failed;
	if (!VMManager::Internal::CPUThreadInitialize())
		lr_logf(LR_LOG_ERROR, "the CPU thread did not initialise");
	else
	{
		SysMemory::Reset(); // the PS5 port: the memory map before the recompilers' first reset
		VMBootParameters params;
		params.filename = s_game_path;
		params.source_type = CDVD_SourceType::Iso;
		if (s_game_path.size() > 4 && !strcasecmp(s_game_path.c_str() + s_game_path.size() - 4, ".elf"))
		{
			params.elf_override = s_game_path;
			params.filename.clear();
			params.source_type = CDVD_SourceType::NoDisc;
		}
		const VMBootResult res = VMManager::Initialize(params, &err);
		if (res == VMBootResult::StartupSuccess)
			result = Boot::Running;
		else
		{
			lr_logf(LR_LOG_ERROR, "the game did not start: %s", err.GetDescription().c_str());
			lr_osd("PS5SX2: " + err.GetDescription(), 8.0f);
		}
	}

	{
		std::unique_lock<std::mutex> lock(s_cpu_mutex);
		s_boot = result;
		s_cpu_cv.notify_all();
		if (result == Boot::Running)
			s_cpu_cv.wait(lock, [] { return s_go || s_stop; });
	}

	if (result == Boot::Running)
	{
		if (!s_stop)
			VMManager::SetState(VMState::Running);
		for (;;)
		{
			run_cpu_tasks();
			const VMState state = VMManager::GetState();
			if (s_stop || state == VMState::Stopping || state == VMState::Shutdown)
				break;
			if (state == VMState::Running)
				VMManager::Execute();
			else if (state == VMState::Resetting)
				VMManager::Reset();
			else
			{
				std::unique_lock<std::mutex> lock(s_cpu_mutex);
				s_cpu_cv.wait(lock, [] { return s_stop || !s_cpu_tasks.empty() || VMManager::GetState() != VMState::Paused; });
			}
		}
		run_cpu_tasks();
		VMManager::Shutdown(false);
	}
	VMManager::Internal::CPUThreadShutdown();
}

// RetroArch's create_device: boots the emulator, whose GS creates the device; or, when it
// runs already and RetroArch made a new context, opens the GS again on it.
static bool open_gs()
{
	if (s_cpu_thread.Joinable())
	{
		bool opened = false;
		run_on_cpu_and_wait([&opened]() { opened = MTGS::WaitForOpen(); });
		s_gs_closed = !opened;
		return opened;
	}
	{
		std::lock_guard<std::mutex> lock(s_cpu_mutex);
		s_boot = Boot::None;
		s_go = false;
		s_stop = false;
	}
	s_cpu_thread.SetStackSize(8 * 1024 * 1024);
	if (!s_cpu_thread.Start(&cpu_thread_main))
	{
		lr_logf(LR_LOG_ERROR, "the CPU thread did not start");
		return false;
	}
	std::unique_lock<std::mutex> lock(s_cpu_mutex);
	s_cpu_cv.wait(lock, [] { return s_boot != Boot::None; });
	return s_boot == Boot::Running;
}

static void stop_emulation()
{
	if (!s_cpu_thread.Joinable())
		return;
	lr_vk_stop_handover(true);
	{
		std::lock_guard<std::mutex> lock(s_cpu_mutex);
		s_stop = true;
		s_cpu_cv.notify_all();
	}
	if (VMManager::GetState() == VMState::Running || VMManager::GetState() == VMState::Paused)
		VMManager::SetState(VMState::Stopping);
	s_cpu_thread.Join();
}

// The hardware context.
static retro_hw_render_callback s_hw_render;

static void context_reset()
{
	if (!lr_vk_context_reset(environ_cb))
		return;
	lr_vk_stop_handover(false);
	std::lock_guard<std::mutex> lock(s_cpu_mutex);
	s_go = true;
	s_cpu_cv.notify_all();
}

static void context_destroy()
{
	// RetroArch is about to destroy the device: the GS lets go of it first.
	lr_vk_context_destroy();
	if (s_cpu_thread.Joinable() && s_boot == Boot::Running && !s_gs_closed)
	{
		lr_vk_stop_handover(true);
		run_on_cpu_and_wait([]() { MTGS::WaitForClose(); });
		s_gs_closed = true;
	}
	lr_vk_forget_device();
}

// Rumble from the game (InputManager.cpp).
extern "C" void orbis_pad_vibration(unsigned pad_index, float large, float small)
{
	if (!rumble_cb || pad_index >= PORTS)
		return;
	rumble_cb(pad_index, RETRO_RUMBLE_STRONG, static_cast<uint16_t>(std::clamp(large, 0.0f, 1.0f) * 65535.0f));
	rumble_cb(pad_index, RETRO_RUMBLE_WEAK, static_cast<uint16_t>(std::clamp(small, 0.0f, 1.0f) * 65535.0f));
}

// The libretro API.
LR_EXPORT unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

LR_EXPORT void retro_set_environment(retro_environment_t cb)
{
	environ_cb = cb;
	retro_log_callback logging;
	if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
		log_cb = logging.log;
	unsigned version = 0;
	if (cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2)
		cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &s_options);
	static const retro_controller_description pads[] = {{"DualShock 2", RETRO_DEVICE_ANALOG}};
	static const retro_controller_info ports[] = {{pads, 1}, {pads, 1}, {nullptr, 0}};
	cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, const_cast<retro_controller_info*>(ports));
}

LR_EXPORT void retro_set_video_refresh(retro_video_refresh_t cb)
{
	video_cb = cb;
}

LR_EXPORT void retro_set_audio_sample(retro_audio_sample_t)
{
}

LR_EXPORT void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
	audio_batch_cb = cb;
}

LR_EXPORT void retro_set_input_poll(retro_input_poll_t cb)
{
	input_poll_cb = cb;
}

LR_EXPORT void retro_set_input_state(retro_input_state_t cb)
{
	input_state_cb = cb;
}

LR_EXPORT void retro_init(void)
{
	retro_rumble_interface rumble = {};
	if (environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble))
		rumble_cb = rumble.set_rumble_state;
}

LR_EXPORT void retro_deinit(void)
{
	stop_emulation();
}

LR_EXPORT void retro_get_system_info(retro_system_info* info)
{
	std::memset(info, 0, sizeof(*info));
	info->library_name = "PS5SX2";
	info->library_version = "0.1";
	info->valid_extensions = "iso|chd|cso|zso|bin|elf";
	info->need_fullpath = true;
	info->block_extract = true;
}

LR_EXPORT void retro_get_system_av_info(retro_system_av_info* info)
{
	const int upscale = std::clamp(std::atoi(option("ps5sx2_upscale", "2").c_str()), 1, 4);
	std::memset(info, 0, sizeof(*info));
	info->geometry.base_width = 640 * upscale;
	info->geometry.base_height = 448 * upscale;
	info->geometry.max_width = 640 * 8;
	info->geometry.max_height = 512 * 8;
	info->geometry.aspect_ratio = option("ps5sx2_aspect", "4:3") == "16:9" ? 16.0f / 9.0f : 4.0f / 3.0f;
	info->timing.fps = 59.94;
	info->timing.sample_rate = 48000.0;
}

LR_EXPORT void retro_set_controller_port_device(unsigned, unsigned)
{
}

LR_EXPORT bool retro_load_game(const retro_game_info* game)
{
	if (!game || !game->path)
		return false;
	find_folders();

	retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
	environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);

	const std::string bios = pick_bios();
	if (bios.empty())
	{
		lr_logf(LR_LOG_ERROR, "no PS2 BIOS in %s", s_bios_dir.c_str());
		lr_osd("PS5SX2 needs a PS2 BIOS in system/pcsx2/bios", 8.0f);
		return false;
	}

	EmuFolders::AppRoot = s_core_dir;
	EmuFolders::DataRoot = s_core_dir;
	EmuFolders::Settings = Path::Combine(s_core_dir, "inis");
	EmuFolders::Resources = Path::Combine(s_core_dir, "resources");
	FileSystem::CreateDirectoryPath(s_core_dir.c_str(), false);
	build_settings(bios);
	{
		auto lock = Host::GetSettingsLock();
		Host::Internal::SetBaseSettingsLayer(&s_settings);
	}
	VMManager::Internal::LoadStartupSettings();

	s_game_path = game->path;
	s_gs_closed = false;

	std::memset(&s_hw_render, 0, sizeof(s_hw_render));
	s_hw_render.context_type = RETRO_HW_CONTEXT_VULKAN;
	s_hw_render.version_major = VK_API_VERSION_1_1;
	s_hw_render.context_reset = context_reset;
	s_hw_render.context_destroy = context_destroy;
	s_hw_render.cache_context = false;
	if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &s_hw_render))
	{
		lr_logf(LR_LOG_ERROR, "RetroArch has no Vulkan context for the core");
		return false;
	}
	lr_vk_set_open_gs(open_gs);
	lr_vk_negotiate(environ_cb);
	lr_logf(LR_LOG_INFO, "loading %s with BIOS %s", s_game_path.c_str(), bios.c_str());
	return true;
}

LR_EXPORT bool retro_load_game_special(unsigned, const retro_game_info*, size_t)
{
	return false;
}

LR_EXPORT void retro_unload_game(void)
{
	stop_emulation();
}

LR_EXPORT void retro_reset(void)
{
	if (s_boot == Boot::Running)
		run_on_cpu_and_wait([]() { VMManager::Reset(); });
}

static void poll_input()
{
	input_poll_cb();
	static const struct
	{
		unsigned retro;
		u32 ps2;
	} buttons[] = {
		{RETRO_DEVICE_ID_JOYPAD_UP, PadDualshock2::Inputs::PAD_UP},
		{RETRO_DEVICE_ID_JOYPAD_RIGHT, PadDualshock2::Inputs::PAD_RIGHT},
		{RETRO_DEVICE_ID_JOYPAD_DOWN, PadDualshock2::Inputs::PAD_DOWN},
		{RETRO_DEVICE_ID_JOYPAD_LEFT, PadDualshock2::Inputs::PAD_LEFT},
		{RETRO_DEVICE_ID_JOYPAD_X, PadDualshock2::Inputs::PAD_TRIANGLE},
		{RETRO_DEVICE_ID_JOYPAD_A, PadDualshock2::Inputs::PAD_CIRCLE},
		{RETRO_DEVICE_ID_JOYPAD_B, PadDualshock2::Inputs::PAD_CROSS},
		{RETRO_DEVICE_ID_JOYPAD_Y, PadDualshock2::Inputs::PAD_SQUARE},
		{RETRO_DEVICE_ID_JOYPAD_SELECT, PadDualshock2::Inputs::PAD_SELECT},
		{RETRO_DEVICE_ID_JOYPAD_START, PadDualshock2::Inputs::PAD_START},
		{RETRO_DEVICE_ID_JOYPAD_L, PadDualshock2::Inputs::PAD_L1},
		{RETRO_DEVICE_ID_JOYPAD_L2, PadDualshock2::Inputs::PAD_L2},
		{RETRO_DEVICE_ID_JOYPAD_R, PadDualshock2::Inputs::PAD_R1},
		{RETRO_DEVICE_ID_JOYPAD_R2, PadDualshock2::Inputs::PAD_R2},
		{RETRO_DEVICE_ID_JOYPAD_L3, PadDualshock2::Inputs::PAD_L3},
		{RETRO_DEVICE_ID_JOYPAD_R3, PadDualshock2::Inputs::PAD_R3},
	};
	float state[PORTS][PadDualshock2::Inputs::LENGTH] = {};
	for (unsigned port = 0; port < PORTS; port++)
	{
		const int16_t mask = input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
		for (const auto& b : buttons)
			state[port][b.ps2] = (mask & (1 << b.retro)) ? 1.0f : 0.0f;
		const auto axis = [&](unsigned index, unsigned id, u32 positive, u32 negative) {
			const float v = input_state_cb(port, RETRO_DEVICE_ANALOG, index, id) / 32767.0f;
			state[port][positive] = std::clamp(v, 0.0f, 1.0f);
			state[port][negative] = std::clamp(-v, 0.0f, 1.0f);
		};
		axis(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, PadDualshock2::Inputs::PAD_L_RIGHT, PadDualshock2::Inputs::PAD_L_LEFT);
		axis(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, PadDualshock2::Inputs::PAD_L_DOWN, PadDualshock2::Inputs::PAD_L_UP);
		axis(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, PadDualshock2::Inputs::PAD_R_RIGHT, PadDualshock2::Inputs::PAD_R_LEFT);
		axis(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, PadDualshock2::Inputs::PAD_R_DOWN, PadDualshock2::Inputs::PAD_R_UP);
	}
	std::lock_guard<std::mutex> lock(s_pad_mutex);
	std::memcpy(s_pad_state, state, sizeof(state));
}

LR_EXPORT void retro_run(void)
{
	bool updated = false;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
	{
		apply_options(s_settings);
		run_on_cpu_and_wait([]() { VMManager::ApplySettings(); });
	}

	poll_input();

	// The frame the GS finished, or the last one again.
	if (!lr_vk_present(video_cb, 50))
		video_cb(nullptr, 0, 0, 0);

	static int16_t samples[48000 / 30 * 2];
	unsigned frames;
	while ((frames = lr_audio_read(samples, sizeof(samples) / sizeof(samples[0]) / 2)) > 0)
		audio_batch_cb(samples, frames);
}

LR_EXPORT size_t retro_serialize_size(void)
{
	return 0;
}

LR_EXPORT bool retro_serialize(void*, size_t)
{
	return false;
}

LR_EXPORT bool retro_unserialize(const void*, size_t)
{
	return false;
}

LR_EXPORT void retro_cheat_reset(void)
{
}

LR_EXPORT void retro_cheat_set(unsigned, bool, const char*)
{
}

LR_EXPORT unsigned retro_get_region(void)
{
	return RETRO_REGION_NTSC;
}

LR_EXPORT void* retro_get_memory_data(unsigned id)
{
	return id == RETRO_MEMORY_SYSTEM_RAM && eeMem ? eeMem->Main : nullptr;
}

LR_EXPORT size_t retro_get_memory_size(unsigned id)
{
	return id == RETRO_MEMORY_SYSTEM_RAM && eeMem ? Ps2MemSize::MainRam : 0;
}
