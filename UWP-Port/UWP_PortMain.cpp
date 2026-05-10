#include "pch.h"
#include "UWP_PortMain.h"
#include "BochsUwpAudio.h"
#include "BochsUwpBridge.h"

using namespace UWP_Port;
using namespace Windows::Foundation;
using namespace Windows::System::Threading;
using namespace Concurrency;

UWP_PortMain::UWP_PortMain(const std::shared_ptr<DX::DeviceResources>& deviceResources) :
	m_deviceResources(deviceResources),
	m_lastPointerX(0.0f),
	m_lastPointerY(0.0f)
{
	m_deviceResources->RegisterDeviceNotify(this);
	m_frameRenderer = std::unique_ptr<BochsFrameRenderer>(new BochsFrameRenderer(m_deviceResources));
	m_bochsRuntime = std::unique_ptr<BochsRuntime>(new BochsRuntime());
}

UWP_PortMain::~UWP_PortMain()
{
	if (m_bochsRuntime)
	{
		m_bochsRuntime->RequestShutdown();
	}
	BochsUwpAudio::Shutdown();
	m_deviceResources->RegisterDeviceNotify(nullptr);
}

void UWP_PortMain::CreateWindowSizeDependentResources()
{
	m_frameRenderer->CreateWindowSizeDependentResources();
}

void UWP_PortMain::StartEmulation(Platform::String^ configPath)
{
	StartEmulation(configPath, nullptr);
}

void UWP_PortMain::StartEmulation(Platform::String^ configPath, Platform::String^ restorePath)
{
	if (!m_bochsRuntime)
	{
		m_bochsRuntime = std::unique_ptr<BochsRuntime>(new BochsRuntime());
	}
	m_bochsRuntime->Start(configPath, restorePath);
}

void UWP_PortMain::StartRenderLoop()
{
	if (m_renderLoopWorker != nullptr && m_renderLoopWorker->Status == AsyncStatus::Started)
	{
		return;
	}

	auto workItemHandler = ref new WorkItemHandler([this](IAsyncAction^ action)
	{
		while (action->Status == AsyncStatus::Started)
		{
			critical_section::scoped_lock lock(m_criticalSection);
			Update();
			if (Render())
			{
				m_deviceResources->Present();
			}
		}
	});

	m_renderLoopWorker = ThreadPool::RunAsync(workItemHandler, WorkItemPriority::High, WorkItemOptions::TimeSliced);
}

void UWP_PortMain::StopRenderLoop()
{
	if (m_renderLoopWorker != nullptr)
	{
		m_renderLoopWorker->Cancel();
	}
}

void UWP_PortMain::Update()
{
	ProcessInput();
	m_timer.Tick([&]() {});
}

void UWP_PortMain::ProcessInput()
{
}

bool UWP_PortMain::Render()
{
	if (m_timer.GetFrameCount() == 0)
	{
		return false;
	}

	return m_frameRenderer->Render();
}

void UWP_PortMain::OnDeviceLost()
{
	m_frameRenderer->ReleaseDeviceDependentResources();
}

void UWP_PortMain::OnDeviceRestored()
{
	m_frameRenderer->CreateDeviceDependentResources();
	CreateWindowSizeDependentResources();
}

void UWP_PortMain::PointerPressed(float x, float y, unsigned buttons, bool absolute)
{
	m_lastPointerX = x;
	m_lastPointerY = y;
	BochsUwpBridge::SendPointer(static_cast<int>(x), static_cast<int>(y), 0, buttons, absolute);
}

void UWP_PortMain::PointerMoved(float x, float y, unsigned buttons, bool absolute)
{
	m_lastPointerX = x;
	m_lastPointerY = y;
	BochsUwpBridge::SendPointer(static_cast<int>(x), absolute ? static_cast<int>(y) : -static_cast<int>(y), 0, buttons, absolute);
}

void UWP_PortMain::PointerReleased(float x, float y, unsigned buttons, bool absolute)
{
	m_lastPointerX = x;
	m_lastPointerY = y;
	BochsUwpBridge::SendPointer(static_cast<int>(x), absolute ? static_cast<int>(y) : -static_cast<int>(y), 0, buttons, absolute);
}

void UWP_PortMain::PointerWheelChanged(float x, float y, int wheelDelta, unsigned buttons, bool absolute)
{
	m_lastPointerX = x;
	m_lastPointerY = y;

	int wheel = wheelDelta / 120;
	if (wheel == 0 && wheelDelta != 0)
	{
		wheel = wheelDelta > 0 ? 1 : -1;
	}

	if (wheel != 0)
	{
		BochsUwpBridge::SendPointer(static_cast<int>(x), static_cast<int>(y), wheel, buttons, absolute);
	}
}

void UWP_PortMain::KeyChanged(unsigned nativeKey, bool pressed)
{
	BochsUwpBridge::SendNativeKey(nativeKey, pressed);
}

void UWP_PortMain::FocusChanged(bool focused)
{
	BochsUwpBridge::SendFocus(focused);
}

void UWP_PortMain::RequestMouseCapture(bool enabled)
{
	BochsUwpBridge::RequestMouseCapture(enabled);
}

void UWP_PortMain::PauseEmulation()
{
	m_bochsRuntime->Pause();
}

void UWP_PortMain::ResumeEmulation()
{
	m_bochsRuntime->Resume();
}

bool UWP_PortMain::SaveEmulationStateIfPossible()
{
	return m_bochsRuntime->SaveStateIfPossible();
}

bool UWP_PortMain::SaveEmulationStateIfPossible(Platform::String^ savePath)
{
	return m_bochsRuntime->SaveStateIfPossible(savePath);
}

void UWP_PortMain::ShutdownEmulation()
{
	if (m_bochsRuntime)
	{
		std::unique_ptr<BochsRuntime> runtime = std::move(m_bochsRuntime);
		runtime->RequestShutdown();
	}
	m_bochsRuntime = std::unique_ptr<BochsRuntime>(new BochsRuntime());
	BochsUwpAudio::Shutdown();
}

bool UWP_PortMain::IsEmulationRunning() const
{
	return m_bochsRuntime && m_bochsRuntime->IsRunning();
}

bool UWP_PortMain::HasEmulationExited() const
{
	return m_bochsRuntime && m_bochsRuntime->HasExited();
}

bool UWP_PortMain::HasEmulationError() const
{
	return m_bochsRuntime && m_bochsRuntime->HasError();
}

Platform::String^ UWP_PortMain::LastEmulationError() const
{
	return m_bochsRuntime ? m_bochsRuntime->LastError() : nullptr;
}
