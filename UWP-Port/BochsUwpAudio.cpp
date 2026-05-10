#include "pch.h"
#include "BochsUwpAudio.h"

#include <algorithm>
#include <deque>
#include <robuffer.h>
#include <xaudio2.h>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace concurrency;
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Devices::Midi;
using namespace Windows::Media;
using namespace Windows::Media::Audio;
using namespace Windows::Media::Capture;
using namespace Windows::Media::MediaProperties;
using namespace Windows::Media::Render;
using namespace Windows::Storage::Streams;

struct __declspec(uuid("5B0D3235-4DBA-4D44-865E-8F1D0E4FD04D")) IMemoryBufferByteAccess : IUnknown
{
	virtual HRESULT STDMETHODCALLTYPE GetBuffer(byte **value, UINT32 *capacity) = 0;
};

namespace
{
	struct AudioBufferContext
	{
		std::vector<unsigned char> data;
	};

	class AudioVoiceCallback final : public IXAudio2VoiceCallback
	{
	public:
		void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
		void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
		void STDMETHODCALLTYPE OnStreamEnd() override {}
		void STDMETHODCALLTYPE OnBufferStart(void *) override {}
		void STDMETHODCALLTYPE OnBufferEnd(void *context) override
		{
			delete static_cast<AudioBufferContext *>(context);
		}
		void STDMETHODCALLTYPE OnLoopEnd(void *) override {}
		void STDMETHODCALLTYPE OnVoiceError(void *context, HRESULT) override
		{
			delete static_cast<AudioBufferContext *>(context);
		}
	};

	std::mutex g_audioMutex;
	ComPtr<IXAudio2> g_engine;
	IXAudio2MasteringVoice *g_masterVoice = nullptr;
	IXAudio2SourceVoice *g_sourceVoice = nullptr;
	AudioVoiceCallback g_voiceCallback;
	WAVEFORMATEX g_waveFormat = {};
	bool g_audioReady = false;

	std::mutex g_captureMutex;
	AudioGraph^ g_captureGraph = nullptr;
	AudioDeviceInputNode^ g_captureNode = nullptr;
	AudioFrameOutputNode^ g_captureOutputNode = nullptr;
	Windows::Foundation::EventRegistrationToken g_captureQuantumToken = {};
	std::deque<unsigned char> g_captureQueue;
	unsigned g_captureSampleRate = 0;
	unsigned g_captureBits = 0;
	unsigned g_captureChannels = 0;
	bool g_captureReady = false;

	std::mutex g_midiMutex;
	MidiSynthesizer^ g_midiSynthesizer = nullptr;
	bool g_midiReady = false;

	bool SameFormat(unsigned sampleRate, unsigned bits, unsigned channels)
	{
		return g_audioReady &&
			g_waveFormat.nSamplesPerSec == sampleRate &&
			g_waveFormat.wBitsPerSample == bits &&
			g_waveFormat.nChannels == channels;
	}

	void CloseAudioLocked()
	{
		if (g_sourceVoice != nullptr)
		{
			g_sourceVoice->Stop(0);
			g_sourceVoice->FlushSourceBuffers();
			g_sourceVoice->DestroyVoice();
			g_sourceVoice = nullptr;
		}
		if (g_masterVoice != nullptr)
		{
			g_masterVoice->DestroyVoice();
			g_masterVoice = nullptr;
		}
		g_engine.Reset();
		g_waveFormat = {};
		g_audioReady = false;
	}

	bool OpenAudioLocked(unsigned sampleRate, unsigned bits, unsigned channels)
	{
		if (SameFormat(sampleRate, bits, channels))
		{
			return true;
		}

		CloseAudioLocked();
		if (sampleRate == 0 || bits == 0 || channels == 0)
		{
			return false;
		}

		if (FAILED(XAudio2Create(&g_engine, 0, XAUDIO2_DEFAULT_PROCESSOR)))
		{
			return false;
		}
		if (FAILED(g_engine->CreateMasteringVoice(&g_masterVoice)))
		{
			CloseAudioLocked();
			return false;
		}

		g_waveFormat.wFormatTag = WAVE_FORMAT_PCM;
		g_waveFormat.nChannels = static_cast<WORD>(channels);
		g_waveFormat.nSamplesPerSec = sampleRate;
		g_waveFormat.wBitsPerSample = static_cast<WORD>(bits);
		g_waveFormat.nBlockAlign = static_cast<WORD>((channels * bits) / 8);
		g_waveFormat.nAvgBytesPerSec = sampleRate * g_waveFormat.nBlockAlign;
		g_waveFormat.cbSize = 0;

		if (FAILED(g_engine->CreateSourceVoice(
			&g_sourceVoice,
			&g_waveFormat,
			0,
			XAUDIO2_DEFAULT_FREQ_RATIO,
			&g_voiceCallback)))
		{
			CloseAudioLocked();
			return false;
		}
		if (FAILED(g_sourceVoice->Start(0)))
		{
			CloseAudioLocked();
			return false;
		}

		g_audioReady = true;
		return true;
	}

	bool SameCaptureFormat(unsigned sampleRate, unsigned bits, unsigned channels)
	{
		return g_captureReady &&
			g_captureSampleRate == sampleRate &&
			g_captureBits == bits &&
			g_captureChannels == channels;
	}

	void CopyCaptureFrame(AudioFrame^ frame)
	{
		if (frame == nullptr)
		{
			return;
		}

		AudioBuffer^ buffer = nullptr;
		Windows::Foundation::IMemoryBufferReference^ reference = nullptr;
		try
		{
			buffer = frame->LockBuffer(AudioBufferAccessMode::Read);
			reference = buffer->CreateReference();
			ComPtr<IMemoryBufferByteAccess> byteAccess;
			if (FAILED(reinterpret_cast<IInspectable *>(reference)->QueryInterface(IID_PPV_ARGS(&byteAccess))))
			{
				return;
			}

			byte *bytes = nullptr;
			UINT32 capacity = 0;
			if (FAILED(byteAccess->GetBuffer(&bytes, &capacity)) || bytes == nullptr || capacity == 0)
			{
				return;
			}

			std::lock_guard<std::mutex> lock(g_captureMutex);
			size_t maxBuffered = (std::max)(static_cast<size_t>(19200 * 8),
				static_cast<size_t>(g_captureSampleRate) * g_captureChannels * ((std::max)(g_captureBits, 8u) / 8u));
			while (g_captureQueue.size() + capacity > maxBuffered && !g_captureQueue.empty())
			{
				g_captureQueue.pop_front();
			}
			g_captureQueue.insert(g_captureQueue.end(), bytes, bytes + capacity);
		}
		catch (...)
		{
		}
	}

	void CaptureQuantumProcessed(AudioGraph^ sender, Object^ args)
	{
		UNREFERENCED_PARAMETER(sender);
		UNREFERENCED_PARAMETER(args);
		AudioFrameOutputNode^ node = g_captureOutputNode;
		if (node == nullptr)
		{
			return;
		}

		try
		{
			CopyCaptureFrame(node->GetFrame());
		}
		catch (...)
		{
		}
	}

	void CloseCaptureLocked()
	{
		if (g_captureGraph != nullptr)
		{
			try
			{
				g_captureGraph->Stop();
				g_captureGraph->QuantumProcessed -= g_captureQuantumToken;
			}
			catch (...)
			{
			}
		}
		if (g_captureNode != nullptr)
		{
			try
			{
				g_captureNode->Stop();
			}
			catch (...)
			{
			}
		}
		if (g_captureOutputNode != nullptr)
		{
			try
			{
				g_captureOutputNode->Stop();
			}
			catch (...)
			{
			}
		}
		g_captureNode = nullptr;
		g_captureOutputNode = nullptr;
		g_captureGraph = nullptr;
		g_captureQueue.clear();
		g_captureSampleRate = 0;
		g_captureBits = 0;
		g_captureChannels = 0;
		g_captureReady = false;
	}

	bool OpenCaptureLocked(unsigned sampleRate, unsigned bits, unsigned channels)
	{
		if (SameCaptureFormat(sampleRate, bits, channels))
		{
			return true;
		}

		CloseCaptureLocked();
		if (sampleRate == 0 || bits == 0 || channels == 0)
		{
			return false;
		}

		try
		{
			AudioGraphSettings^ settings = ref new AudioGraphSettings(AudioRenderCategory::Media);
			settings->EncodingProperties = AudioEncodingProperties::CreatePcm(sampleRate, channels, bits);
			CreateAudioGraphResult^ graphResult = create_task(AudioGraph::CreateAsync(settings)).get();
			if (graphResult == nullptr || graphResult->Status != AudioGraphCreationStatus::Success)
			{
				return false;
			}

			g_captureGraph = graphResult->Graph;
			AudioEncodingProperties^ captureFormat = AudioEncodingProperties::CreatePcm(sampleRate, channels, bits);
			CreateAudioDeviceInputNodeResult^ nodeResult =
				create_task(g_captureGraph->CreateDeviceInputNodeAsync(MediaCategory::Other, captureFormat)).get();
			if (nodeResult == nullptr || nodeResult->Status != AudioDeviceNodeCreationStatus::Success)
			{
				CloseCaptureLocked();
				return false;
			}

			g_captureNode = nodeResult->DeviceInputNode;
			g_captureOutputNode = g_captureGraph->CreateFrameOutputNode(captureFormat);
			if (g_captureOutputNode == nullptr)
			{
				CloseCaptureLocked();
				return false;
			}
			g_captureNode->AddOutgoingConnection(g_captureOutputNode);
			g_captureSampleRate = sampleRate;
			g_captureBits = bits;
			g_captureChannels = channels;
			g_captureQuantumToken = g_captureGraph->QuantumProcessed +=
				ref new TypedEventHandler<AudioGraph^, Object^>(&CaptureQuantumProcessed);
			g_captureOutputNode->Start();
			g_captureNode->Start();
			g_captureGraph->Start();
			g_captureReady = true;
			return true;
		}
		catch (...)
		{
			CloseCaptureLocked();
			return false;
		}
	}

	IBuffer^ BufferFromBytes(const unsigned char *data, unsigned length)
	{
		Buffer^ buffer = ref new Buffer(length);
		buffer->Length = length;
		ComPtr<IBufferByteAccess> byteAccess;
		reinterpret_cast<IInspectable *>(buffer)->QueryInterface(IID_PPV_ARGS(&byteAccess));
		unsigned char *bytes = nullptr;
		byteAccess->Buffer(&bytes);
		if (bytes != nullptr && data != nullptr && length > 0)
		{
			std::memcpy(bytes, data, length);
		}
		return buffer;
	}

	IMidiMessage^ CreateMidiMessage(unsigned command, unsigned length, const unsigned char *data)
	{
		unsigned status = command & 0xff;
		unsigned channel = status & 0x0f;
		unsigned type = status & 0xf0;
		unsigned d0 = (length > 0 && data != nullptr) ? data[0] : 0;
		unsigned d1 = (length > 1 && data != nullptr) ? data[1] : 0;

		switch (type)
		{
		case 0x80:
			return ref new MidiNoteOffMessage(static_cast<byte>(channel), static_cast<byte>(d0), static_cast<byte>(d1));
		case 0x90:
			return ref new MidiNoteOnMessage(static_cast<byte>(channel), static_cast<byte>(d0), static_cast<byte>(d1));
		case 0xa0:
			return ref new MidiPolyphonicKeyPressureMessage(static_cast<byte>(channel), static_cast<byte>(d0), static_cast<byte>(d1));
		case 0xb0:
			return ref new MidiControlChangeMessage(static_cast<byte>(channel), static_cast<byte>(d0), static_cast<byte>(d1));
		case 0xc0:
			return ref new MidiProgramChangeMessage(static_cast<byte>(channel), static_cast<byte>(d0));
		case 0xd0:
			return ref new MidiChannelPressureMessage(static_cast<byte>(channel), static_cast<byte>(d0));
		case 0xe0:
			return ref new MidiPitchBendChangeMessage(static_cast<byte>(channel), static_cast<unsigned short>((d0 & 0x7f) | ((d1 & 0x7f) << 7)));
		default:
			break;
		}

		std::vector<unsigned char> raw;
		raw.push_back(static_cast<unsigned char>(status));
		for (unsigned i = 0; i < length && data != nullptr; ++i)
		{
			raw.push_back(data[i]);
		}

		if (status == 0xf0 || status == 0xf7)
		{
			return ref new MidiSystemExclusiveMessage(BufferFromBytes(raw.data(), static_cast<unsigned>(raw.size())));
		}
		if (status == 0xf6)
		{
			return ref new MidiTuneRequestMessage();
		}
		if (status == 0xf8)
		{
			return ref new MidiTimingClockMessage();
		}
		if (status == 0xfa)
		{
			return ref new MidiStartMessage();
		}
		if (status == 0xfb)
		{
			return ref new MidiContinueMessage();
		}
		if (status == 0xfc)
		{
			return ref new MidiStopMessage();
		}
		if (status == 0xfe)
		{
			return ref new MidiActiveSensingMessage();
		}
		if (status == 0xff)
		{
			return ref new MidiSystemResetMessage();
		}
		return nullptr;
	}
}

extern "C" void bx_uwp_audio_host_open(unsigned sample_rate, unsigned bits,
	unsigned channels)
{
	std::lock_guard<std::mutex> lock(g_audioMutex);
	OpenAudioLocked(sample_rate, bits, channels);
}

extern "C" void bx_uwp_audio_host_submit(const void *data, unsigned length,
	unsigned sample_rate, unsigned bits, unsigned channels)
{
	if (data == nullptr || length == 0)
	{
		return;
	}

	std::lock_guard<std::mutex> lock(g_audioMutex);
	if (!OpenAudioLocked(sample_rate, bits, channels) || g_sourceVoice == nullptr)
	{
		return;
	}

	XAUDIO2_VOICE_STATE state = {};
	g_sourceVoice->GetState(&state);
	if (state.BuffersQueued > 8)
	{
		return;
	}

	AudioBufferContext *context = new AudioBufferContext();
	context->data.resize(length);
	std::memcpy(context->data.data(), data, length);

	XAUDIO2_BUFFER buffer = {};
	buffer.AudioBytes = length;
	buffer.pAudioData = context->data.data();
	buffer.pContext = context;

	if (FAILED(g_sourceVoice->SubmitSourceBuffer(&buffer)))
	{
		delete context;
	}
}

extern "C" void bx_uwp_audio_host_close(void)
{
	std::lock_guard<std::mutex> lock(g_audioMutex);
	CloseAudioLocked();
}

extern "C" int bx_uwp_audio_host_input_open(unsigned sample_rate, unsigned bits,
	unsigned channels)
{
	std::lock_guard<std::mutex> lock(g_captureMutex);
	return OpenCaptureLocked(sample_rate, bits, channels) ? 0 : -1;
}

extern "C" int bx_uwp_audio_host_input_get(void *data, unsigned length)
{
	if (data == nullptr || length == 0)
	{
		return 0;
	}

	std::lock_guard<std::mutex> lock(g_captureMutex);
	unsigned char *out = static_cast<unsigned char *>(data);
	unsigned copied = 0;
	while (copied < length && !g_captureQueue.empty())
	{
		out[copied++] = g_captureQueue.front();
		g_captureQueue.pop_front();
	}
	if (copied < length)
	{
		std::memset(out + copied, 0, length - copied);
	}
	return g_captureReady ? 0 : -1;
}

extern "C" void bx_uwp_audio_host_input_close(void)
{
	std::lock_guard<std::mutex> lock(g_captureMutex);
	CloseCaptureLocked();
}

extern "C" int bx_uwp_midi_host_open(const char *device)
{
	UNREFERENCED_PARAMETER(device);
	std::lock_guard<std::mutex> lock(g_midiMutex);
	if (g_midiReady && g_midiSynthesizer != nullptr)
	{
		return 0;
	}

	try
	{
		g_midiSynthesizer = create_task(MidiSynthesizer::CreateAsync()).get();
		g_midiReady = g_midiSynthesizer != nullptr;
		return g_midiReady ? 0 : -1;
	}
	catch (...)
	{
		g_midiSynthesizer = nullptr;
		g_midiReady = false;
		return -1;
	}
}

extern "C" int bx_uwp_midi_host_ready(void)
{
	std::lock_guard<std::mutex> lock(g_midiMutex);
	return g_midiReady && g_midiSynthesizer != nullptr ? 0 : -1;
}

extern "C" int bx_uwp_midi_host_send(unsigned delta, unsigned command,
	unsigned length, const unsigned char *data)
{
	UNREFERENCED_PARAMETER(delta);
	std::lock_guard<std::mutex> lock(g_midiMutex);
	if (!g_midiReady || g_midiSynthesizer == nullptr)
	{
		return -1;
	}

	try
	{
		IMidiMessage^ message = CreateMidiMessage(command, length, data);
		if (message == nullptr)
		{
			return -1;
		}
		g_midiSynthesizer->SendMessage(message);
		return 0;
	}
	catch (...)
	{
		return -1;
	}
}

extern "C" void bx_uwp_midi_host_close(void)
{
	std::lock_guard<std::mutex> lock(g_midiMutex);
	g_midiSynthesizer = nullptr;
	g_midiReady = false;
}

void UWP_Port::BochsUwpAudio::Shutdown()
{
	bx_uwp_audio_host_close();
	bx_uwp_audio_host_input_close();
	bx_uwp_midi_host_close();
}
