// SPDX-License-Identifier: GPL-3.0-or-later
// Libretro_PS5SX2: SPU2's output goes to RetroArch. The app opens libSceAudioOut on its
// own thread (orbis-shims/ProsperoAudio.cpp); a core has no audio device of its own, so
// the stream here only buffers, and retro_run takes what the emulation produced since the
// previous frame (lr_audio_read) and hands it to the audio batch callback. No time
// stretching: RetroArch's dynamic rate control absorbs the small drift between the two
// clocks.

#include "Host/AudioStream.h"
#include "common/Error.h"
#include "USB/usb-mic/audiodev.h"
#include "USB/usb-mic/audiodev-cubeb.h"

#include "libretro_host.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

std::vector<std::pair<std::string, std::string>> AudioStream::GetCubebDriverNames()
{
	return {};
}

std::vector<AudioStream::DeviceInfo> AudioStream::GetCubebOutputDevices(const char* driver)
{
	(void)driver;
	return {};
}

namespace
{
class LibretroAudioStream;
std::atomic<LibretroAudioStream*> s_stream{nullptr};

class LibretroAudioStream final : public AudioStream
{
public:
	LibretroAudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
		: AudioStream(sample_rate, parameters)
	{
	}
	~LibretroAudioStream() override
	{
		LibretroAudioStream* self = this;
		s_stream.compare_exchange_strong(self, nullptr);
	}

	void Open()
	{
		BaseInitialize(&StereoSampleReaderImpl, false);
		s_stream.store(this);
	}

	u32 Read(int16_t* out, u32 max_frames)
	{
		alignas(64) float buf[1024 * 2];
		u32 total = 0;
		u32 frames = std::min(GetBufferedFramesRelaxed(), max_frames);
		while (frames > 0)
		{
			const u32 n = std::min<u32>(frames, 1024);
			ReadFrames(buf, n);
			for (u32 i = 0; i < n * 2; i++)
			{
				const float s = std::clamp(buf[i], -1.0f, 1.0f);
				out[total * 2 + i] = static_cast<int16_t>(std::lrintf(s * 32767.0f));
			}
			total += n;
			frames -= n;
		}
		return total;
	}
};
} // namespace

unsigned lr_audio_read(short* out, unsigned max_frames)
{
	LibretroAudioStream* stream = s_stream.load();
	return stream ? stream->Read(out, max_frames) : 0;
}

std::unique_ptr<AudioStream> AudioStream::CreateCubebAudioStream(u32 sample_rate, const AudioStreamParameters& parameters,
	const char* driver_name, const char* device_name, bool stretch_enabled, Error* error)
{
	(void)driver_name;
	(void)device_name;
	(void)stretch_enabled;
	(void)error;
	AudioStreamParameters p = parameters;
	p.expansion_mode = AudioExpansionMode::Disabled;
	auto stream = std::make_unique<LibretroAudioStream>(sample_rate, p);
	stream->Open();
	lr_logf(LR_LOG_INFO, "audio: %u Hz stereo to RetroArch", sample_rate);
	return stream;
}

std::unique_ptr<AudioStream> AudioStream::CreateSDLAudioStream(u32 sample_rate, const AudioStreamParameters& parameters,
	bool stretch_enabled, Error* error)
{
	return CreateCubebAudioStream(sample_rate, parameters, nullptr, nullptr, stretch_enabled, error);
}

// The USB microphone and headset have no device in a core: a source that stays silent, a
// sink that drops what it is given.
namespace usb_mic
{
namespace audiodev_cubeb
{
CubebAudioDevice::CubebAudioDevice(AudioDir dir, u32 channels, std::string devname, s32 latency)
	: AudioDevice(dir, channels)
	, mDeviceName(std::move(devname))
{
	(void)latency;
	mContext = nullptr;
	mDeviceId = nullptr;
}
CubebAudioDevice::~CubebAudioDevice() = default;
std::vector<std::pair<std::string, std::string>> CubebAudioDevice::GetDeviceList(bool input)
{
	(void)input;
	return {};
}
uint32_t CubebAudioDevice::GetBuffer(int16_t* buff, uint32_t frames)
{
	(void)buff;
	(void)frames;
	return 0;
}
uint32_t CubebAudioDevice::SetBuffer(int16_t* buff, uint32_t frames)
{
	(void)buff;
	return frames;
}
bool CubebAudioDevice::GetFrames(uint32_t* size)
{
	if (size)
		*size = 0;
	return true;
}
void CubebAudioDevice::SetResampling(int samplerate)
{
	(void)samplerate;
}
bool CubebAudioDevice::Start()
{
	return true;
}
void CubebAudioDevice::Stop()
{
}
void CubebAudioDevice::ResetBuffers()
{
}
long CubebAudioDevice::DataCallback(struct cubeb_stream* stream, void* user_ptr, void const* input_buffer,
	void* output_buffer, long nframes)
{
	(void)stream;
	(void)user_ptr;
	(void)input_buffer;
	(void)output_buffer;
	return nframes;
}
} // namespace audiodev_cubeb
} // namespace usb_mic
