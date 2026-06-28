//
// DirectXPage.xaml.h
// Declaração da classe DirectXPage.
//

#pragma once

#include "DirectXPage.g.h"

#include "Common\DeviceResources.h"
#include "UWP_PortMain.h"

#include <string>

namespace UWP_Port
{
	/// <summary>
	/// Uma página que hospeda um SwapChainPanel do DirectX.
	/// </summary>
	public ref class DirectXPage sealed
	{
	public:
		DirectXPage();
		virtual ~DirectXPage();

		void SaveInternalState(Windows::Foundation::Collections::IPropertySet^ state);
		void LoadInternalState(Windows::Foundation::Collections::IPropertySet^ state);
		void Shutdown();

	private:
		// Manipulador de eventos de renderização de baixo nível XAML.
		void OnRendering(Platform::Object^ sender, Platform::Object^ args);

		// Manipuladores de eventos da janela.
		void OnVisibilityChanged(Windows::UI::Core::CoreWindow^ sender, Windows::UI::Core::VisibilityChangedEventArgs^ args);
		void OnWindowClosed(Windows::UI::Core::CoreWindow^ sender, Windows::UI::Core::CoreWindowEventArgs^ args);
		void OnKeyDown(Windows::UI::Core::CoreWindow^ sender, Windows::UI::Core::KeyEventArgs^ args);
		void OnKeyUp(Windows::UI::Core::CoreWindow^ sender, Windows::UI::Core::KeyEventArgs^ args);
		void OnMouseMoved(Windows::Devices::Input::MouseDevice^ sender, Windows::Devices::Input::MouseEventArgs^ args);

		// Manipuladores de eventos DisplayInformation.
		void OnDpiChanged(Windows::Graphics::Display::DisplayInformation^ sender, Platform::Object^ args);
		void OnOrientationChanged(Windows::Graphics::Display::DisplayInformation^ sender, Platform::Object^ args);
		void OnDisplayContentsInvalidated(Windows::Graphics::Display::DisplayInformation^ sender, Platform::Object^ args);

		// Outros manipuladores de eventos.
		void ChooseDiskButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ResetDiskButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ChooseDisk2Button_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ResetDisk2Button_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ChooseFloppyButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ResetFloppyButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ChooseFloppyBButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ResetFloppyBButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ChooseCdromButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ResetCdromButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ChooseCdrom2Button_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ResetCdrom2Button_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ChooseSharedFolderButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ResetSharedFolderButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ChooseUsbImageButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ResetUsbImageButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void SelectBiosButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ResetBiosButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void SelectVgaBiosButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ResetVgaBiosButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void StartEmulatorButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ToggleTabsButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void MemorySlider_ValueChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs^ e);
		void BootDeviceComboBox_SelectionChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^ e);
		void DiskImageModeComboBox_SelectionChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^ e);
		void FeatureSelection_Changed(Platform::Object^ sender, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^ e);
		void FeatureToggle_Changed(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void NetworkSocketPortTextBox_TextChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::TextChangedEventArgs^ e);
		void PauseButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ResumeButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ShutdownButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void SaveButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void RestoreButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void RuntimeFloppyAButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void RuntimeFloppyBButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void RuntimeCdromButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void RuntimeEjectButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void MachineProfileComboBox_SelectionChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^ e);
		void SaveStateSlotComboBox_SelectionChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^ e);
		void VideoSlider_ValueChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs^ e);
		void ClearProblemsButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void BootTabButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void SettingsTabButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void AboutTabButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ProblemsTabButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void BochsrcTabButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void CheckForSavedState();
		void StartPreparedEmulator(bool restoreSavedState);
		void SaveSelectedState();
		void RestoreSelectedState();
		void MonitorStartupAfterDelay();
		void FocusEmulatorSurface();
		void HandleStartupFailure(Platform::String^ message);
		void HandleAsyncFailure(Platform::String^ message);
		void RfbStatusTimer_Tick(Platform::Object^ sender, Platform::Object^ e);
		void UpdateRfbStatusOverlay();
		void SelectConfigurationPanel(int panelIndex);
		void AppendProblemLog(Platform::String^ message);
		void SetRichTextBlockText(Windows::UI::Xaml::Controls::RichTextBlock^ block, Platform::String^ text);
		void UpdateProblemLogView();
		void UpdateDiagnosticSummary();
		void UpdateSaveStateSlotStatus();
		void UpdateBochsrcPreview();
		void UpdateDiskImageModeLabel();
		int SelectedMemoryMb();
		Platform::String^ SelectedCpuModel();
		Platform::String^ SelectedDiskImageMode();
		Platform::String^ SelectedDisk2ImageMode();
		Platform::String^ EffectiveDiskImageMode();
		Platform::String^ EffectiveDisk2ImageMode();
		Platform::String^ SelectedBootSequence();
		Platform::String^ SelectedSaveStateSlotId();
		Platform::String^ SelectedSaveStateSlotLabel();
		Platform::String^ SelectedSaveStateFolderPath();
		bool SelectedSoundEnabled();
		Platform::String^ SelectedNetworkAdapter();
		int SelectedNetworkSocketPort();
		Platform::String^ SelectedUsbController();
		Platform::String^ SelectedUsbDevice();
		Platform::String^ SelectedSerialMode();
		bool SelectedParallelEnabled();
		Platform::String^ SelectedPciChipset();
		bool SelectedAcpiEnabled();
		bool SelectedHpetEnabled();
		bool SelectedIoApicEnabled();
		Platform::String^ SelectedDisplayLibrary();
		int SelectedRfbTimeout();
		Platform::String^ SelectedVideoExtension();
		int SelectedVideoUpdateFreq();
		bool SelectedVideoRealtime();
		int SelectedVbeMemoryMb();
		void SetBootSequencePrimary(const wchar_t *primary);
		void ApplyMachineProfile(Platform::String^ profile);
		void MarkMachineProfileCustom();
		void UpdateVideoLabel();
		void UpdatePlatformFeatureState();
		void InsertRuntimeMedia(const wchar_t *target);
		void EjectRuntimeMedia();
		void UpdateMemoryLabel();
		void UpdateCommandState();
		void UpdateCaptureIndicators();
		void OnCompositionScaleChanged(Windows::UI::Xaml::Controls::SwapChainPanel^ sender, Object^ args);
		void OnSwapChainPanelSizeChanged(Platform::Object^ sender, Windows::UI::Xaml::SizeChangedEventArgs^ e);

		// Rastreie nossa entrada independente em um thread de trabalho de segundo plano.

		// Funções de manipulação de entrada independente.
		void OnPointerPressed(Platform::Object^ sender, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e);
		void OnPointerMoved(Platform::Object^ sender, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e);
		void OnPointerReleased(Platform::Object^ sender, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e);
		void OnPointerWheelChanged(Platform::Object^ sender, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e);
		unsigned PointerButtonMask(Windows::UI::Input::PointerPointProperties^ properties);
		Windows::Foundation::Point ScalePointerToGuest(Windows::Foundation::Point point);
		unsigned NormalizeVirtualKey(Windows::System::VirtualKey virtualKey, Windows::UI::Core::CorePhysicalKeyStatus keyStatus);
		bool HandleMouseCaptureShortcut(Windows::UI::Core::KeyEventArgs^ args, bool pressed);
		void ToggleMouseCaptureFromShortcut();
		void UpdateMouseCaptureFromCore();
		void ReleaseMouseCapture();

		// Recursos usados para renderizar o conteúdo do DirectX na tela de fundo da página XAML.
		std::shared_ptr<DX::DeviceResources> m_deviceResources;
		std::unique_ptr<UWP_PortMain> m_main; 
		bool m_windowVisible;
		bool m_emulationStarted;
		bool m_emulationPaused;
		bool m_saveStateAvailable;
		bool m_mouseCaptured;
		bool m_mouseCapturePending;
		bool m_mouseCaptureUserReleased;
		bool m_mouseCaptureShortcutDown;
		bool m_tabsEnabled;
		bool m_isPageInitialized;
		bool m_applyingMachineProfile;
		bool m_rfbDisplayActive;
		int m_selectedPanelIndex;
		unsigned m_pointerButtons;
		Windows::Foundation::Point m_lastPointerPosition;
		Platform::String^ m_selectedDiskPath;
		Platform::String^ m_selectedDisk2Path;
		Platform::String^ m_selectedFloppyPath;
		Platform::String^ m_selectedFloppyBPath;
		Platform::String^ m_selectedCdromPath;
		Platform::String^ m_selectedCdrom2Path;
		Platform::String^ m_selectedSharedFolderPath;
		Platform::String^ m_selectedUsbImagePath;
		Platform::String^ m_runtimeMediaTarget;
		Platform::String^ m_bootTarget;
		Platform::String^ m_selectedBiosPath;
		Platform::String^ m_selectedVgaBiosPath;
		Windows::UI::Xaml::DispatcherTimer^ m_rfbStatusTimer;
		std::wstring m_problemLog;
		std::wstring m_lastBochsrcPreview;
		unsigned m_problemCount;
	};
}

