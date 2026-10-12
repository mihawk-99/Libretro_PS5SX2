// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Libretro_PS5SX2: PCSX2's host interface inside a libretro core. The app's host
// (tests/ctest/core/StubHost.cpp) drives the app's shelf, notifications, pad thread and
// hotkeys; here RetroArch is the host, so messages go to its log and on-screen display,
// work for the CPU thread is queued and run at its next message pump, and the settings
// come from the core options (libretro.cpp).

#include "pcsx2/Achievements.h"
#include "pcsx2/GS.h"
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/MTGS.h"
#include "common/Error.h"
#include "common/ProgressCallback.h"

#include "libretro_host.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

void Host::CommitBaseSettingChanges()
{
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
	// RetroArch does achievements (rcheevos against the core's memory), so PCSX2's own stay off.
	EmuConfig.Achievements.Enabled = false;
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	return false;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	lr_logf(LR_LOG_INFO, "%.*s: %.*s", (int)title.size(), title.data(), (int)message.size(), message.data());
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	lr_logf(LR_LOG_ERROR, "%.*s: %.*s", (int)title.size(), title.data(), (int)message.size(), message.data());
	lr_osd(std::string(message), 5.0f);
}

void Host::OpenURL(const std::string_view url)
{
}

bool Host::InBatchMode()
{
	return false;
}

bool Host::InNoGUIMode()
{
	return true;
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	return false;
}

std::string Host::GetTextFromClipboard()
{
	return std::string();
}

void Host::BeginTextInput()
{
}

void Host::EndTextInput()
{
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	return std::nullopt;
}

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
}

void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier)
{
}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor)
{
}

void Host::SetMouseLock(bool state)
{
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	// No window: the GS renders into RetroArch's Vulkan device and hands each frame over
	// (GSDeviceVK's LIBRETRO present), so the device runs surfaceless.
	WindowInfo wi;
	wi.type = WindowInfo::Type::Surfaceless;
	wi.surface_width = 1920;
	wi.surface_height = 1080;
	wi.surface_scale = 1.0f;
	wi.surface_refresh_rate = 60.0f;
	return wi;
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

void Host::OnVMStarting()
{
}

void Host::OnVMStarted()
{
}

void Host::OnVMDestroyed()
{
}

void Host::OnVMPaused()
{
}

void Host::OnVMResumed()
{
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
	const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
	lr_logf(LR_LOG_INFO, "game: %s [%s] crc %08X", title.c_str(), disc_serial.c_str(), current_crc);
}

void Host::OnPerformanceMetricsUpdated()
{
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
}

// Work for the CPU thread, run at its next PumpMessagesOnCPUThread (each vsync).
static std::mutex s_cpu_work_lock;
static std::deque<std::function<void()>> s_cpu_work;

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	std::lock_guard<std::mutex> lock(s_cpu_work_lock);
	s_cpu_work.push_back(std::move(function));
}

void Host::PumpMessagesOnCPUThread()
{
	for (;;)
	{
		std::function<void()> work;
		{
			std::lock_guard<std::mutex> lock(s_cpu_work_lock);
			if (s_cpu_work.empty())
				break;
			work = std::move(s_cpu_work.front());
			s_cpu_work.pop_front();
		}
		work();
	}
	lr_cpu_thread_pump();
}

void Host::RunOnGSThread(std::function<void()> function)
{
	MTGS::RunOnGSThread(std::move(function));
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
}

void Host::CancelGameListRefresh()
{
}

bool Host::IsFullscreen()
{
	return true;
}

void Host::SetFullscreen(bool enabled)
{
}

void Host::OnCaptureStarted(const std::string& filename)
{
}

void Host::OnCaptureStopped()
{
}

void Host::RequestExitApplication(bool allow_confirm)
{
}

void Host::RequestExitBigPicture()
{
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
}

s32 Host::Internal::GetTranslatedStringImpl(
	const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	else if (msg.empty())
		return 0;

	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	TinyString count_str = TinyString::from_format("{}", count);

	std::string ret(msg);
	for (;;)
	{
		std::string::size_type pos = ret.find("%n");
		if (pos == std::string::npos)
			break;

		ret.replace(pos, pos + 2, count_str.view());
	}

	return ret;
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
}

void Host::OnAchievementsRefreshed()
{
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
}

bool Host::LocaleCircleConfirm()
{
	return false;
}

bool Host::ShouldPreferHostFileSelector()
{
	return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
	FileSelectorFilters filters, std::string_view initial_directory)
{
	callback(std::string());
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	int res = std::strncmp(lhs.data(), rhs.data(), std::min(lhs.size(), rhs.size()));
	if (res != 0)
		return res;
	return lhs.size() > rhs.size() ? 1 : lhs.size() < rhs.size() ? -1 : 0;
}

// PS5 port (vk-285-72): the host's key codes are USB HID usages (page 7), which is what the console's
// keyboard library reports (the port's ProsperoKbdMouse.cpp). The names are the ones the USB HID keyboard
// device asks for (USB/usb-hid/usb-hid.cpp, s_qkeycode_names), so a key the console reports reaches the
// PS2's keyboard as the same key. Consumer-page keys (media, browser) have no usage here.
namespace
{
	struct OrbisHostKey
	{
		const char* name;
		u32 usage;
	};
	constexpr OrbisHostKey s_orbis_host_keys[] = {
		{"A", 0x04}, {"B", 0x05}, {"C", 0x06}, {"D", 0x07}, {"E", 0x08}, {"F", 0x09}, {"G", 0x0a}, {"H", 0x0b},
		{"I", 0x0c}, {"J", 0x0d}, {"K", 0x0e}, {"L", 0x0f}, {"M", 0x10}, {"N", 0x11}, {"O", 0x12}, {"P", 0x13},
		{"Q", 0x14}, {"R", 0x15}, {"S", 0x16}, {"T", 0x17}, {"U", 0x18}, {"V", 0x19}, {"W", 0x1a}, {"X", 0x1b},
		{"Y", 0x1c}, {"Z", 0x1d}, {"1", 0x1e}, {"2", 0x1f}, {"3", 0x20}, {"4", 0x21}, {"5", 0x22}, {"6", 0x23},
		{"7", 0x24}, {"8", 0x25}, {"9", 0x26}, {"0", 0x27}, {"Return", 0x28}, {"Escape", 0x29},
		{"Backspace", 0x2a}, {"Tab", 0x2b}, {"Space", 0x2c}, {"Minus", 0x2d}, {"Equal", 0x2e},
		{"BracketLeft", 0x2f}, {"BracketRight", 0x30}, {"Backslash", 0x31}, {"Semicolon", 0x33},
		{"Apostrophe", 0x34}, {"Agrave", 0x35}, {"Comma", 0x36}, {"Period", 0x37}, {"Slash", 0x38},
		{"Caps_lock", 0x39}, {"F1", 0x3a}, {"F2", 0x3b}, {"F3", 0x3c}, {"F4", 0x3d}, {"F5", 0x3e}, {"F6", 0x3f},
		{"F7", 0x40}, {"F8", 0x41}, {"F9", 0x42}, {"F10", 0x43}, {"F11", 0x44}, {"F12", 0x45}, {"Print", 0x46},
		{"Scroll_lock", 0x47}, {"Pause", 0x48}, {"Insert", 0x49}, {"Home", 0x4a}, {"PageUp", 0x4b},
		{"Delete", 0x4c}, {"End", 0x4d}, {"PageDown", 0x4e}, {"Right", 0x4f}, {"Left", 0x50}, {"Down", 0x51},
		{"Up", 0x52}, {"Num_lock", 0x53}, {"NumpadSlash", 0x54}, {"NumpadAsterisk", 0x55},
		{"NumpadMinus", 0x56}, {"NumpadPlus", 0x57}, {"NumpadReturn", 0x58}, {"Numpad1", 0x59},
		{"Numpad2", 0x5a}, {"Numpad3", 0x5b}, {"Numpad4", 0x5c}, {"Numpad5", 0x5d}, {"Numpad6", 0x5e},
		{"Numpad7", 0x5f}, {"Numpad8", 0x60}, {"Numpad9", 0x61}, {"Numpad0", 0x62}, {"NumpadPeriod", 0x63},
		{"Less", 0x64}, {"Compose", 0x65}, {"Power", 0x66}, {"NumpadEqual", 0x67}, {"Help", 0x75},
		{"Menu", 0x76}, {"Front", 0x77}, {"Stop", 0x78}, {"Again", 0x79}, {"Undo", 0x7a}, {"Cut", 0x7b},
		{"Copy", 0x7c}, {"Paste", 0x7d}, {"Find", 0x7e}, {"AudioMute", 0x7f}, {"VolumeUp", 0x80},
		{"VolumeDown", 0x81}, {"NumpadComma", 0x85}, {"Ro", 0x87}, {"KatakanaHiragana", 0x88}, {"Yen", 0x89},
		{"Henkan", 0x8a}, {"Muhenkan", 0x8b}, {"Hiragana", 0x93}, {"Sysrq", 0x9a}, {"Control", 0xe0},
		{"Shift", 0xe1}, {"Alt", 0xe2}, {"Meta", 0xe3}, {"Control_r", 0xe4}, {"Shift_r", 0xe5}, {"Alt_r", 0xe6},
	};
} // namespace

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str)
{
	for (const OrbisHostKey& key : s_orbis_host_keys)
	{
		if (str == key.name)
			return key.usage;
	}
	return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	for (const OrbisHostKey& key : s_orbis_host_keys)
	{
		if (code == key.usage)
			return std::string(key.name);
	}
	return std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
	return nullptr;
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()
