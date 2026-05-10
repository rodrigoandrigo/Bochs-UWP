#include "pch.h"
#include "bochs_core_uwp.h"

#include "bochs.h"
#include "gui/siminterface.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern int bxmain(void);

namespace
{
	std::mutex g_controlMutex;
	std::condition_variable g_controlChanged;
	std::mutex g_logMutex;
	std::string g_recentLog;
	bool g_running = false;
	bool g_pauseRequested = false;
	bool g_pauseObserved = false;
	bool g_shutdownRequested = false;
	const size_t MaxRecentLogBytes = 16384;

	void set_initial_dir_from_config(const char *config_path)
	{
#ifdef WIN32
		bx_startup_flags.initial_dir[0] = 0;
		if (config_path == nullptr)
		{
			return;
		}

		strncpy_s(bx_startup_flags.initial_dir, config_path, _TRUNCATE);
		size_t length = strlen(bx_startup_flags.initial_dir);
		while (length > 0 &&
			bx_startup_flags.initial_dir[length - 1] != '\\' &&
			bx_startup_flags.initial_dir[length - 1] != '/')
		{
			length--;
		}
		bx_startup_flags.initial_dir[length] = 0;
#else
		(void)config_path;
#endif
	}

	void set_core_running(bool running)
	{
		std::lock_guard<std::mutex> lock(g_controlMutex);
		g_running = running;
		if (!running)
		{
			g_pauseObserved = false;
		}
		g_controlChanged.notify_all();
	}

	void trim_recent_log()
	{
		if (g_recentLog.size() <= MaxRecentLogBytes)
		{
			return;
		}

		size_t excess = g_recentLog.size() - MaxRecentLogBytes;
		size_t lineStart = g_recentLog.find('\n', excess);
		g_recentLog.erase(0, lineStart == std::string::npos ? excess : lineStart + 1);
	}
}

int bochs_core_uwp_run(const char *config_path)
{
	return bochs_core_uwp_run_with_restore(config_path, nullptr);
}

int bochs_core_uwp_run_with_restore(const char *config_path, const char *restore_path)
{
	if (config_path == nullptr || config_path[0] == 0)
	{
		if (restore_path == nullptr || restore_path[0] == 0)
		{
			return -1;
		}
	}

	std::vector<std::string> args;
	args.push_back("bochs");
	args.push_back("-q");
	if (restore_path != nullptr && restore_path[0] != 0)
	{
		args.push_back("-r");
		args.push_back(restore_path);
	}
	else
	{
		args.push_back("-f");
		args.push_back(config_path);
	}

	std::vector<char *> argv;
	for (std::string &arg : args)
	{
		argv.push_back(&arg[0]);
	}

	set_initial_dir_from_config(config_path);
	bx_startup_flags.argc = static_cast<int>(argv.size());
	bx_startup_flags.argv = argv.data();
	bochs_core_uwp_clear_log();

	{
		std::lock_guard<std::mutex> lock(g_controlMutex);
		g_shutdownRequested = false;
		g_pauseObserved = false;
		g_running = true;
	}
	g_controlChanged.notify_all();

	int result = bxmain();
	set_core_running(false);
	return result;
}

void bochs_core_uwp_pause(void)
{
	std::lock_guard<std::mutex> lock(g_controlMutex);
	g_pauseRequested = true;
	g_controlChanged.notify_all();
}

void bochs_core_uwp_resume(void)
{
	std::lock_guard<std::mutex> lock(g_controlMutex);
	g_pauseRequested = false;
	g_pauseObserved = false;
	g_controlChanged.notify_all();
}

void bochs_core_uwp_request_shutdown(void)
{
	std::lock_guard<std::mutex> lock(g_controlMutex);
	g_shutdownRequested = true;
	g_pauseRequested = false;
	g_pauseObserved = false;
	g_controlChanged.notify_all();
}

int bochs_core_uwp_wait_until_paused(unsigned timeout_ms)
{
	std::unique_lock<std::mutex> lock(g_controlMutex);
	if (!g_running || !g_pauseRequested || g_shutdownRequested)
	{
		return 1;
	}

	return g_controlChanged.wait_for(lock, std::chrono::milliseconds(timeout_ms), []()
	{
		return !g_running || !g_pauseRequested || g_pauseObserved || g_shutdownRequested;
	}) ? 1 : 0;
}

int bochs_core_uwp_poll(void)
{
	std::unique_lock<std::mutex> lock(g_controlMutex);
	if (g_shutdownRequested)
	{
		return 1;
	}

	if (g_pauseRequested)
	{
		g_pauseObserved = true;
		g_controlChanged.notify_all();
		g_controlChanged.wait(lock, []()
		{
			return !g_pauseRequested || g_shutdownRequested;
		});
		g_pauseObserved = false;
	}

	return g_shutdownRequested ? 1 : 0;
}

int bochs_core_uwp_save_state(const char *checkpoint_path)
{
	if (checkpoint_path == nullptr || checkpoint_path[0] == 0)
	{
		return 0;
	}

	{
		std::lock_guard<std::mutex> lock(g_controlMutex);
		if (!g_running || g_shutdownRequested)
		{
			return 0;
		}
	}

	return SIM->save_state(checkpoint_path) ? 1 : 0;
}

void bochs_core_uwp_clear_log(void)
{
	std::lock_guard<std::mutex> lock(g_logMutex);
	g_recentLog.clear();
}

void bochs_core_uwp_report_log(int level, const char *prefix, const char *message)
{
	std::lock_guard<std::mutex> lock(g_logMutex);
	if (prefix != nullptr && prefix[0] != 0)
	{
		g_recentLog += prefix;
		g_recentLog += " ";
	}

	switch (level)
	{
	case 0:
		g_recentLog += "DEBUG: ";
		break;
	case 1:
		g_recentLog += "INFO: ";
		break;
	case 2:
		g_recentLog += "ERROR: ";
		break;
	case 3:
		g_recentLog += "PANIC: ";
		break;
	default:
		break;
	}

	if (message != nullptr)
	{
		g_recentLog += message;
	}
	g_recentLog += "\n";
	trim_recent_log();
}

size_t bochs_core_uwp_copy_log(char *buffer, size_t buffer_size)
{
	std::lock_guard<std::mutex> lock(g_logMutex);
	size_t length = g_recentLog.size();
	if (buffer != nullptr && buffer_size > 0)
	{
		size_t copyLength = length < buffer_size - 1 ? length : buffer_size - 1;
		memcpy(buffer, g_recentLog.data(), copyLength);
		buffer[copyLength] = 0;
	}
	return length;
}
