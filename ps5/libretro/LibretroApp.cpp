// SPDX-License-Identifier: GPL-3.0-or-later
// Libretro_PS5SX2: what the emulator asks of the PS5SX2 app, answered inside a core.
//
// PCSX2's PS5 sources call into the app around them: its boot code (main-boot.cpp), its
// notifications, profiler, debug overlay, disc drive, NFS shares and frontend. The core
// leaves those files out (ps5/libretro/Makefile.libretro), so each call lands here:
// notifications become RetroArch messages, the app's settings and folders get the core's
// answer, and what a core has no use for (the profiler, the overlay, the drive) does
// nothing. Executable memory comes from PS5 RetroArch's platform layer, which the title
// binds for every core, instead of the JIT shared memory the elevated app can create.

#include "common/Pcsx2Defs.h"
#include "common/Error.h"
#include "common/HTTPDownloader.h"
#include "common/CrashHandler.h"
#include "pcsx2/CDVD/CDVDdiscReader.h"
#include "ps5/frontend/fe_i18n.h"

#include "libretro_host.h"

#include <atomic>
#include <ctime>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include <ps5platform/exec.h>

// main-boot.cpp's globals the emulator reads.
std::atomic<int> g_orbis_filter_cycle{0};
// The software renderer draws on the Vulkan device too (GS.cpp), so its frames reach
// RetroArch the way the hardware renderer's do, never through the app's CPU present.
bool g_orbis_sw_on_gl = true;
extern "C" int g_orbis_test_build = 0;
std::vector<u32> g_orbis_watermark;
int g_orbis_watermark_w = 0, g_orbis_watermark_h = 0;
extern "C" volatile unsigned long long orbis_fault_count = 0;

// The recompilers' code area (Memory.cpp tries direct memory made executable first; this is
// its second choice): read, write and execute memory from the platform layer.
extern "C" void* orbis_alloc_jit(unsigned long long len)
{
	return ps5_exec_allocate(static_cast<size_t>(len), 0);
}

// LnxHostSys.cpp's address-space reservations (the app's mmapshim.cpp).
extern "C" int sceKernelReserveVirtualRange(void** addr, unsigned long long len, int flags, unsigned long long align);
extern "C" int orbis_reserve_range(void** addr, unsigned long long len)
{
	return sceKernelReserveVirtualRange(addr, len, 0, 0);
}

// The app's own text drawing and CPU framebuffer present (demo_renderer.cpp), for a
// software renderer without a GPU device, which the core never runs (g_orbis_sw_on_gl).
extern "C" void orbis_text_rgba(u32* buf, unsigned w, unsigned h, unsigned x, unsigned y, const char* s, unsigned scale,
	u32 color)
{
}

extern "C" void orbis_present_frame(const unsigned char* pixels, int width, int height, int pitch)
{
}

// Notifications (ProsperoNotify.cpp): RetroArch's messages.
void OrbisNotifyPlain(const char* text)
{
	if (text && *text)
		lr_osd(text, 4.0f);
}

void OrbisNotifyRich(const char* message, const char* sub_message, const char* icon, const char* sound, const char* channel)
{
	std::string text = message ? message : "";
	if (sub_message && *sub_message)
		text += text.empty() ? sub_message : std::string(": ") + sub_message;
	if (!text.empty())
		lr_osd(text, 4.0f);
}

void OrbisNotifyRichNow(const char* message, const char* sub_message, const char* icon, const char* sound, const char* channel)
{
	OrbisNotifyRich(message, sub_message, icon, sound, channel);
}

void OrbisExitApp(int code)
{
	lr_logf(LR_LOG_WARN, "the emulator asked the app to exit (%d); a core stays loaded", code);
}

// The frontend's strings (fe_i18n.cpp), for PCSX2's achievements code, which a core never
// runs (RetroArch does achievements): no text.
namespace fe
{
const char* Tr(Str id)
{
	return "";
}
} // namespace fe

bool orbis_web_qr(std::vector<unsigned char>& modules, int& size, std::string& shown)
{
	return false;
}

void OrbisAchievementsLoginUnreachable()
{
}

// The app's per-game settings file, bezel and texture folders (main-boot.cpp): the core's
// settings are its options, so none of these exist.
const char* OrbisGameIniPath()
{
	return "";
}

std::string OrbisBezelDirectory()
{
	return std::string();
}

std::string OrbisTexturesGameDir(const std::string& serial, std::string& how)
{
	how = "none";
	return std::string();
}

std::string OrbisUserAgentProduct()
{
	return "PS5SX2-libretro";
}

// The profiler (orbis_eeprof.cpp) and the debug overlay (debug_overlay.cpp).
void OrbisEEProfMark()
{
}

void OrbisGSProfStart()
{
}

void OrbisVUProfStart()
{
}

void OrbisJitRegister(const char* group, const void* ptr, size_t size, u64 key, const char* name)
{
}

void OrbisAutoProfSecond(float speed, float ee, float gs, float vu)
{
}

namespace ps5::debug
{
void set_line(int line, const char* fmt, ...)
{
}
} // namespace ps5::debug

void CrashHandler::CrashSignalHandler(int signal, siginfo_t* siginfo, void* ctx)
{
}

std::unique_ptr<HTTPDownloader> HTTPDownloader::Create(std::string user_agent)
{
	return nullptr;
}

// The disc drive (OrbisIOCtlSrc.cpp): a core plays images, so there is no drive to open.
void GetValidDrive(std::string& drive)
{
	drive.clear();
}

IOCtlSrc::IOCtlSrc(std::string filename)
	: m_filename(std::move(filename))
{
}

IOCtlSrc::~IOCtlSrc() = default;

bool IOCtlSrc::Reopen(Error* error)
{
	Error::SetString(error, "PS5SX2's core reads disc images, not the drive");
	return false;
}

u32 IOCtlSrc::GetSectorCount() const
{
	return 0;
}

const std::vector<toc_entry>& IOCtlSrc::ReadTOC() const
{
	return m_toc;
}

bool IOCtlSrc::ReadSectors2048(u32 sector, u32 count, u8* buffer) const
{
	return false;
}

bool IOCtlSrc::ReadSectors2352(u32 sector, u32 count, u8* buffer) const
{
	return false;
}

bool IOCtlSrc::ReadTrackSubQ(cdvdSubQ* subq) const
{
	return false;
}

u32 IOCtlSrc::GetLayerBreakAddress() const
{
	return 0;
}

s32 IOCtlSrc::GetMediaType() const
{
	return -1;
}

bool IOCtlSrc::DiscReady()
{
	return false;
}

// Network information for DEV9's adapter (AdapterUtils.cpp): the title does not link the
// console's NetCtl library, so the core answers that there is none to read, and the
// adapter falls back to its defaults.
extern "C" int sceNetCtlInit(void)
{
	return -1;
}

extern "C" int sceNetCtlGetInfo(int code, void* info)
{
	return -1;
}

// timegm (CDVD.cpp, MemoryCardFolder.cpp): the console's libc has no export of it.
extern "C" time_t timegm(struct tm* tm)
{
	// Days from 1970-01-01 to the civil date (Howard Hinnant's days_from_civil).
	const long long y = tm->tm_year + 1900LL - (tm->tm_mon + 1 <= 2);
	const long long era = (y >= 0 ? y : y - 399) / 400;
	const long long yoe = y - era * 400;
	const long long m = tm->tm_mon + 1;
	const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + tm->tm_mday - 1;
	const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	const long long days = era * 146097 + doe - 719468;
	return static_cast<time_t>(days * 86400LL + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec);
}
