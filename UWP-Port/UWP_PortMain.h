#pragma once

#include "Common\StepTimer.h"
#include "Common\DeviceResources.h"
#include "Content\BochsFrameRenderer.h"
#include "BochsRuntime.h"

// Renderiza conteúdo Direct2D e 3D na tela.
namespace UWP_Port
{
	class UWP_PortMain : public DX::IDeviceNotify
	{
	public:
		UWP_PortMain(const std::shared_ptr<DX::DeviceResources>& deviceResources);
		~UWP_PortMain();
		void CreateWindowSizeDependentResources();
		void StartEmulation(Platform::String^ configPath);
		void StartEmulation(Platform::String^ configPath, Platform::String^ restorePath);
		void PointerPressed(float x, float y, unsigned buttons, bool absolute);
		void PointerMoved(float x, float y, unsigned buttons, bool absolute);
		void PointerReleased(float x, float y, unsigned buttons, bool absolute);
		void PointerWheelChanged(float x, float y, int wheelDelta, unsigned buttons, bool absolute);
		void KeyChanged(unsigned nativeKey, bool pressed);
		void FocusChanged(bool focused);
		void RequestMouseCapture(bool enabled);
		void PauseEmulation();
		void ResumeEmulation();
		bool SaveEmulationStateIfPossible();
		bool SaveEmulationStateIfPossible(Platform::String^ savePath);
		void ShutdownEmulation();
		bool IsEmulationRunning() const;
		bool HasEmulationExited() const;
		bool HasEmulationError() const;
		Platform::String^ LastEmulationError() const;
		void StartRenderLoop();
		void StopRenderLoop();
		Concurrency::critical_section& GetCriticalSection() { return m_criticalSection; }

		// IDeviceNotify
		virtual void OnDeviceLost();
		virtual void OnDeviceRestored();

	private:
		void ProcessInput();
		void Update();
		bool Render();

		// Ponteiro armazenado em cache para recursos de dispositivo.
		std::shared_ptr<DX::DeviceResources> m_deviceResources;

		// TODO: substitua pelos seus próprios renderizadores de conteúdo.
		std::unique_ptr<BochsFrameRenderer> m_frameRenderer;
		std::unique_ptr<BochsRuntime> m_bochsRuntime;

		Windows::Foundation::IAsyncAction^ m_renderLoopWorker;
		Concurrency::critical_section m_criticalSection;

		// Temporizador do loop de renderização.
		DX::StepTimer m_timer;

		// Rastreie a posição atual do ponteiro de entrada.
		float m_lastPointerX;
		float m_lastPointerY;
	};
}
