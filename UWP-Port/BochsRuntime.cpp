#include "pch.h"
#include "BochsRuntime.h"
#include "BochsUwpStorage.h"
#include "BochsUwpBridge.h"

#include "..\bochs_core_uwp\bochs_core_uwp.h"

#include <exception>
#include <string>
#include <vector>

using namespace UWP_Port;

#ifndef BX_UWP_ENABLE_BOCHS_CORE
#define BX_UWP_ENABLE_BOCHS_CORE 0
#endif

namespace
{
	std::string PlatformStringToUtf8(Platform::String^ value)
	{
		if (value == nullptr)
		{
			return std::string();
		}

		int length = WideCharToMultiByte(CP_UTF8, 0, value->Data(), -1, nullptr, 0, nullptr, nullptr);
		if (length <= 1)
		{
			return std::string();
		}

		std::string result(static_cast<size_t>(length - 1), '\0');
		WideCharToMultiByte(CP_UTF8, 0, value->Data(), -1, &result[0], length, nullptr, nullptr);
		return result;
	}

	std::wstring Utf8ToWide(const char *value)
	{
		if (value == nullptr || value[0] == 0)
		{
			return std::wstring();
		}

		int length = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
		if (length <= 1)
		{
			return std::wstring();
		}

		std::wstring result(static_cast<size_t>(length - 1), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, value, -1, &result[0], length);
		return result;
	}

	std::wstring RecentCoreLog()
	{
		size_t length = bochs_core_uwp_copy_log(nullptr, 0);
		if (length == 0)
		{
			return std::wstring();
		}

		std::vector<char> buffer(length + 1, 0);
		bochs_core_uwp_copy_log(buffer.data(), buffer.size());
		return Utf8ToWide(buffer.data());
	}

	void AppendRecentCoreLog(std::wstring& message)
	{
		std::wstring log = RecentCoreLog();
		if (!log.empty())
		{
			message += L"\n\nRecent Bochs log:\n";
			message += log;
		}
	}
}

BochsRuntime::BochsRuntime() :
	m_started(false),
	m_running(false),
	m_paused(true),
	m_shutdownRequested(false),
	m_exited(false),
	m_hasError(false)
{
}

BochsRuntime::~BochsRuntime()
{
	RequestShutdown();
}

void BochsRuntime::Start()
{
	Start(nullptr);
}

void BochsRuntime::Start(Platform::String^ configPath)
{
	Start(configPath, nullptr);
}

void BochsRuntime::Start(Platform::String^ configPath, Platform::String^ restorePath)
{
	bool expected = false;
	if (!m_started.compare_exchange_strong(expected, true))
	{
		Resume();
		return;
	}

	m_shutdownRequested = false;
	m_exited = false;
	m_paused = false;
	m_configPath = configPath ? configPath->Data() : L"";
	m_restorePath = restorePath ? restorePath->Data() : L"";
	ClearLastError();
	bochs_core_uwp_resume();
	try
	{
		m_thread = std::thread(&BochsRuntime::Run, this);
	}
	catch (const std::exception& ex)
	{
		std::wstring message(L"Failed to create the emulator thread: ");
		int length = MultiByteToWideChar(CP_UTF8, 0, ex.what(), -1, nullptr, 0);
		if (length > 1)
		{
			std::wstring details(static_cast<size_t>(length - 1), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, ex.what(), -1, &details[0], length);
			message += details;
		}
		SetLastError(message);
		m_started = false;
		m_exited = true;
		m_running = false;
	}
	catch (...)
	{
		SetLastError(L"Unknown failure while creating the emulator thread.");
		m_started = false;
		m_exited = true;
		m_running = false;
	}
}

bool BochsRuntime::Pause()
{
	m_paused = true;
	bochs_core_uwp_pause();
	bool paused = bochs_core_uwp_wait_until_paused(2000) != 0;
	m_stateChanged.notify_all();
	return paused;
}

void BochsRuntime::Resume()
{
	if (!m_started.load())
	{
		return;
	}

	m_paused = false;
	bochs_core_uwp_resume();
	m_stateChanged.notify_all();
}

bool BochsRuntime::SaveStateIfPossible()
{
	return SaveStateIfPossible(BochsUwpStorage::GetSaveStateFolderPath());
}

bool BochsRuntime::SaveStateIfPossible(Platform::String^ savePath)
{
	if (!m_started.load() || m_shutdownRequested.load())
	{
		return false;
	}

	if (!Pause())
	{
		return false;
	}
	std::string savePathUtf8 = PlatformStringToUtf8(savePath);
	return bochs_core_uwp_save_state(savePathUtf8.c_str()) != 0;
}

void BochsRuntime::RequestShutdown()
{
	m_shutdownRequested = true;
	m_paused = false;
	bochs_core_uwp_request_shutdown();
	m_stateChanged.notify_all();
	BochsUwpBridge::RequestShutdown();

	if (m_thread.joinable())
	{
		m_thread.join();
	}

	m_running = false;
	m_started = false;
	m_exited = true;
}

bool BochsRuntime::IsRunning() const
{
	return m_running.load();
}

bool BochsRuntime::IsPaused() const
{
	return m_paused.load();
}

bool BochsRuntime::HasExited() const
{
	return m_exited.load();
}

bool BochsRuntime::HasError() const
{
	return m_hasError.load();
}

Platform::String^ BochsRuntime::LastError() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_lastError.empty())
	{
		return nullptr;
	}

	return ref new Platform::String(m_lastError.c_str());
}

void BochsRuntime::SetLastError(const std::wstring& message)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_lastError = message;
	m_hasError = !m_lastError.empty();
	m_stateChanged.notify_all();
}

void BochsRuntime::ClearLastError()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_lastError.clear();
	m_hasError = false;
}

void BochsRuntime::Run()
{
	m_running = true;

#if BX_UWP_ENABLE_BOCHS_CORE
	try
	{
		Platform::String^ configPath = m_configPath.empty()
			? BochsUwpStorage::EnsureDefaultConfigAsync().get()
			: ref new Platform::String(m_configPath.c_str());
		Platform::String^ restorePath = m_restorePath.empty()
			? nullptr
			: ref new Platform::String(m_restorePath.c_str());
		if (!m_shutdownRequested.load())
		{
			std::string configPathUtf8 = PlatformStringToUtf8(configPath);
			std::string restorePathUtf8 = PlatformStringToUtf8(restorePath);
			int result = bochs_core_uwp_run_with_restore(configPathUtf8.c_str(), restorePathUtf8.c_str());
			if (!m_shutdownRequested.load() && result != 0)
			{
				std::wstring message(L"bochs_core_uwp_run_with_restore failed with code ");
				message += std::to_wstring(result);
				message += L".";
				AppendRecentCoreLog(message);
				SetLastError(message);
			}
		}
	}
	catch (Platform::Exception^ ex)
	{
		std::wstring message(L"Failed to start the Bochs core");
		if (ex != nullptr && ex->Message != nullptr && ex->Message->Length() > 0)
		{
			message += L": ";
			message += ex->Message->Data();
		}
		message += L".";
		AppendRecentCoreLog(message);
		SetLastError(message);
	}
	catch (const std::exception& ex)
	{
		std::wstring message(L"Failed to start the Bochs core: ");
		int length = MultiByteToWideChar(CP_UTF8, 0, ex.what(), -1, nullptr, 0);
		if (length > 1)
		{
			std::wstring details(static_cast<size_t>(length - 1), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, ex.what(), -1, &details[0], length);
			message += details;
		}
		else
		{
			message += L"std::exception without details";
		}
		message += L".";
		AppendRecentCoreLog(message);
		SetLastError(message);
	}
	catch (...)
	{
		std::wstring message(L"Unknown failure while starting the Bochs core.");
		AppendRecentCoreLog(message);
		SetLastError(message);
	}
#else
	std::unique_lock<std::mutex> lock(m_mutex);
	while (!m_shutdownRequested.load())
	{
		m_stateChanged.wait(lock, [this]()
		{
			return m_shutdownRequested.load() || !m_paused.load();
		});

		if (!m_shutdownRequested.load())
		{
			m_stateChanged.wait(lock, [this]()
			{
				return m_shutdownRequested.load() || m_paused.load();
			});
		}
	}
#endif

	m_running = false;
	m_exited = true;
}
