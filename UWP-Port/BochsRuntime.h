#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace UWP_Port
{
	class BochsRuntime
	{
	public:
		BochsRuntime();
		~BochsRuntime();

		void Start();
		void Start(Platform::String^ configPath);
		void Start(Platform::String^ configPath, Platform::String^ restorePath);
		bool Pause();
		void Resume();
		bool SaveStateIfPossible();
		bool SaveStateIfPossible(Platform::String^ savePath);
		void RequestShutdown();
		bool IsRunning() const;
		bool IsPaused() const;
		bool HasExited() const;
		bool HasError() const;
		Platform::String^ LastError() const;

	private:
		void Run();
		void SetLastError(const std::wstring& message);
		void ClearLastError();

		mutable std::mutex m_mutex;
		std::condition_variable m_stateChanged;
		std::thread m_thread;
		std::atomic<bool> m_started;
		std::atomic<bool> m_running;
		std::atomic<bool> m_paused;
		std::atomic<bool> m_shutdownRequested;
		std::atomic<bool> m_exited;
		std::atomic<bool> m_hasError;
		std::wstring m_lastError;
		std::wstring m_configPath;
		std::wstring m_restorePath;
	};
}
