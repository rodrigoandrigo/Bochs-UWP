#pragma once

#include "..\gui\uwp_dx_bridge.h"
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace UWP_Port
{
	struct BochsFrameSnapshot
	{
		unsigned width;
		unsigned height;
		unsigned pitch;
		unsigned bpp;
		bool valid;
		bool dirty;
		std::vector<uint32_t> pixels;
		std::string statusText;
	};

	class BochsUwpBridge
	{
	public:
		static BochsFrameSnapshot CopyFrame();
		static void SendNativeKey(unsigned nativeKey, bool pressed);
		static void SendPointer(int x, int y, int z, unsigned buttons, bool absolute);
		static void SendFocus(bool focused);
		static void RequestMouseCapture(bool enabled);
		static void RequestShutdown();
		static bool IsMouseCaptured();
		static bool IsMouseAbsolute();
	};
}
