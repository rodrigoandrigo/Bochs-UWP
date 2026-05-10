#include "pch.h"
#include "BochsUwpBridge.h"

#include <algorithm>
#include <cstring>

using namespace UWP_Port;

namespace
{
	std::mutex g_bridgeMutex;
	bx_uwp_dx_sink_t g_sink = {};
	BochsFrameSnapshot g_frame = {};
	bool g_mouseCaptured = false;
	bool g_mouseAbsolute = false;
	bool g_presentRequested = false;
}

extern "C" void bx_uwp_dx_host_set_sink(const bx_uwp_dx_sink_t *sink)
{
	std::lock_guard<std::mutex> lock(g_bridgeMutex);
	g_sink = sink ? *sink : bx_uwp_dx_sink_t{};
}

extern "C" void bx_uwp_dx_host_configure(unsigned width, unsigned height,
	unsigned pitch, unsigned bpp)
{
	std::lock_guard<std::mutex> lock(g_bridgeMutex);
	g_frame.width = width;
	g_frame.height = height;
	g_frame.pitch = pitch;
	g_frame.bpp = bpp;
	g_frame.valid = (width > 0) && (height > 0) && (pitch >= width * 4) && (bpp == 32);
	g_frame.dirty = true;
	g_frame.pixels.assign(g_frame.valid ? width * height : 0, 0xff000000);
}

extern "C" void bx_uwp_dx_host_update_rect(const void *bgra, unsigned sourcePitch,
	unsigned x, unsigned y, unsigned width, unsigned height)
{
	if (bgra == nullptr || width == 0 || height == 0)
	{
		return;
	}

	std::lock_guard<std::mutex> lock(g_bridgeMutex);
	if (!g_frame.valid || x >= g_frame.width || y >= g_frame.height)
	{
		return;
	}

	width = (std::min)(width, g_frame.width - x);
	height = (std::min)(height, g_frame.height - y);

	const unsigned char *src = static_cast<const unsigned char *>(bgra);
	uint32_t *dst = g_frame.pixels.data() + y * g_frame.width + x;
	const unsigned rowBytes = width * 4;
	for (unsigned row = 0; row < height; row++)
	{
		std::memcpy(dst, src, rowBytes);
		src += sourcePitch;
		dst += g_frame.width;
	}

	g_frame.dirty = true;
}

extern "C" void bx_uwp_dx_host_present(void)
{
	std::lock_guard<std::mutex> lock(g_bridgeMutex);
	g_presentRequested = true;
}

extern "C" void bx_uwp_dx_host_clear(unsigned bgra)
{
	std::lock_guard<std::mutex> lock(g_bridgeMutex);
	if (!g_frame.valid)
	{
		return;
	}

	for (unsigned y = 0; y < g_frame.height; y++)
	{
		uint32_t *row = g_frame.pixels.data() + y * g_frame.width;
		std::fill(row, row + g_frame.width, bgra);
	}
	g_frame.dirty = true;
	g_presentRequested = true;
}

extern "C" void bx_uwp_dx_host_set_mouse_capture(int enabled)
{
	std::lock_guard<std::mutex> lock(g_bridgeMutex);
	g_mouseCaptured = (enabled != 0);
}

extern "C" void bx_uwp_dx_host_set_mouse_mode(int absolute)
{
	std::lock_guard<std::mutex> lock(g_bridgeMutex);
	g_mouseAbsolute = (absolute != 0);
}

extern "C" void bx_uwp_dx_host_set_status_text(const char *text)
{
	std::lock_guard<std::mutex> lock(g_bridgeMutex);
	g_frame.statusText = text ? text : "";
}

BochsFrameSnapshot BochsUwpBridge::CopyFrame()
{
	std::lock_guard<std::mutex> lock(g_bridgeMutex);
	BochsFrameSnapshot copy = g_frame;
	copy.dirty = g_frame.dirty || g_presentRequested;
	g_frame.dirty = false;
	g_presentRequested = false;
	return copy;
}

void BochsUwpBridge::SendNativeKey(unsigned nativeKey, bool pressed)
{
	bx_uwp_dx_native_key_cb callback = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_bridgeMutex);
		callback = g_sink.key_event;
	}
	if (callback)
	{
		callback(nativeKey, pressed ? 1 : 0);
	}
}

void BochsUwpBridge::SendPointer(int x, int y, int z, unsigned buttons, bool absolute)
{
	bx_uwp_dx_pointer_cb callback = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_bridgeMutex);
		callback = g_sink.pointer_event;
	}
	if (callback)
	{
		callback(x, y, z, buttons, absolute ? 1 : 0);
	}
}

void BochsUwpBridge::SendFocus(bool focused)
{
	bx_uwp_dx_focus_cb callback = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_bridgeMutex);
		callback = g_sink.focus_event;
	}
	if (callback)
	{
		callback(focused ? 1 : 0);
	}
}

void BochsUwpBridge::RequestMouseCapture(bool enabled)
{
	bx_uwp_dx_mouse_capture_cb callback = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_bridgeMutex);
		callback = g_sink.mouse_capture_event;
	}
	if (callback)
	{
		callback(enabled ? 1 : 0);
	}
}

void BochsUwpBridge::RequestShutdown()
{
	bx_uwp_dx_shutdown_cb callback = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_bridgeMutex);
		callback = g_sink.shutdown_event;
	}
	if (callback)
	{
		callback();
	}
}

bool BochsUwpBridge::IsMouseCaptured()
{
	std::lock_guard<std::mutex> lock(g_bridgeMutex);
	return g_mouseCaptured;
}

bool BochsUwpBridge::IsMouseAbsolute()
{
	std::lock_guard<std::mutex> lock(g_bridgeMutex);
	return g_mouseAbsolute;
}
