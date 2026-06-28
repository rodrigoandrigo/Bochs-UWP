#include "pch.h"
#include "DirectXPage.xaml.h"
#include "BochsUwpBridge.h"
#include "BochsUwpStorage.h"

#include <cwchar>
#include <string>
#include <vector>

using namespace UWP_Port;

using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Devices::Input;
using namespace Windows::Graphics::Display;
using namespace Windows::Networking;
using namespace Windows::Networking::Connectivity;
using namespace Windows::System;
using namespace Windows::System::Threading;
using namespace Windows::UI::Core;
using namespace Windows::UI::Input;
using namespace Windows::UI::Popups;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Data;
using namespace Windows::UI::Xaml::Documents;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Navigation;
using namespace concurrency;

extern "C" int bx_uwp_rfb_get_port();
extern "C" int bx_uwp_rfb_is_server_running();
extern "C" int bx_uwp_rfb_is_client_connected();

namespace
{
	const unsigned NativeKeyNone = 0;
	const unsigned NativeKeyKeypadEnter = 0x1000;
	const unsigned VkAbntC1 = 0xc1;
	const unsigned VkAbntC2 = 0xc2;

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

	String^ FormatPlatformFailure(const wchar_t *context, Platform::Exception^ ex)
	{
		std::wstring message(context);
		if (ex != nullptr)
		{
			if (ex->Message != nullptr && ex->Message->Length() > 0)
			{
				message += L": ";
				message += ex->Message->Data();
			}

			wchar_t hresult[16];
			swprintf_s(hresult, L"0x%08X", static_cast<unsigned int>(ex->HResult));
			message += L" (HRESULT ";
			message += hresult;
			message += L")";
		}
		message += L".";
		return ref new String(message.c_str());
	}

	String^ FormatStdFailure(const wchar_t *context, const std::exception& ex)
	{
		std::wstring message(context);
		std::wstring details = Utf8ToWide(ex.what());
		if (!details.empty())
		{
			message += L": ";
			message += details;
		}
		message += L".";
		return ref new String(message.c_str());
	}

	unsigned NativeKeyFromPhysicalScanCode(unsigned scanCode, bool extended)
	{
		if (scanCode == 0)
		{
			return NativeKeyNone;
		}

		return BX_UWP_DX_NATIVE_KEY_SCANCODE |
			(extended ? BX_UWP_DX_NATIVE_KEY_EXTENDED : 0) |
			(scanCode & BX_UWP_DX_NATIVE_KEY_CODE_MASK);
	}

	unsigned NativeKeyFromVirtualKey(unsigned virtualKey, CorePhysicalKeyStatus keyStatus)
	{
		switch (virtualKey)
		{
		case 0x0d:
			return keyStatus.IsExtendedKey ? NativeKeyKeypadEnter : virtualKey;
		case 0x10:
			return keyStatus.ScanCode == 0x36 ? 0xa1 : 0xa0;
		case 0x11:
			return keyStatus.IsExtendedKey ? 0xa3 : 0xa2;
		case 0x12:
			return keyStatus.IsExtendedKey ? 0xa5 : 0xa4;
		case VkAbntC1:
			return 0xbf;
		case VkAbntC2:
			return 0x6e;
		default:
			return virtualKey;
		}
	}

	std::wstring BootTagFromComboBox(ComboBox^ comboBox)
	{
		if (comboBox == nullptr)
		{
			return std::wstring();
		}

		ComboBoxItem^ item = dynamic_cast<ComboBoxItem^>(comboBox->SelectedItem);
		if (item == nullptr)
		{
			return std::wstring();
		}

		String^ tag = dynamic_cast<String^>(item->Tag);
		return tag != nullptr && tag->Length() > 0
			? std::wstring(tag->Data())
			: std::wstring();
	}

	bool BootTagAlreadyAdded(const std::vector<std::wstring>& tags, const std::wstring& tag)
	{
		for (const auto& existing : tags)
		{
			if (existing == tag)
			{
				return true;
			}
		}
		return false;
	}

	void AddBootTag(std::vector<std::wstring>& tags, const std::wstring& tag)
	{
		if (tag.empty() || tag == L"none" || BootTagAlreadyAdded(tags, tag) || tags.size() >= 3)
		{
			return;
		}
		tags.push_back(tag);
	}

	void SelectBootComboBoxItem(ComboBox^ comboBox, const wchar_t *tag)
	{
		if (comboBox == nullptr || tag == nullptr)
		{
			return;
		}

		for (unsigned int i = 0; i < comboBox->Items->Size; ++i)
		{
			ComboBoxItem^ item = dynamic_cast<ComboBoxItem^>(comboBox->Items->GetAt(i));
			if (item == nullptr)
			{
				continue;
			}

			String^ itemTag = dynamic_cast<String^>(item->Tag);
			if (itemTag != nullptr && wcscmp(itemTag->Data(), tag) == 0)
			{
				comboBox->SelectedIndex = static_cast<int>(i);
				return;
			}
		}
	}

	String^ ComboBoxTagString(ComboBox^ comboBox, const wchar_t *fallback)
	{
		if (comboBox == nullptr)
		{
			return ref new String(fallback);
		}

		ComboBoxItem^ item = dynamic_cast<ComboBoxItem^>(comboBox->SelectedItem);
		if (item == nullptr)
		{
			return ref new String(fallback);
		}

		String^ tag = dynamic_cast<String^>(item->Tag);
		return tag != nullptr && tag->Length() > 0
			? tag
			: ref new String(fallback);
	}

	void SetTextIfChanged(TextBlock^ block, const std::wstring& text)
	{
		if (block == nullptr)
		{
			return;
		}

		if (block->Text != nullptr && wcscmp(block->Text->Data(), text.c_str()) == 0)
		{
			return;
		}

		block->Text = ref new String(text.c_str());
	}

	String^ LocalIpv4Address()
	{
		static String^ cachedAddress = nullptr;
		if (cachedAddress != nullptr && cachedAddress->Length() > 0)
		{
			return cachedAddress;
		}

		try
		{
			auto hostNames = NetworkInformation::GetHostNames();
			for (unsigned int i = 0; i < hostNames->Size; ++i)
			{
				HostName^ hostName = hostNames->GetAt(i);
				if (hostName == nullptr ||
					hostName->Type != HostNameType::Ipv4 ||
					hostName->IPInformation == nullptr ||
					hostName->CanonicalName == nullptr ||
					hostName->CanonicalName->Length() == 0)
				{
					continue;
				}

				if (wcscmp(hostName->CanonicalName->Data(), L"127.0.0.1") != 0)
				{
					cachedAddress = hostName->CanonicalName;
					return cachedAddress;
				}
			}
		}
		catch (...)
		{
		}

		return ref new String(L"127.0.0.1");
	}
}

DirectXPage::DirectXPage() :
	m_windowVisible(true),
	m_emulationStarted(false),
	m_emulationPaused(false),
	m_saveStateAvailable(false),
	m_mouseCaptured(false),
	m_mouseCapturePending(false),
	m_mouseCaptureUserReleased(false),
	m_mouseCaptureShortcutDown(false),
	m_tabsEnabled(false),
	m_isPageInitialized(false),
	m_applyingMachineProfile(false),
	m_rfbDisplayActive(false),
	m_selectedPanelIndex(0),
	m_pointerButtons(0),
	m_lastPointerPosition(Point(0.0f, 0.0f)),
	m_selectedDiskPath(nullptr),
	m_selectedDisk2Path(nullptr),
	m_selectedFloppyPath(nullptr),
	m_selectedFloppyBPath(nullptr),
	m_selectedCdromPath(nullptr),
	m_selectedCdrom2Path(nullptr),
	m_selectedSharedFolderPath(nullptr),
	m_selectedUsbImagePath(nullptr),
	m_runtimeMediaTarget(ref new String(L"cdrom")),
	m_bootTarget(ref new String(L"disk, cdrom, floppy")),
		m_selectedBiosPath(nullptr),
		m_selectedVgaBiosPath(nullptr),
		m_rfbStatusTimer(nullptr),
		m_lastBochsrcPreview(),
		m_problemCount(0)
{
	InitializeComponent();

	CoreWindow^ window = Window::Current->CoreWindow;
	window->VisibilityChanged +=
		ref new TypedEventHandler<CoreWindow^, VisibilityChangedEventArgs^>(this, &DirectXPage::OnVisibilityChanged);
	window->Closed +=
		ref new TypedEventHandler<CoreWindow^, CoreWindowEventArgs^>(this, &DirectXPage::OnWindowClosed);
	window->KeyDown +=
		ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(this, &DirectXPage::OnKeyDown);
	window->KeyUp +=
		ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(this, &DirectXPage::OnKeyUp);
	MouseDevice^ mouseDevice = MouseDevice::GetForCurrentView();
	if (mouseDevice != nullptr)
	{
		mouseDevice->MouseMoved +=
			ref new TypedEventHandler<MouseDevice^, MouseEventArgs^>(this, &DirectXPage::OnMouseMoved);
	}

	DisplayInformation^ currentDisplayInformation = DisplayInformation::GetForCurrentView();
	currentDisplayInformation->DpiChanged +=
		ref new TypedEventHandler<DisplayInformation^, Object^>(this, &DirectXPage::OnDpiChanged);
	currentDisplayInformation->OrientationChanged +=
		ref new TypedEventHandler<DisplayInformation^, Object^>(this, &DirectXPage::OnOrientationChanged);
	DisplayInformation::DisplayContentsInvalidated +=
		ref new TypedEventHandler<DisplayInformation^, Object^>(this, &DirectXPage::OnDisplayContentsInvalidated);

	swapChainPanel->CompositionScaleChanged +=
		ref new TypedEventHandler<SwapChainPanel^, Object^>(this, &DirectXPage::OnCompositionScaleChanged);
	swapChainPanel->SizeChanged +=
		ref new SizeChangedEventHandler(this, &DirectXPage::OnSwapChainPanelSizeChanged);
	swapChainPanel->PointerPressed +=
		ref new PointerEventHandler(this, &DirectXPage::OnPointerPressed);
	swapChainPanel->PointerMoved +=
		ref new PointerEventHandler(this, &DirectXPage::OnPointerMoved);
	swapChainPanel->PointerReleased +=
		ref new PointerEventHandler(this, &DirectXPage::OnPointerReleased);
	swapChainPanel->PointerWheelChanged +=
		ref new PointerEventHandler(this, &DirectXPage::OnPointerWheelChanged);

	m_deviceResources = std::make_shared<DX::DeviceResources>();
	m_deviceResources->SetSwapChainPanel(swapChainPanel);

	m_main = std::unique_ptr<UWP_PortMain>(new UWP_PortMain(m_deviceResources));
	m_rfbStatusTimer = ref new DispatcherTimer();
	TimeSpan rfbStatusInterval;
	rfbStatusInterval.Duration = 10000000;
	m_rfbStatusTimer->Interval = rfbStatusInterval;
	m_rfbStatusTimer->Tick += ref new EventHandler<Object^>(this, &DirectXPage::RfbStatusTimer_Tick);
	m_rfbStatusTimer->Start();
	m_isPageInitialized = true;
	topChrome->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
	configurationPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
	UpdateMemoryLabel();
	UpdatePlatformFeatureState();
	UpdateVideoLabel();
	SelectConfigurationPanel(0);
	UpdateBochsrcPreview();
	UpdateCommandState();
	CheckForSavedState();
}

DirectXPage::~DirectXPage()
{
	if (m_rfbStatusTimer != nullptr)
	{
		m_rfbStatusTimer->Stop();
	}
	Shutdown();
}

void DirectXPage::SaveInternalState(IPropertySet^ state)
{
	UNREFERENCED_PARAMETER(state);
	if (m_emulationStarted)
	{
		critical_section::scoped_lock lock(m_main->GetCriticalSection());
		m_main->PauseEmulation();
		m_main->SaveEmulationStateIfPossible(SelectedSaveStateFolderPath());
		m_main->StopRenderLoop();
	}
	m_deviceResources->Trim();
}

void DirectXPage::LoadInternalState(IPropertySet^ state)
{
	UNREFERENCED_PARAMETER(state);
	if (m_emulationStarted)
	{
		m_main->ResumeEmulation();
		if (!m_rfbDisplayActive)
		{
			m_main->StartRenderLoop();
		}
	}
}

void DirectXPage::Shutdown()
{
	ReleaseMouseCapture();
	if (m_main)
	{
		m_main->StopRenderLoop();
		m_main->ShutdownEmulation();
	}
}

void DirectXPage::OnVisibilityChanged(CoreWindow^ sender, VisibilityChangedEventArgs^ args)
{
	UNREFERENCED_PARAMETER(sender);
	m_windowVisible = args->Visible;
	if (!m_emulationStarted)
	{
		return;
	}

	m_main->FocusChanged(m_windowVisible);
	if (m_windowVisible)
	{
		m_main->ResumeEmulation();
		if (!m_rfbDisplayActive)
		{
			m_main->StartRenderLoop();
		}
		m_emulationPaused = false;
	}
	else
	{
		ReleaseMouseCapture();
		m_main->PauseEmulation();
		m_main->StopRenderLoop();
		m_emulationPaused = true;
	}
	UpdateCaptureIndicators();
	UpdateRfbStatusOverlay();
}

void DirectXPage::OnWindowClosed(CoreWindow^ sender, CoreWindowEventArgs^ args)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(args);
	Shutdown();
}

void DirectXPage::OnKeyDown(CoreWindow^ sender, KeyEventArgs^ args)
{
	UNREFERENCED_PARAMETER(sender);
	if (!m_emulationStarted || m_rfbDisplayActive)
	{
		return;
	}
	if (HandleMouseCaptureShortcut(args, true))
	{
		args->Handled = true;
		return;
	}
	if (!args->KeyStatus.WasKeyDown)
	{
		m_main->KeyChanged(NormalizeVirtualKey(args->VirtualKey, args->KeyStatus), true);
	}
	args->Handled = true;
}

void DirectXPage::OnKeyUp(CoreWindow^ sender, KeyEventArgs^ args)
{
	UNREFERENCED_PARAMETER(sender);
	if (!m_emulationStarted || m_rfbDisplayActive)
	{
		return;
	}
	if (HandleMouseCaptureShortcut(args, false))
	{
		args->Handled = true;
		return;
	}
	m_main->KeyChanged(NormalizeVirtualKey(args->VirtualKey, args->KeyStatus), false);
	args->Handled = true;
}

void DirectXPage::OnMouseMoved(MouseDevice^ sender, MouseEventArgs^ args)
{
	UNREFERENCED_PARAMETER(sender);
	if (!m_emulationStarted)
	{
		return;
	}

	UpdateMouseCaptureFromCore();
	if (!m_mouseCaptured)
	{
		return;
	}

	MouseDelta delta = args->MouseDelta;
	if (delta.X == 0 && delta.Y == 0)
	{
		return;
	}

	m_main->PointerMoved(
		static_cast<float>(delta.X),
		static_cast<float>(delta.Y),
		m_pointerButtons,
		false);
}

void DirectXPage::OnDpiChanged(DisplayInformation^ sender, Object^ args)
{
	UNREFERENCED_PARAMETER(args);
	critical_section::scoped_lock lock(m_main->GetCriticalSection());
	if (m_emulationStarted)
	{
		m_main->PauseEmulation();
	}
	m_deviceResources->SetDpi(sender->LogicalDpi);
	m_main->CreateWindowSizeDependentResources();
	if (m_emulationStarted)
	{
		m_main->ResumeEmulation();
	}
}

void DirectXPage::OnOrientationChanged(DisplayInformation^ sender, Object^ args)
{
	UNREFERENCED_PARAMETER(args);
	critical_section::scoped_lock lock(m_main->GetCriticalSection());
	if (m_emulationStarted)
	{
		m_main->PauseEmulation();
	}
	m_deviceResources->SetCurrentOrientation(sender->CurrentOrientation);
	m_main->CreateWindowSizeDependentResources();
	if (m_emulationStarted)
	{
		m_main->ResumeEmulation();
	}
}

void DirectXPage::OnDisplayContentsInvalidated(DisplayInformation^ sender, Object^ args)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(args);
	critical_section::scoped_lock lock(m_main->GetCriticalSection());
	m_deviceResources->ValidateDevice();
}

void DirectXPage::ChooseDiskButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);

	chooseDiskButton->IsEnabled = false;
	startEmulatorButton->IsEnabled = false;
	startupStatusText->Text = "Selecting image...";

	BochsUwpStorage::PickDiskImageToLocalFolderAsync().then([this](task<String^> pickTask)
	{
		chooseDiskButton->IsEnabled = true;
		String^ diskPath = nullptr;
		try
		{
			diskPath = pickTask.get();
		}
		catch (...)
		{
			HandleAsyncFailure("Could not select the HDD image.");
			return;
		}

		if (diskPath == nullptr || diskPath->Length() == 0)
		{
			startupStatusText->Text = "Selection canceled.";
			startEmulatorButton->IsEnabled = false;
			UpdateCommandState();
			return;
		}

		m_selectedDiskPath = diskPath;
		SetBootSequencePrimary(L"disk");
		diskPathText->Text = diskPath;
		chooseDiskButton->Label = ref new String(L"Change HDD");
		UpdateDiskImageModeLabel();
		std::wstring status(L"HDD selected. Mode ");
		status += EffectiveDiskImageMode()->Data();
		status += L"; boot set to disk.";
		startupStatusText->Text = ref new String(status.c_str());
		UpdateCommandState();
	}, task_continuation_context::use_current());
}

void DirectXPage::ResetDiskButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (m_emulationStarted)
	{
		return;
	}

	m_selectedDiskPath = nullptr;
	SetBootSequencePrimary(m_selectedCdromPath != nullptr && m_selectedCdromPath->Length() > 0
		? L"cdrom"
		: (m_selectedFloppyPath != nullptr && m_selectedFloppyPath->Length() > 0 ? L"floppy" : L"disk"));
	diskPathText->Text = "No image selected";
	UpdateDiskImageModeLabel();
	chooseDiskButton->Label = ref new String(L"Select HDD");
	startupStatusText->Text = "HDD removed.";
	UpdateCommandState();
}

void DirectXPage::ChooseDisk2Button_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (m_emulationStarted)
	{
		return;
	}

	chooseDisk2Button->IsEnabled = false;
	startupStatusText->Text = "Selecting second HDD image...";
	BochsUwpStorage::PickDiskImageToLocalFolderAsync().then([this](task<String^> pickTask)
	{
		String^ diskPath = nullptr;
		try
		{
			diskPath = pickTask.get();
		}
		catch (...)
		{
			HandleAsyncFailure("Could not select the second HDD image.");
			return;
		}

		if (diskPath == nullptr || diskPath->Length() == 0)
		{
			startupStatusText->Text = "Second HDD selection canceled.";
			UpdateCommandState();
			return;
		}

		m_selectedDisk2Path = diskPath;
		disk2PathText->Text = diskPath;
		chooseDisk2Button->Label = ref new String(L"Change HDD 2");
		startupStatusText->Text = "Second HDD selected.";
		UpdateCommandState();
	}, task_continuation_context::use_current());
}

void DirectXPage::ResetDisk2Button_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (m_emulationStarted)
	{
		return;
	}

	m_selectedDisk2Path = nullptr;
	disk2PathText->Text = "No image selected";
	chooseDisk2Button->Label = ref new String(L"Select HDD 2");
	startupStatusText->Text = "Second HDD removed.";
	UpdateCommandState();
}

void DirectXPage::ChooseFloppyButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (m_emulationStarted)
	{
		return;
	}

	chooseFloppyButton->IsEnabled = false;
	startEmulatorButton->IsEnabled = false;
	startupStatusText->Text = "Selecting floppy...";

	BochsUwpStorage::PickFloppyImageToLocalFolderAsync().then([this](task<String^> pickTask)
	{
		String^ floppyPath = nullptr;
		try
		{
			floppyPath = pickTask.get();
		}
		catch (...)
		{
			HandleAsyncFailure("Could not select the floppy.");
			return;
		}

		if (floppyPath == nullptr || floppyPath->Length() == 0)
		{
			startupStatusText->Text = "Floppy selection canceled.";
			UpdateCommandState();
			return;
		}

		m_selectedFloppyPath = floppyPath;
		SetBootSequencePrimary(L"floppy");
		floppyPathText->Text = floppyPath;
		chooseFloppyButton->Label = ref new String(L"Change floppy");
		startupStatusText->Text = "Floppy selected. Boot set to floppy.";
		UpdateCommandState();
	}, task_continuation_context::use_current());
}

void DirectXPage::ResetFloppyButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (m_emulationStarted)
	{
		return;
	}

	m_selectedFloppyPath = nullptr;
	SetBootSequencePrimary(m_selectedDiskPath != nullptr && m_selectedDiskPath->Length() > 0
		? L"disk"
		: (m_selectedCdromPath != nullptr && m_selectedCdromPath->Length() > 0 ? L"cdrom" : L"disk"));
	floppyPathText->Text = "No floppy selected";
	chooseFloppyButton->Label = ref new String(L"Select floppy");
	startupStatusText->Text = "Floppy removed.";
	UpdateCommandState();
}

void DirectXPage::ChooseFloppyBButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (m_emulationStarted)
	{
		return;
	}

	chooseFloppyBButton->IsEnabled = false;
	startupStatusText->Text = "Selecting floppy B...";
	BochsUwpStorage::PickFloppyImageToLocalFolderAsync().then([this](task<String^> pickTask)
	{
		String^ floppyPath = nullptr;
		try
		{
			floppyPath = pickTask.get();
		}
		catch (...)
		{
			HandleAsyncFailure("Could not select floppy B.");
			return;
		}

		if (floppyPath == nullptr || floppyPath->Length() == 0)
		{
			startupStatusText->Text = "Floppy B selection canceled.";
			UpdateCommandState();
			return;
		}

		m_selectedFloppyBPath = floppyPath;
		floppyBPathText->Text = floppyPath;
		chooseFloppyBButton->Label = ref new String(L"Change floppy B");
		startupStatusText->Text = "Floppy B selected.";
		UpdateCommandState();
	}, task_continuation_context::use_current());
}

void DirectXPage::ResetFloppyBButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (m_emulationStarted)
	{
		return;
	}

	m_selectedFloppyBPath = nullptr;
	floppyBPathText->Text = "No floppy selected";
	chooseFloppyBButton->Label = ref new String(L"Select floppy B");
	startupStatusText->Text = "Floppy B removed.";
	UpdateCommandState();
}

void DirectXPage::ChooseCdromButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (m_emulationStarted)
	{
		return;
	}

	chooseCdromButton->IsEnabled = false;
	startEmulatorButton->IsEnabled = false;
	startupStatusText->Text = "Selecting ISO...";

	BochsUwpStorage::PickCdromImageToLocalFolderAsync().then([this](task<String^> pickTask)
	{
		String^ cdromPath = nullptr;
		try
		{
			cdromPath = pickTask.get();
		}
		catch (...)
		{
			HandleAsyncFailure("Could not select the ISO.");
			return;
		}

		if (cdromPath == nullptr || cdromPath->Length() == 0)
		{
			startupStatusText->Text = "ISO selection canceled.";
			UpdateCommandState();
			return;
		}

		m_selectedCdromPath = cdromPath;
		SetBootSequencePrimary(L"cdrom");
		cdromPathText->Text = cdromPath;
		chooseCdromButton->Label = ref new String(L"Change ISO");
		startupStatusText->Text = "ISO selected. Boot set to ISO.";
		UpdateCommandState();
	}, task_continuation_context::use_current());
}

void DirectXPage::ResetCdromButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (m_emulationStarted)
	{
		return;
	}

	m_selectedCdromPath = nullptr;
	SetBootSequencePrimary(m_selectedDiskPath != nullptr && m_selectedDiskPath->Length() > 0
		? L"disk"
		: (m_selectedFloppyPath != nullptr && m_selectedFloppyPath->Length() > 0 ? L"floppy" : L"disk"));
	cdromPathText->Text = "No ISO selected";
	chooseCdromButton->Label = ref new String(L"Select ISO");
	startupStatusText->Text = "ISO removed.";
	UpdateCommandState();
}

void DirectXPage::ChooseCdrom2Button_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (m_emulationStarted)
	{
		return;
	}

	chooseCdrom2Button->IsEnabled = false;
	startupStatusText->Text = "Selecting second ISO...";
	BochsUwpStorage::PickCdromImageToLocalFolderAsync().then([this](task<String^> pickTask)
	{
		String^ cdromPath = nullptr;
		try
		{
			cdromPath = pickTask.get();
		}
		catch (...)
		{
			HandleAsyncFailure("Could not select the second ISO.");
			return;
		}

		if (cdromPath == nullptr || cdromPath->Length() == 0)
		{
			startupStatusText->Text = "Second ISO selection canceled.";
			UpdateCommandState();
			return;
		}

		m_selectedCdrom2Path = cdromPath;
		cdrom2PathText->Text = cdromPath;
		chooseCdrom2Button->Label = ref new String(L"Change ISO 2");
		startupStatusText->Text = "Second ISO selected.";
		UpdateCommandState();
	}, task_continuation_context::use_current());
}

void DirectXPage::ResetCdrom2Button_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (m_emulationStarted)
	{
		return;
	}

	m_selectedCdrom2Path = nullptr;
	cdrom2PathText->Text = "No ISO selected";
	chooseCdrom2Button->Label = ref new String(L"Select ISO 2");
	startupStatusText->Text = "Second ISO removed.";
	UpdateCommandState();
}

void DirectXPage::ChooseSharedFolderButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (m_emulationStarted)
	{
		return;
	}

	chooseSharedFolderButton->IsEnabled = false;
	startupStatusText->Text = "Selecting VVFAT shared folder...";

	BochsUwpStorage::PickSharedFolderAsync().then([this](task<String^> pickTask)
	{
		String^ sharedFolderPath = nullptr;
		try
		{
			sharedFolderPath = pickTask.get();
		}
		catch (...)
		{
			HandleAsyncFailure("Could not select the shared folder.");
			return;
		}

		if (sharedFolderPath == nullptr || sharedFolderPath->Length() == 0)
		{
			startupStatusText->Text = "Folder selection canceled.";
			UpdateCommandState();
			return;
		}

		m_selectedSharedFolderPath = sharedFolderPath;
		sharedFolderPathText->Text = sharedFolderPath;
		chooseSharedFolderButton->Label = ref new String(L"Change folder");
		startupStatusText->Text = "Shared folder active through read-only VVFAT.";
		UpdateCommandState();
	}, task_continuation_context::use_current());
}

void DirectXPage::ResetSharedFolderButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (m_emulationStarted)
	{
		return;
	}

	m_selectedSharedFolderPath = nullptr;
	sharedFolderPathText->Text = "No folder selected";
	chooseSharedFolderButton->Label = ref new String(L"Select folder");
	startupStatusText->Text = "Shared folder removed.";
	UpdateCommandState();
}

void DirectXPage::ChooseUsbImageButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (m_emulationStarted)
	{
		return;
	}

	chooseUsbImageButton->IsEnabled = false;
	startupStatusText->Text = "Selecting USB image...";
	String^ usbDevice = SelectedUsbDevice();
	task<String^> pickTask =
		wcscmp(usbDevice->Data(), L"cdrom") == 0
			? BochsUwpStorage::PickCdromImageToLocalFolderAsync()
			: (wcscmp(usbDevice->Data(), L"floppy") == 0
				? BochsUwpStorage::PickFloppyImageToLocalFolderAsync()
				: BochsUwpStorage::PickDiskImageToLocalFolderAsync());
	pickTask.then([this](task<String^> pickTask)
	{
		String^ imagePath = nullptr;
		try
		{
			imagePath = pickTask.get();
		}
		catch (...)
		{
			HandleAsyncFailure("Could not select the USB image.");
			return;
		}

		if (imagePath == nullptr || imagePath->Length() == 0)
		{
			startupStatusText->Text = "USB image selection canceled.";
			UpdateCommandState();
			return;
		}

		m_selectedUsbImagePath = imagePath;
		usbImagePathText->Text = imagePath;
		chooseUsbImageButton->Label = ref new String(L"Change USB image");
		startupStatusText->Text = "USB image selected.";
		UpdateCommandState();
	}, task_continuation_context::use_current());
}

void DirectXPage::ResetUsbImageButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (m_emulationStarted)
	{
		return;
	}

	m_selectedUsbImagePath = nullptr;
	usbImagePathText->Text = "No USB image selected";
	chooseUsbImageButton->Label = ref new String(L"Select USB image");
	startupStatusText->Text = "USB image removed.";
	UpdateCommandState();
}

void DirectXPage::SelectBiosButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (m_emulationStarted)
	{
		return;
	}

	selectBiosButton->IsEnabled = false;
	startupStatusText->Text = "Selecting BIOS...";
	BochsUwpStorage::PickBiosImageToLocalFolderAsync().then([this](task<String^> pickTask)
	{
		String^ biosPath = nullptr;
		try
		{
			biosPath = pickTask.get();
		}
		catch (...)
		{
			HandleAsyncFailure("Could not select the BIOS.");
			return;
		}

		if (biosPath == nullptr || biosPath->Length() == 0)
		{
			startupStatusText->Text = "BIOS selection canceled.";
			UpdateCommandState();
			return;
		}

		m_selectedBiosPath = biosPath;
		biosPathText->Text = biosPath;
		selectBiosButton->Label = ref new String(L"Change BIOS");
		startupStatusText->Text = "Custom BIOS selected.";
		UpdateCommandState();
	}, task_continuation_context::use_current());
}

void DirectXPage::ResetBiosButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (m_emulationStarted)
	{
		return;
	}

	m_selectedBiosPath = nullptr;
	biosPathText->Text = "Built-in BIOS-bochs-latest";
	selectBiosButton->Label = ref new String(L"Select BIOS");
	startupStatusText->Text = "Built-in BIOS restored.";
	UpdateCommandState();
}

void DirectXPage::SelectVgaBiosButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (m_emulationStarted)
	{
		return;
	}

	selectVgaBiosButton->IsEnabled = false;
	startupStatusText->Text = "Selecting VGA BIOS...";
	BochsUwpStorage::PickBiosImageToLocalFolderAsync().then([this](task<String^> pickTask)
	{
		String^ vgaBiosPath = nullptr;
		try
		{
			vgaBiosPath = pickTask.get();
		}
		catch (...)
		{
			HandleAsyncFailure("Could not select the VGA BIOS.");
			return;
		}

		if (vgaBiosPath == nullptr || vgaBiosPath->Length() == 0)
		{
			startupStatusText->Text = "VGA BIOS selection canceled.";
			UpdateCommandState();
			return;
		}

		m_selectedVgaBiosPath = vgaBiosPath;
		vgaBiosPathText->Text = vgaBiosPath;
		selectVgaBiosButton->Label = ref new String(L"Change VGA BIOS");
		startupStatusText->Text = "Custom VGA BIOS selected.";
		UpdateCommandState();
	}, task_continuation_context::use_current());
}

void DirectXPage::ResetVgaBiosButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (m_emulationStarted)
	{
		return;
	}

	m_selectedVgaBiosPath = nullptr;
	vgaBiosPathText->Text = "Built-in VGABIOS-lgpl-latest.bin";
	selectVgaBiosButton->Label = ref new String(L"Select VGA BIOS");
	startupStatusText->Text = "Built-in VGA BIOS restored.";
	UpdateCommandState();
}

void DirectXPage::StartEmulatorButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);

	if ((m_selectedDiskPath == nullptr || m_selectedDiskPath->Length() == 0) &&
		(m_selectedDisk2Path == nullptr || m_selectedDisk2Path->Length() == 0) &&
		(m_selectedFloppyPath == nullptr || m_selectedFloppyPath->Length() == 0) &&
		(m_selectedFloppyBPath == nullptr || m_selectedFloppyBPath->Length() == 0) &&
		(m_selectedCdromPath == nullptr || m_selectedCdromPath->Length() == 0) &&
		(m_selectedCdrom2Path == nullptr || m_selectedCdrom2Path->Length() == 0) &&
		!m_saveStateAvailable)
	{
		startupStatusText->Text = "Choose an HDD, floppy or ISO image before starting.";
		AppendProblemLog("Start blocked: no boot image or saved state was selected.");
		return;
	}

	if (m_saveStateAvailable)
	{
		String^ slotLabel = SelectedSaveStateSlotLabel();
		std::wstring message(L"A saved state exists in ");
		message += slotLabel->Data();
		message += L". Restore this session?";
		MessageDialog^ dialog = ref new MessageDialog(
			ref new String(message.c_str()),
			"Saved state found");
		UICommand^ restoreCommand = ref new UICommand("Restore");
		UICommand^ newCommand = ref new UICommand("Start new");
		dialog->Commands->Append(restoreCommand);
		dialog->Commands->Append(newCommand);
		dialog->DefaultCommandIndex = 0;
		dialog->CancelCommandIndex = 1;

		create_task(dialog->ShowAsync()).then([this, restoreCommand](task<IUICommand^> dialogTask)
		{
			IUICommand^ command = nullptr;
			try
			{
				command = dialogTask.get();
			}
			catch (...)
			{
				HandleAsyncFailure("Could not open the restore choice.");
				return;
			}
			StartPreparedEmulator(command == restoreCommand);
		}, task_continuation_context::use_current());
		return;
	}

	StartPreparedEmulator(false);
}

void DirectXPage::CheckForSavedState()
{
	String^ slotId = SelectedSaveStateSlotId();
	String^ slotLabel = SelectedSaveStateSlotLabel();
	BochsUwpStorage::HasSaveStateAsync(slotId).then([this, slotId, slotLabel](task<bool> stateTask)
	{
		bool hasSaveState = false;
		try
		{
			hasSaveState = stateTask.get();
		}
		catch (...)
		{
			m_saveStateAvailable = false;
			UpdateCommandState();
			return;
		}

		String^ currentSlotId = SelectedSaveStateSlotId();
		if (currentSlotId == nullptr || slotId == nullptr || wcscmp(currentSlotId->Data(), slotId->Data()) != 0)
		{
			return;
		}

		m_saveStateAvailable = hasSaveState;
		if (saveStateSlotStatusText != nullptr)
		{
			std::wstring text(slotLabel->Data());
			text += hasSaveState ? L": saved state available" : L": empty";
			saveStateSlotStatusText->Text = ref new String(text.c_str());
		}
		if (hasSaveState && !m_emulationStarted)
		{
			std::wstring text(L"Saved state found in ");
			text += slotLabel->Data();
			text += L". Click Start to restore or start new.";
			startupStatusText->Text = ref new String(text.c_str());
		}
		UpdateCommandState();
	}, task_continuation_context::use_current());
}

void DirectXPage::StartPreparedEmulator(bool restoreSavedState)
{
	chooseDiskButton->IsEnabled = false;
	startEmulatorButton->IsEnabled = false;
	m_rfbDisplayActive = wcscmp(SelectedDisplayLibrary()->Data(), L"rfb") == 0;
	UpdateRfbStatusOverlay();

	if (restoreSavedState)
	{
		std::wstring status(L"Restoring ");
		status += SelectedSaveStateSlotLabel()->Data();
		status += L"...";
		startupStatusText->Text = ref new String(status.c_str());
		Platform::String^ restorePath = SelectedSaveStateFolderPath();
		m_main->StartEmulation(nullptr, restorePath);
		if (!m_rfbDisplayActive)
		{
			m_main->StartRenderLoop();
		}
		m_emulationStarted = true;
		m_emulationPaused = false;
		m_tabsEnabled = false;
		readyOverlay->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
		UpdateRfbStatusOverlay();
		topChrome->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
		configurationPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
		FocusEmulatorSurface();
		UpdateCommandState();
		MonitorStartupAfterDelay();
		return;
	}

	if ((m_selectedDiskPath == nullptr || m_selectedDiskPath->Length() == 0) &&
		(m_selectedDisk2Path == nullptr || m_selectedDisk2Path->Length() == 0) &&
		(m_selectedFloppyPath == nullptr || m_selectedFloppyPath->Length() == 0) &&
		(m_selectedFloppyBPath == nullptr || m_selectedFloppyBPath->Length() == 0) &&
		(m_selectedCdromPath == nullptr || m_selectedCdromPath->Length() == 0) &&
		(m_selectedCdrom2Path == nullptr || m_selectedCdrom2Path->Length() == 0))
	{
		chooseDiskButton->IsEnabled = true;
		startEmulatorButton->IsEnabled = m_saveStateAvailable;
		startupStatusText->Text = "Choose an HDD, floppy or ISO image to start a new session.";
		UpdateCommandState();
		return;
	}

	startupStatusText->Text = "Preparing configuration...";
	UpdateBochsrcPreview();
	m_bootTarget = SelectedBootSequence();
	BochsUwpStorage::CreateConfigAsync(
		m_selectedDiskPath,
		m_selectedDisk2Path,
		m_selectedFloppyPath,
		m_selectedFloppyBPath,
		m_selectedCdromPath,
		m_selectedCdrom2Path,
		m_selectedSharedFolderPath,
		m_bootTarget,
		SelectedMemoryMb(),
		SelectedCpuModel(),
		m_selectedBiosPath,
		m_selectedVgaBiosPath,
		SelectedSoundEnabled(),
		SelectedNetworkAdapter(),
		SelectedDiskImageMode(),
		SelectedDisk2ImageMode(),
		SelectedUsbController(),
		SelectedUsbDevice(),
		m_selectedUsbImagePath,
		SelectedSerialMode(),
		SelectedParallelEnabled(),
		SelectedVideoExtension(),
		SelectedVideoUpdateFreq(),
		SelectedVideoRealtime(),
		SelectedVbeMemoryMb(),
		SelectedDisplayLibrary(),
		SelectedRfbTimeout(),
		SelectedNetworkSocketPort(),
		SelectedPciChipset(),
		SelectedAcpiEnabled(),
		SelectedHpetEnabled(),
		SelectedIoApicEnabled()).then([this](task<String^> configTask)
	{
		String^ configPath = nullptr;
		try
		{
			configPath = configTask.get();
		}
		catch (Platform::Exception^ ex)
		{
			HandleAsyncFailure(FormatPlatformFailure(L"Could not prepare the configuration", ex));
			return;
		}
		catch (const std::exception& ex)
		{
			HandleAsyncFailure(FormatStdFailure(L"Could not prepare the configuration", ex));
			return;
		}
		catch (...)
		{
			HandleAsyncFailure("Could not prepare the configuration.");
			return;
		}
		if (configPath == nullptr || configPath->Length() == 0)
		{
			HandleAsyncFailure("The generated configuration is empty.");
			return;
		}
		startupStatusText->Text = "Starting emulator...";
		m_main->StartEmulation(configPath);
		if (!m_rfbDisplayActive)
		{
			m_main->StartRenderLoop();
		}
		m_emulationStarted = true;
		m_emulationPaused = false;
		m_tabsEnabled = false;
		readyOverlay->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
		UpdateRfbStatusOverlay();
		topChrome->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
		configurationPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
		FocusEmulatorSurface();
		UpdateCommandState();
		MonitorStartupAfterDelay();
	}, task_continuation_context::use_current());
}

void DirectXPage::MonitorStartupAfterDelay()
{
	create_task([]()
	{
		Sleep(1500);
	}).then([this]()
	{
		if (!m_emulationStarted || m_main == nullptr)
		{
			return;
		}

		if (m_main->HasEmulationError())
		{
			String^ message = m_main->LastEmulationError();
			HandleStartupFailure(message != nullptr && message->Length() > 0
				? message
				: ref new String(L"Failed to start the emulator."));
			return;
		}

		if (m_main->HasEmulationExited() && !m_main->IsEmulationRunning())
		{
			HandleStartupFailure(ref new String(L"The emulator exited before startup completed."));
			return;
		}

		startupStatusText->Text = m_rfbDisplayActive
			? ref new String(L"VNC/RFB server running.")
			: ref new String(L"Emulator running.");
		UpdateRfbStatusOverlay();
		UpdateCommandState();
	}, task_continuation_context::use_current());
}

void DirectXPage::FocusEmulatorSurface()
{
	if (emulatorFocusTarget == nullptr)
	{
		return;
	}

	emulatorFocusTarget->Focus(Windows::UI::Xaml::FocusState::Programmatic);
}

void DirectXPage::HandleStartupFailure(String^ message)
{
	String^ finalMessage = message != nullptr && message->Length() > 0
		? message
		: ref new String(L"Failed to start the emulator.");
	m_main->StopRenderLoop();
	m_main->ShutdownEmulation();
	m_emulationStarted = false;
	m_emulationPaused = false;
	m_rfbDisplayActive = false;
	readyOverlay->Visibility = Windows::UI::Xaml::Visibility::Visible;
	UpdateRfbStatusOverlay();
	configurationPanel->Visibility = m_tabsEnabled
		? Windows::UI::Xaml::Visibility::Visible
		: Windows::UI::Xaml::Visibility::Collapsed;
	startupStatusText->Text = finalMessage;
	AppendProblemLog(finalMessage);
	UpdateCommandState();
}

void DirectXPage::HandleAsyncFailure(String^ message)
{
	startupStatusText->Text = message;
	AppendProblemLog(message);
	UpdateCommandState();
}

void DirectXPage::RfbStatusTimer_Tick(Object^ sender, Object^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	UpdateRfbStatusOverlay();
}

void DirectXPage::UpdateRfbStatusOverlay()
{
	if (rfbStatusOverlay == nullptr || rfbStatusText == nullptr ||
		rfbEndpointText == nullptr || rfbPortText == nullptr ||
		rfbConnectionText == nullptr)
	{
		return;
	}

	bool showOverlay = m_emulationStarted && m_rfbDisplayActive;
	rfbStatusOverlay->Visibility = showOverlay
		? Windows::UI::Xaml::Visibility::Visible
		: Windows::UI::Xaml::Visibility::Collapsed;
	if (!showOverlay)
	{
		return;
	}

	int port = bx_uwp_rfb_get_port();
	bool serverRunning = bx_uwp_rfb_is_server_running() != 0;
	bool clientConnected = bx_uwp_rfb_is_client_connected() != 0;
	bool coreError = m_main != nullptr && m_main->HasEmulationError();
	bool coreExited = m_main != nullptr && m_main->HasEmulationExited() && !m_main->IsEmulationRunning();

	std::wstring status(L"Status: ");
	if (coreError || coreExited)
	{
		status += L"failed";
	}
	else if (m_emulationPaused)
	{
		status += L"paused";
	}
	else if (!serverRunning || port <= 0)
	{
		status += L"starting RFB server";
	}
	else
	{
		status += L"running";
	}
	SetTextIfChanged(rfbStatusText, status);

	std::wstring ipText(L"IP: ");
	ipText += LocalIpv4Address()->Data();
	SetTextIfChanged(rfbEndpointText, ipText);

	std::wstring portText(L"Port: ");
	portText += std::to_wstring(port > 0 ? port : 5900);
	if (port <= 0)
	{
		portText += L" (probing 5900-5949)";
	}
	SetTextIfChanged(rfbPortText, portText);

	std::wstring connectionText(L"Connection: ");
	if (coreError || coreExited)
	{
		connectionText += L"emulator stopped before RFB was ready";
	}
	else if (clientConnected)
	{
		connectionText += L"client connected";
	}
	else if (serverRunning && port > 0)
	{
		connectionText += L"waiting for VNC client";
	}
	else
	{
		connectionText += L"not ready";
	}
	SetTextIfChanged(rfbConnectionText, connectionText);
}

void DirectXPage::ToggleTabsButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	m_tabsEnabled = !m_tabsEnabled;
	topChrome->Visibility = m_tabsEnabled
		? Windows::UI::Xaml::Visibility::Visible
		: Windows::UI::Xaml::Visibility::Collapsed;
	if (m_tabsEnabled)
	{
		SelectConfigurationPanel(m_selectedPanelIndex);
	}
	else
	{
		configurationPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
	}
	UpdateCommandState();
}

void DirectXPage::MemorySlider_ValueChanged(Object^ sender, RangeBaseValueChangedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	MarkMachineProfileCustom();
	UpdateMemoryLabel();
}

void DirectXPage::BootDeviceComboBox_SelectionChanged(Object^ sender, SelectionChangedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (m_emulationStarted || bootDeviceComboBox == nullptr)
	{
		return;
	}

	m_bootTarget = SelectedBootSequence();
	startupStatusText->Text = "Boot sequence selected.";
	MarkMachineProfileCustom();
	UpdateBochsrcPreview();
}

void DirectXPage::DiskImageModeComboBox_SelectionChanged(Object^ sender, SelectionChangedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (!m_isPageInitialized)
	{
		return;
	}

	MarkMachineProfileCustom();
	UpdateDiskImageModeLabel();
	UpdateCommandState();
}

void DirectXPage::FeatureSelection_Changed(Object^ sender, SelectionChangedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (!m_isPageInitialized)
	{
		return;
	}

	MarkMachineProfileCustom();
	UpdatePlatformFeatureState();
	UpdateVideoLabel();
	UpdateCommandState();
}

void DirectXPage::FeatureToggle_Changed(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (!m_isPageInitialized)
	{
		return;
	}

	MarkMachineProfileCustom();
	UpdatePlatformFeatureState();
	UpdateVideoLabel();
	UpdateCommandState();
}

void DirectXPage::NetworkSocketPortTextBox_TextChanged(Object^ sender, TextChangedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (!m_isPageInitialized)
	{
		return;
	}

	MarkMachineProfileCustom();
	UpdateCommandState();
}

void DirectXPage::PauseButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (!m_emulationStarted)
	{
		return;
	}

	m_main->PauseEmulation();
	m_emulationPaused = true;
	startupStatusText->Text = "Emulator paused.";
	UpdateRfbStatusOverlay();
	UpdateCommandState();
}

void DirectXPage::ResumeButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (!m_emulationStarted)
	{
		return;
	}

	m_main->ResumeEmulation();
	if (!m_rfbDisplayActive)
	{
		m_main->StartRenderLoop();
	}
	m_emulationPaused = false;
	startupStatusText->Text = m_rfbDisplayActive
		? ref new String(L"VNC/RFB server running.")
		: ref new String(L"Emulator running.");
	FocusEmulatorSurface();
	UpdateRfbStatusOverlay();
	UpdateCommandState();
}

void DirectXPage::ShutdownButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (!m_emulationStarted)
	{
		return;
	}

	Shutdown();
	m_emulationStarted = false;
	m_emulationPaused = false;
	m_rfbDisplayActive = false;
	readyOverlay->Visibility = Windows::UI::Xaml::Visibility::Visible;
	UpdateRfbStatusOverlay();
	configurationPanel->Visibility = m_tabsEnabled
		? Windows::UI::Xaml::Visibility::Visible
		: Windows::UI::Xaml::Visibility::Collapsed;
	startupStatusText->Text = "Emulator shut down.";
	UpdateCommandState();
}

void DirectXPage::SaveButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (!m_emulationStarted)
	{
		return;
	}

	String^ slotId = SelectedSaveStateSlotId();
	String^ slotLabel = SelectedSaveStateSlotLabel();
	BochsUwpStorage::HasSaveStateAsync(slotId).then([this, slotLabel](task<bool> stateTask)
	{
		bool hasState = false;
		try
		{
			hasState = stateTask.get();
		}
		catch (...)
		{
			HandleAsyncFailure("Could not check the save-state slot.");
			return;
		}

		if (!hasState)
		{
			SaveSelectedState();
			return;
		}

		std::wstring message(L"The ");
		message += slotLabel->Data();
		message += L" already contains a saved state. Overwrite it?";
		MessageDialog^ dialog = ref new MessageDialog(ref new String(message.c_str()), "Overwrite save-state");
		UICommand^ overwriteCommand = ref new UICommand("Overwrite");
		UICommand^ cancelCommand = ref new UICommand("Cancel");
		dialog->Commands->Append(overwriteCommand);
		dialog->Commands->Append(cancelCommand);
		dialog->DefaultCommandIndex = 0;
		dialog->CancelCommandIndex = 1;
		create_task(dialog->ShowAsync()).then([this, overwriteCommand](task<IUICommand^> dialogTask)
		{
			IUICommand^ command = nullptr;
			try
			{
				command = dialogTask.get();
			}
			catch (...)
			{
				HandleAsyncFailure("Could not open the overwrite confirmation.");
				return;
			}
			if (command == overwriteCommand)
			{
				SaveSelectedState();
			}
			else
			{
				startupStatusText->Text = "Save-state canceled.";
			}
		}, task_continuation_context::use_current());
	}, task_continuation_context::use_current());
}

void DirectXPage::RestoreButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (!m_saveStateAvailable)
	{
		startupStatusText->Text = "No saved state found.";
		AppendProblemLog("Restore blocked: no saved state was found.");
		return;
	}

	if (m_emulationStarted)
	{
		MessageDialog^ dialog = ref new MessageDialog(
			"Restoring a saved state will shut down the current session before loading the selected slot.",
			"Restore save-state");
		UICommand^ restoreCommand = ref new UICommand("Restore");
		UICommand^ cancelCommand = ref new UICommand("Cancel");
		dialog->Commands->Append(restoreCommand);
		dialog->Commands->Append(cancelCommand);
		dialog->DefaultCommandIndex = 0;
		dialog->CancelCommandIndex = 1;
		create_task(dialog->ShowAsync()).then([this, restoreCommand](task<IUICommand^> dialogTask)
		{
			IUICommand^ command = nullptr;
			try
			{
				command = dialogTask.get();
			}
			catch (...)
			{
				HandleAsyncFailure("Could not open the restore confirmation.");
				return;
			}
			if (command == restoreCommand)
			{
				RestoreSelectedState();
			}
		}, task_continuation_context::use_current());
		return;
	}

	RestoreSelectedState();
}

void DirectXPage::InsertRuntimeMedia(const wchar_t *target)
{
	if (!m_emulationStarted)
	{
		startupStatusText->Text = "Runtime media changes require a running emulator.";
		return;
	}

	String^ targetString = ref new String(target != nullptr ? target : L"cdrom");
	m_runtimeMediaTarget = targetString;
	task<String^> pickTask =
		wcscmp(targetString->Data(), L"cdrom") == 0
			? BochsUwpStorage::PickCdromImageToLocalFolderAsync()
			: BochsUwpStorage::PickFloppyImageToLocalFolderAsync();

	startupStatusText->Text = "Selecting runtime media...";
	pickTask.then([this, targetString](task<String^> mediaTask)
	{
		String^ mediaPath = nullptr;
		try
		{
			mediaPath = mediaTask.get();
		}
		catch (...)
		{
			HandleAsyncFailure("Could not select runtime media.");
			return;
		}

		if (mediaPath == nullptr || mediaPath->Length() == 0)
		{
			startupStatusText->Text = "Runtime media selection canceled.";
			UpdateCommandState();
			return;
		}

		BochsUwpBridge::SetRuntimeMedia(targetString, mediaPath, true);
		startupStatusText->Text = "Runtime media inserted.";
		UpdateCommandState();
	}, task_continuation_context::use_current());
}

void DirectXPage::EjectRuntimeMedia()
{
	if (!m_emulationStarted)
	{
		startupStatusText->Text = "Runtime media changes require a running emulator.";
		return;
	}

	BochsUwpBridge::SetRuntimeMedia(m_runtimeMediaTarget, nullptr, false);
	startupStatusText->Text = "Runtime media ejected.";
	UpdateCommandState();
}

void DirectXPage::RuntimeFloppyAButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	InsertRuntimeMedia(L"floppya");
}

void DirectXPage::RuntimeFloppyBButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	InsertRuntimeMedia(L"floppyb");
}

void DirectXPage::RuntimeCdromButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	InsertRuntimeMedia(L"cdrom");
}

void DirectXPage::RuntimeEjectButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	EjectRuntimeMedia();
}

void DirectXPage::MachineProfileComboBox_SelectionChanged(Object^ sender, SelectionChangedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (!m_isPageInitialized || m_emulationStarted)
	{
		return;
	}

	String^ profile = ComboBoxTagString(machineProfileComboBox, L"balanced");
	if (wcscmp(profile->Data(), L"custom") != 0)
	{
		ApplyMachineProfile(profile);
	}
}

void DirectXPage::SaveStateSlotComboBox_SelectionChanged(Object^ sender, SelectionChangedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	CheckForSavedState();
}

void DirectXPage::VideoSlider_ValueChanged(Object^ sender, RangeBaseValueChangedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	if (!m_isPageInitialized)
	{
		return;
	}

	MarkMachineProfileCustom();
	UpdateVideoLabel();
	UpdateCommandState();
}

void DirectXPage::ClearProblemsButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	m_problemLog.clear();
	m_problemCount = 0;
	UpdateProblemLogView();
	startupStatusText->Text = "Diagnostics cleared.";
}

void DirectXPage::SaveSelectedState()
{
	String^ slotLabel = SelectedSaveStateSlotLabel();
	std::wstring status(L"Saving state to ");
	status += slotLabel->Data();
	status += L"...";
	startupStatusText->Text = ref new String(status.c_str());

	bool saved = m_main->SaveEmulationStateIfPossible(SelectedSaveStateFolderPath());
	m_saveStateAvailable = saved || m_saveStateAvailable;
	m_emulationPaused = true;
	if (saved)
	{
		std::wstring done(L"State saved to ");
		done += slotLabel->Data();
		done += L".";
		startupStatusText->Text = ref new String(done.c_str());
	}
	else
	{
		startupStatusText->Text = "Could not save the state right now.";
		AppendProblemLog("Failed to save state: the core did not confirm the checkpoint.");
	}
	UpdateSaveStateSlotStatus();
	UpdateCommandState();
}

void DirectXPage::RestoreSelectedState()
{
	if (m_emulationStarted)
	{
		Shutdown();
		m_emulationStarted = false;
		m_emulationPaused = false;
	}
	StartPreparedEmulator(true);
}

void DirectXPage::BootTabButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	SelectConfigurationPanel(0);
}

void DirectXPage::SettingsTabButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	SelectConfigurationPanel(1);
}

void DirectXPage::AboutTabButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	SelectConfigurationPanel(4);
}

void DirectXPage::ProblemsTabButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	UpdateProblemLogView();
	SelectConfigurationPanel(3);
}

void DirectXPage::BochsrcTabButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(e);
	UpdateBochsrcPreview();
	SelectConfigurationPanel(2);
}

void DirectXPage::SelectConfigurationPanel(int panelIndex)
{
	m_selectedPanelIndex = panelIndex;
	if (!m_tabsEnabled)
	{
		return;
	}

	configurationPanel->Visibility = Windows::UI::Xaml::Visibility::Visible;
	bootPanel->Visibility = panelIndex == 0 ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
	settingsPanel->Visibility = panelIndex == 1 ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
	bochsrcPanel->Visibility = panelIndex == 2 ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
	problemsPanel->Visibility = panelIndex == 3 ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
	aboutPanel->Visibility = panelIndex == 4 ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;

	bootTabButton->Foreground = panelIndex == 0
		? ref new SolidColorBrush(Windows::UI::Colors::White)
		: ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 156, 163, 175));
	settingsTabButton->Foreground = panelIndex == 1
		? ref new SolidColorBrush(Windows::UI::Colors::White)
		: ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 156, 163, 175));
	bochsrcTabButton->Foreground = panelIndex == 2
		? ref new SolidColorBrush(Windows::UI::Colors::White)
		: ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 156, 163, 175));
	problemsTabButton->Foreground = panelIndex == 3
		? ref new SolidColorBrush(Windows::UI::Colors::White)
		: ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 156, 163, 175));
	aboutTabButton->Foreground = panelIndex == 4
		? ref new SolidColorBrush(Windows::UI::Colors::White)
		: ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 156, 163, 175));
}

void DirectXPage::AppendProblemLog(String^ message)
{
	if (message == nullptr || message->Length() == 0)
	{
		return;
	}

	m_problemCount++;
	m_problemLog += L"- ";
	m_problemLog += message->Data();
	m_problemLog += L"\n";
	UpdateProblemLogView();
}

void DirectXPage::SetRichTextBlockText(RichTextBlock^ block, String^ text)
{
	if (block == nullptr)
	{
		return;
	}

	block->Blocks->Clear();
	Paragraph^ paragraph = ref new Paragraph();
	Run^ run = ref new Run();
	run->Text = text != nullptr && text->Length() > 0 ? text : ref new String(L" ");
	paragraph->Inlines->Append(run);
	block->Blocks->Append(paragraph);
}

void DirectXPage::UpdateProblemLogView()
{
	if (problemCountText != nullptr)
	{
		std::wstring count = std::to_wstring(m_problemCount);
		count += m_problemCount == 1 ? L" event" : L" events";
		problemCountText->Text = ref new String(count.c_str());
	}
	UpdateDiagnosticSummary();

	if (m_problemLog.empty())
	{
		SetRichTextBlockText(problemLogRichTextBlock, "No problems recorded in this session.");
		return;
	}

	SetRichTextBlockText(problemLogRichTextBlock, ref new String(m_problemLog.c_str()));
}

void DirectXPage::UpdateDiagnosticSummary()
{
	if (diagnosticSummaryText == nullptr)
	{
		return;
	}

	bool hasDisk = m_selectedDiskPath != nullptr && m_selectedDiskPath->Length() > 0;
	bool hasDisk2 = m_selectedDisk2Path != nullptr && m_selectedDisk2Path->Length() > 0;
	bool hasFloppy = m_selectedFloppyPath != nullptr && m_selectedFloppyPath->Length() > 0;
	bool hasFloppyB = m_selectedFloppyBPath != nullptr && m_selectedFloppyBPath->Length() > 0;
	bool hasCdrom = m_selectedCdromPath != nullptr && m_selectedCdromPath->Length() > 0;
	bool hasCdrom2 = m_selectedCdrom2Path != nullptr && m_selectedCdrom2Path->Length() > 0;
	bool hasSharedFolder = m_selectedSharedFolderPath != nullptr && m_selectedSharedFolderPath->Length() > 0;
	bool hasUsbImage = m_selectedUsbImagePath != nullptr && m_selectedUsbImagePath->Length() > 0;
	std::wstring summary(L"State: ");
	if (m_emulationStarted)
	{
		summary += m_emulationPaused ? L"paused" : L"running";
	}
	else
	{
		summary += L"ready";
	}
	summary += L"\nBoot: ";
	summary += SelectedBootSequence()->Data();
	summary += L"\nMedia: HDD ";
	summary += hasDisk ? L"selected" : L"empty";
	if (hasDisk)
	{
		summary += L" (";
		summary += EffectiveDiskImageMode()->Data();
		summary += L")";
	}
	summary += L", HDD2 ";
	summary += hasDisk2 ? L"selected" : L"empty";
	if (hasDisk2)
	{
		summary += L" (";
		summary += EffectiveDisk2ImageMode()->Data();
		summary += L")";
	}
	summary += L", Floppy ";
	summary += hasFloppy ? L"selected" : L"empty";
	summary += L", Floppy B ";
	summary += hasFloppyB ? L"selected" : L"empty";
	summary += L", ISO ";
	summary += hasCdrom ? L"selected" : L"empty";
	summary += L", ISO2 ";
	summary += hasCdrom2 ? L"selected" : L"empty";
	summary += L", Shared folder ";
	summary += hasSharedFolder ? L"selected" : L"empty";
	summary += L"\nSave-state: ";
	summary += SelectedSaveStateSlotLabel()->Data();
	summary += m_saveStateAvailable ? L" with saved state" : L" empty";
	summary += L"\nFeatures: sound ";
	summary += SelectedSoundEnabled() ? L"on" : L"off";
	summary += L", network ";
	summary += SelectedNetworkAdapter()->Data();
	if (wcsstr(SelectedNetworkAdapter()->Data(), L"socket") != nullptr)
	{
		summary += L":";
		summary += std::to_wstring(SelectedNetworkSocketPort());
	}
	summary += L", chipset ";
	summary += SelectedPciChipset()->Data();
	summary += L", ACPI ";
	summary += SelectedAcpiEnabled() ? L"on" : L"off";
	summary += L", HPET ";
	summary += SelectedHpetEnabled() ? L"on" : L"off";
	summary += L", IOAPIC ";
	summary += SelectedIoApicEnabled() ? L"on" : L"off";
	summary += L", USB ";
	summary += SelectedUsbController()->Data();
	summary += L"/";
	summary += SelectedUsbDevice()->Data();
	if (hasUsbImage)
	{
		summary += L" image";
	}
	summary += L", COM1 ";
	summary += SelectedSerialMode()->Data();
	summary += L", LPT1 ";
	summary += SelectedParallelEnabled() ? L"file" : L"off";
	summary += L"\nDisplay: ";
	summary += SelectedDisplayLibrary()->Data();
	if (wcscmp(SelectedDisplayLibrary()->Data(), L"rfb") == 0)
	{
		summary += L" timeout ";
		summary += std::to_wstring(SelectedRfbTimeout());
		summary += L" s";
	}
	summary += L"\nVideo: ";
	summary += SelectedVideoExtension()->Data();
	summary += L", ";
	summary += std::to_wstring(SelectedVideoUpdateFreq());
	summary += L" Hz";
	if (wcscmp(SelectedVideoExtension()->Data(), L"vbe") == 0)
	{
		summary += L", VBE ";
		summary += std::to_wstring(SelectedVbeMemoryMb());
		summary += L" MB";
	}
	summary += L"\nMemory: guest ";
	summary += std::to_wstring(SelectedMemoryMb());
	summary += L" MB, host ";
	summary += std::to_wstring(BochsUwpStorage::EffectiveHostMemoryMb(SelectedMemoryMb()));
	summary += L" MB";
	diagnosticSummaryText->Text = ref new String(summary.c_str());
}

void DirectXPage::UpdateSaveStateSlotStatus()
{
	CheckForSavedState();
}

void DirectXPage::UpdateBochsrcPreview()
{
	if (bochsrcRichTextBlock == nullptr)
	{
		return;
	}

	m_bootTarget = SelectedBootSequence();
	String^ configText = BochsUwpStorage::CreateConfigText(
		m_selectedDiskPath,
		m_selectedDisk2Path,
		m_selectedFloppyPath,
		m_selectedFloppyBPath,
		m_selectedCdromPath,
		m_selectedCdrom2Path,
		m_selectedSharedFolderPath,
		m_bootTarget,
		SelectedMemoryMb(),
		SelectedCpuModel(),
		m_selectedBiosPath,
		m_selectedVgaBiosPath,
		SelectedSoundEnabled(),
		SelectedNetworkAdapter(),
		SelectedDiskImageMode(),
		SelectedDisk2ImageMode(),
		SelectedUsbController(),
		SelectedUsbDevice(),
		m_selectedUsbImagePath,
		SelectedSerialMode(),
		SelectedParallelEnabled(),
		SelectedVideoExtension(),
		SelectedVideoUpdateFreq(),
		SelectedVideoRealtime(),
		SelectedVbeMemoryMb(),
		SelectedDisplayLibrary(),
		SelectedRfbTimeout(),
		SelectedNetworkSocketPort(),
		SelectedPciChipset(),
		SelectedAcpiEnabled(),
		SelectedHpetEnabled(),
		SelectedIoApicEnabled());
	if (configText != nullptr && m_lastBochsrcPreview == configText->Data())
	{
		return;
	}
	m_lastBochsrcPreview = configText != nullptr ? configText->Data() : L"";
	SetRichTextBlockText(bochsrcRichTextBlock, configText);
}

void DirectXPage::UpdateDiskImageModeLabel()
{
	if (diskImageModeText == nullptr)
	{
		return;
	}

	std::wstring text(L"Effective mode: ");
	text += EffectiveDiskImageMode()->Data();
	if (m_selectedDiskPath != nullptr && m_selectedDiskPath->Length() > 0)
	{
		String^ selected = SelectedDiskImageMode();
		if (selected != nullptr && selected->Length() > 0 && wcscmp(selected->Data(), L"auto") != 0)
		{
			text += L" (manual)";
		}
	}
	diskImageModeText->Text = ref new String(text.c_str());

	if (disk2ImageModeText == nullptr)
	{
		return;
	}

	std::wstring disk2Text(L"Effective mode: ");
	disk2Text += EffectiveDisk2ImageMode()->Data();
	if (m_selectedDisk2Path != nullptr && m_selectedDisk2Path->Length() > 0)
	{
		String^ selected = SelectedDisk2ImageMode();
		if (selected != nullptr && selected->Length() > 0 && wcscmp(selected->Data(), L"auto") != 0)
		{
			disk2Text += L" (manual)";
		}
	}
	disk2ImageModeText->Text = ref new String(disk2Text.c_str());
}

int DirectXPage::SelectedMemoryMb()
{
	if (memorySlider == nullptr)
	{
		return BochsUwpStorage::NormalizeGuestMemoryMb(512);
	}

	return BochsUwpStorage::NormalizeGuestMemoryMb(static_cast<int>(memorySlider->Value + 0.5));
}

String^ DirectXPage::SelectedCpuModel()
{
	if (cpuModelComboBox == nullptr)
	{
		return ref new String(L"corei7_haswell_4770");
	}

	ComboBoxItem^ item = dynamic_cast<ComboBoxItem^>(cpuModelComboBox->SelectedItem);
	if (item == nullptr)
	{
		return ref new String(L"corei7_haswell_4770");
	}

	String^ model = dynamic_cast<String^>(item->Tag);
	return model != nullptr && model->Length() > 0
		? model
		: ref new String(L"corei7_haswell_4770");
}

String^ DirectXPage::SelectedDiskImageMode()
{
	return ComboBoxTagString(diskImageModeComboBox, L"auto");
}

String^ DirectXPage::SelectedDisk2ImageMode()
{
	return ComboBoxTagString(disk2ImageModeComboBox, L"auto");
}

String^ DirectXPage::EffectiveDiskImageMode()
{
	if (m_selectedDiskPath == nullptr || m_selectedDiskPath->Length() == 0)
	{
		String^ selected = SelectedDiskImageMode();
		return selected != nullptr && selected->Length() > 0 && wcscmp(selected->Data(), L"auto") != 0
			? selected
			: ref new String(L"flat");
	}

	if (wcsncmp(m_selectedDiskPath->Data(), L"uwp://", 6) == 0)
	{
		return BochsUwpStorage::DetectDiskImageMode(m_selectedDiskPath);
	}

	String^ selectedMode = SelectedDiskImageMode();
	if (selectedMode != nullptr && selectedMode->Length() > 0 && wcscmp(selectedMode->Data(), L"auto") != 0)
	{
		return selectedMode;
	}
	return BochsUwpStorage::DetectDiskImageMode(m_selectedDiskPath);
}

String^ DirectXPage::EffectiveDisk2ImageMode()
{
	if (m_selectedDisk2Path == nullptr || m_selectedDisk2Path->Length() == 0)
	{
		String^ selected = SelectedDisk2ImageMode();
		return selected != nullptr && selected->Length() > 0 && wcscmp(selected->Data(), L"auto") != 0
			? selected
			: ref new String(L"flat");
	}

	if (wcsncmp(m_selectedDisk2Path->Data(), L"uwp://", 6) == 0)
	{
		return BochsUwpStorage::DetectDiskImageMode(m_selectedDisk2Path);
	}

	String^ selectedMode = SelectedDisk2ImageMode();
	if (selectedMode != nullptr && selectedMode->Length() > 0 && wcscmp(selectedMode->Data(), L"auto") != 0)
	{
		return selectedMode;
	}
	return BochsUwpStorage::DetectDiskImageMode(m_selectedDisk2Path);
}

String^ DirectXPage::SelectedBootSequence()
{
	std::vector<std::wstring> tags;
	AddBootTag(tags, BootTagFromComboBox(bootDeviceComboBox));
	AddBootTag(tags, BootTagFromComboBox(bootDevice2ComboBox));
	AddBootTag(tags, BootTagFromComboBox(bootDevice3ComboBox));

	if (tags.empty())
	{
		return m_bootTarget != nullptr && m_bootTarget->Length() > 0
			? m_bootTarget
			: ref new String(L"disk");
	}

	std::wstring sequence;
	for (size_t i = 0; i < tags.size(); ++i)
	{
		if (i > 0)
		{
			sequence += L", ";
		}
		sequence += tags[i];
	}
	return ref new String(sequence.c_str());
}

String^ DirectXPage::SelectedSaveStateSlotId()
{
	if (saveStateSlotComboBox == nullptr)
	{
		return ref new String(L"1");
	}

	ComboBoxItem^ item = dynamic_cast<ComboBoxItem^>(saveStateSlotComboBox->SelectedItem);
	if (item == nullptr)
	{
		return ref new String(L"1");
	}

	String^ slot = dynamic_cast<String^>(item->Tag);
	return slot != nullptr && slot->Length() > 0
		? slot
		: ref new String(L"1");
}

String^ DirectXPage::SelectedSaveStateSlotLabel()
{
	std::wstring label(L"Slot ");
	label += SelectedSaveStateSlotId()->Data();
	return ref new String(label.c_str());
}

String^ DirectXPage::SelectedSaveStateFolderPath()
{
	return BochsUwpStorage::GetSaveStateFolderPath(SelectedSaveStateSlotId());
}

void DirectXPage::SetBootSequencePrimary(const wchar_t *primary)
{
	const wchar_t *first = primary != nullptr ? primary : L"disk";
	SelectBootComboBoxItem(bootDeviceComboBox, first);

	const wchar_t *fallbacks[] = { L"disk", L"cdrom", L"floppy" };
	std::vector<std::wstring> tags;
	AddBootTag(tags, first);
	for (const wchar_t *fallback : fallbacks)
	{
		AddBootTag(tags, fallback);
	}

	SelectBootComboBoxItem(bootDevice2ComboBox, tags.size() > 1 ? tags[1].c_str() : L"none");
	SelectBootComboBoxItem(bootDevice3ComboBox, tags.size() > 2 ? tags[2].c_str() : L"none");
	m_bootTarget = SelectedBootSequence();
}

void DirectXPage::ApplyMachineProfile(String^ profile)
{
	if (profile == nullptr)
	{
		return;
	}

	m_applyingMachineProfile = true;
	if (wcscmp(profile->Data(), L"legacy486") == 0)
	{
		memorySlider->Value = 64;
		SelectBootComboBoxItem(cpuModelComboBox, L"i486dx4");
		SelectBootComboBoxItem(networkAdapterComboBox, L"none");
		SelectBootComboBoxItem(usbControllerComboBox, L"none");
		SelectBootComboBoxItem(usbDeviceComboBox, L"none");
		SelectBootComboBoxItem(pciChipsetComboBox, L"i430fx");
		SelectBootComboBoxItem(displayLibraryComboBox, L"uwp_dx");
		SelectBootComboBoxItem(videoExtensionComboBox, L"none");
		acpiToggleSwitch->IsOn = false;
		hpetToggleSwitch->IsOn = false;
		ioApicToggleSwitch->IsOn = false;
		videoUpdateSlider->Value = 10;
		rfbTimeoutSlider->Value = 0;
		videoRealtimeToggleSwitch->IsOn = true;
	}
	else if (wcscmp(profile->Data(), L"pentium") == 0)
	{
		memorySlider->Value = 128;
		SelectBootComboBoxItem(cpuModelComboBox, L"pentium_mmx");
		SelectBootComboBoxItem(networkAdapterComboBox, L"ne2k");
		SelectBootComboBoxItem(usbControllerComboBox, L"uhci");
		SelectBootComboBoxItem(usbDeviceComboBox, L"mouse");
		SelectBootComboBoxItem(pciChipsetComboBox, L"i440fx");
		SelectBootComboBoxItem(displayLibraryComboBox, L"uwp_dx");
		SelectBootComboBoxItem(videoExtensionComboBox, L"vbe");
		SelectBootComboBoxItem(vbeMemoryComboBox, L"8");
		acpiToggleSwitch->IsOn = true;
		hpetToggleSwitch->IsOn = true;
		ioApicToggleSwitch->IsOn = true;
		videoUpdateSlider->Value = 15;
		rfbTimeoutSlider->Value = 0;
		videoRealtimeToggleSwitch->IsOn = true;
	}
	else if (wcscmp(profile->Data(), L"modern") == 0)
	{
		memorySlider->Value = 1024;
		SelectBootComboBoxItem(cpuModelComboBox, L"corei7_haswell_4770");
		SelectBootComboBoxItem(networkAdapterComboBox, L"e1000");
		SelectBootComboBoxItem(usbControllerComboBox, L"xhci");
		SelectBootComboBoxItem(usbDeviceComboBox, L"tablet");
		SelectBootComboBoxItem(pciChipsetComboBox, L"i440bx");
		SelectBootComboBoxItem(displayLibraryComboBox, L"uwp_dx");
		SelectBootComboBoxItem(videoExtensionComboBox, L"vbe");
		SelectBootComboBoxItem(vbeMemoryComboBox, L"32");
		acpiToggleSwitch->IsOn = true;
		hpetToggleSwitch->IsOn = true;
		ioApicToggleSwitch->IsOn = true;
		videoUpdateSlider->Value = 30;
		rfbTimeoutSlider->Value = 0;
		videoRealtimeToggleSwitch->IsOn = true;
	}
	else
	{
		memorySlider->Value = 512;
		SelectBootComboBoxItem(cpuModelComboBox, L"corei7_haswell_4770");
		SelectBootComboBoxItem(networkAdapterComboBox, L"ne2k");
		SelectBootComboBoxItem(usbControllerComboBox, L"none");
		SelectBootComboBoxItem(usbDeviceComboBox, L"none");
		SelectBootComboBoxItem(pciChipsetComboBox, L"i440fx");
		SelectBootComboBoxItem(displayLibraryComboBox, L"uwp_dx");
		SelectBootComboBoxItem(videoExtensionComboBox, L"vbe");
		SelectBootComboBoxItem(vbeMemoryComboBox, L"16");
		acpiToggleSwitch->IsOn = true;
		hpetToggleSwitch->IsOn = true;
		ioApicToggleSwitch->IsOn = true;
		videoUpdateSlider->Value = 10;
		rfbTimeoutSlider->Value = 0;
		videoRealtimeToggleSwitch->IsOn = true;
	}

	m_applyingMachineProfile = false;
	UpdateMemoryLabel();
	UpdateVideoLabel();
	UpdateCommandState();
}

void DirectXPage::MarkMachineProfileCustom()
{
	if (!m_isPageInitialized || m_emulationStarted || m_applyingMachineProfile ||
		machineProfileComboBox == nullptr)
	{
		return;
	}
	if (wcscmp(ComboBoxTagString(machineProfileComboBox, L"custom")->Data(), L"custom") == 0)
	{
		return;
	}

	SelectBootComboBoxItem(machineProfileComboBox, L"custom");
}

bool DirectXPage::SelectedSoundEnabled()
{
	return soundToggleSwitch == nullptr || soundToggleSwitch->IsOn;
}

String^ DirectXPage::SelectedNetworkAdapter()
{
	return ComboBoxTagString(networkAdapterComboBox, L"ne2k");
}

int DirectXPage::SelectedNetworkSocketPort()
{
	if (networkSocketPortTextBox == nullptr || networkSocketPortTextBox->Text == nullptr)
	{
		return 40000;
	}

	int port = _wtoi(networkSocketPortTextBox->Text->Data());
	if (port < 1024)
	{
		return 40000;
	}
	if (port > 65534)
	{
		return 65534;
	}
	return port;
}

String^ DirectXPage::SelectedUsbController()
{
	return ComboBoxTagString(usbControllerComboBox, L"none");
}

String^ DirectXPage::SelectedUsbDevice()
{
	return ComboBoxTagString(usbDeviceComboBox, L"none");
}

String^ DirectXPage::SelectedSerialMode()
{
	return ComboBoxTagString(serialModeComboBox, L"disabled");
}

bool DirectXPage::SelectedParallelEnabled()
{
	return parallelToggleSwitch != nullptr && parallelToggleSwitch->IsOn;
}

String^ DirectXPage::SelectedPciChipset()
{
	return ComboBoxTagString(pciChipsetComboBox, L"i440fx");
}

bool DirectXPage::SelectedAcpiEnabled()
{
	String^ chipset = SelectedPciChipset();
	if (wcscmp(chipset->Data(), L"i430fx") == 0)
	{
		return false;
	}
	if (wcscmp(chipset->Data(), L"i440bx") == 0)
	{
		return true;
	}
	return acpiToggleSwitch == nullptr || acpiToggleSwitch->IsOn;
}

bool DirectXPage::SelectedHpetEnabled()
{
	if (wcscmp(SelectedPciChipset()->Data(), L"i430fx") == 0)
	{
		return false;
	}
	return hpetToggleSwitch == nullptr || hpetToggleSwitch->IsOn;
}

bool DirectXPage::SelectedIoApicEnabled()
{
	return ioApicToggleSwitch == nullptr || ioApicToggleSwitch->IsOn;
}

String^ DirectXPage::SelectedDisplayLibrary()
{
	return ComboBoxTagString(displayLibraryComboBox, L"uwp_dx");
}

int DirectXPage::SelectedRfbTimeout()
{
	if (rfbTimeoutSlider == nullptr)
	{
		return 0;
	}
	return static_cast<int>(rfbTimeoutSlider->Value + 0.5);
}

String^ DirectXPage::SelectedVideoExtension()
{
	return ComboBoxTagString(videoExtensionComboBox, L"vbe");
}

int DirectXPage::SelectedVideoUpdateFreq()
{
	if (videoUpdateSlider == nullptr)
	{
		return 10;
	}
	return static_cast<int>(videoUpdateSlider->Value + 0.5);
}

bool DirectXPage::SelectedVideoRealtime()
{
	return videoRealtimeToggleSwitch == nullptr || videoRealtimeToggleSwitch->IsOn;
}

int DirectXPage::SelectedVbeMemoryMb()
{
	String^ value = ComboBoxTagString(vbeMemoryComboBox, L"16");
	return _wtoi(value->Data());
}

void DirectXPage::UpdateVideoLabel()
{
	if (videoUpdateText == nullptr)
	{
		return;
	}

	int freq = SelectedVideoUpdateFreq();
	std::wstring text = freq == 0 ? L"device" : std::to_wstring(freq) + L" Hz";
	videoUpdateText->Text = ref new String(text.c_str());
	if (vbeMemoryComboBox != nullptr)
	{
		vbeMemoryComboBox->IsEnabled = !m_emulationStarted && wcscmp(SelectedVideoExtension()->Data(), L"vbe") == 0;
	}
	if (rfbTimeoutText != nullptr)
	{
		std::wstring timeoutText = std::to_wstring(SelectedRfbTimeout());
		timeoutText += L" s";
		rfbTimeoutText->Text = ref new String(timeoutText.c_str());
	}
	if (rfbTimeoutSlider != nullptr)
	{
		rfbTimeoutSlider->IsEnabled = !m_emulationStarted && wcscmp(SelectedDisplayLibrary()->Data(), L"rfb") == 0;
	}
	if (vgaBiosPathText != nullptr && (m_selectedVgaBiosPath == nullptr || m_selectedVgaBiosPath->Length() == 0))
	{
		vgaBiosPathText->Text = wcscmp(SelectedVideoExtension()->Data(), L"cirrus") == 0
			? ref new String(L"Built-in VGABIOS-lgpl-latest-cirrus.bin")
			: ref new String(L"Built-in VGABIOS-lgpl-latest.bin");
	}
}

void DirectXPage::UpdatePlatformFeatureState()
{
	if (networkSocketPortTextBox == nullptr || acpiToggleSwitch == nullptr ||
		hpetToggleSwitch == nullptr || ioApicToggleSwitch == nullptr)
	{
		return;
	}

	String^ network = SelectedNetworkAdapter();
	bool socketNetwork = wcsstr(network->Data(), L"socket") != nullptr;
	networkSocketPortTextBox->IsEnabled = !m_emulationStarted && socketNetwork;

	String^ chipset = SelectedPciChipset();
	bool i430fx = wcscmp(chipset->Data(), L"i430fx") == 0;
	bool i440bx = wcscmp(chipset->Data(), L"i440bx") == 0;
	if (!m_emulationStarted)
	{
		if (i430fx)
		{
			acpiToggleSwitch->IsOn = false;
			hpetToggleSwitch->IsOn = false;
		}
		else if (i440bx)
		{
			acpiToggleSwitch->IsOn = true;
		}
	}

	acpiToggleSwitch->IsEnabled = !m_emulationStarted && !i430fx && !i440bx;
	hpetToggleSwitch->IsEnabled = !m_emulationStarted && !i430fx;
	ioApicToggleSwitch->IsEnabled = !m_emulationStarted;
}

void DirectXPage::UpdateMemoryLabel()
{
	if (memoryValueText == nullptr)
	{
		return;
	}

	int guestMemory = SelectedMemoryMb();
	int hostMemory = BochsUwpStorage::EffectiveHostMemoryMb(guestMemory);
	std::wstring text = std::to_wstring(guestMemory);
	text += L" MB guest";
	if (hostMemory < guestMemory)
	{
		text += L"; host ";
		text += std::to_wstring(hostMemory);
		text += L" MB";
	}
	memoryValueText->Text = ref new String(text.c_str());
	UpdateBochsrcPreview();
}

void DirectXPage::UpdateCaptureIndicators()
{
	if (keyboardCaptureIndicator == nullptr || mouseCaptureIndicator == nullptr)
	{
		return;
	}

	bool keyboardCaptured = m_emulationStarted && !m_emulationPaused && m_windowVisible && !m_rfbDisplayActive;
	bool mouseCaptured = keyboardCaptured && m_mouseCaptured;
	keyboardCaptureIndicator->Visibility = keyboardCaptured
		? Windows::UI::Xaml::Visibility::Visible
		: Windows::UI::Xaml::Visibility::Collapsed;
	mouseCaptureIndicator->Visibility = mouseCaptured
		? Windows::UI::Xaml::Visibility::Visible
		: Windows::UI::Xaml::Visibility::Collapsed;
}

void DirectXPage::UpdateCommandState()
{
	if (toggleTabsButton == nullptr || startEmulatorButton == nullptr || pauseButton == nullptr ||
		resumeButton == nullptr || shutdownButton == nullptr || saveButton == nullptr ||
		restoreButton == nullptr || runtimeFloppyAButton == nullptr ||
		runtimeFloppyBButton == nullptr || runtimeCdromButton == nullptr ||
		runtimeEjectButton == nullptr || machineProfileComboBox == nullptr ||
		displayLibraryComboBox == nullptr || rfbTimeoutSlider == nullptr ||
		networkSocketPortTextBox == nullptr || pciChipsetComboBox == nullptr ||
		acpiToggleSwitch == nullptr || hpetToggleSwitch == nullptr ||
		ioApicToggleSwitch == nullptr ||
		videoExtensionComboBox == nullptr || videoUpdateSlider == nullptr ||
		videoRealtimeToggleSwitch == nullptr || vbeMemoryComboBox == nullptr)
	{
		return;
	}

	bool hasDisk = m_selectedDiskPath != nullptr && m_selectedDiskPath->Length() > 0;
	bool hasDisk2 = m_selectedDisk2Path != nullptr && m_selectedDisk2Path->Length() > 0;
	bool hasFloppy = m_selectedFloppyPath != nullptr && m_selectedFloppyPath->Length() > 0;
	bool hasFloppyB = m_selectedFloppyBPath != nullptr && m_selectedFloppyBPath->Length() > 0;
	bool hasCdrom = m_selectedCdromPath != nullptr && m_selectedCdromPath->Length() > 0;
	bool hasCdrom2 = m_selectedCdrom2Path != nullptr && m_selectedCdrom2Path->Length() > 0;
	bool hasSharedFolder = m_selectedSharedFolderPath != nullptr && m_selectedSharedFolderPath->Length() > 0;
	bool hasUsbImage = m_selectedUsbImagePath != nullptr && m_selectedUsbImagePath->Length() > 0;
	bool usbMassStorage = wcscmp(SelectedUsbDevice()->Data(), L"disk") == 0 ||
		wcscmp(SelectedUsbDevice()->Data(), L"cdrom") == 0 ||
		wcscmp(SelectedUsbDevice()->Data(), L"floppy") == 0;
	bool usbEnabled = wcscmp(SelectedUsbController()->Data(), L"none") != 0;
	toggleTabsButton->Label = m_tabsEnabled
		? ref new String(L"Hide tabs")
		: ref new String(L"Show tabs");
	chooseDiskButton->IsEnabled = !m_emulationStarted;
	resetDiskButton->IsEnabled = !m_emulationStarted && hasDisk;
	chooseDisk2Button->IsEnabled = !m_emulationStarted;
	resetDisk2Button->IsEnabled = !m_emulationStarted && hasDisk2;
	chooseFloppyButton->IsEnabled = !m_emulationStarted;
	resetFloppyButton->IsEnabled = !m_emulationStarted && hasFloppy;
	chooseFloppyBButton->IsEnabled = !m_emulationStarted;
	resetFloppyBButton->IsEnabled = !m_emulationStarted && hasFloppyB;
	chooseCdromButton->IsEnabled = !m_emulationStarted;
	resetCdromButton->IsEnabled = !m_emulationStarted && hasCdrom;
	chooseCdrom2Button->IsEnabled = !m_emulationStarted;
	resetCdrom2Button->IsEnabled = !m_emulationStarted && hasCdrom2;
	chooseSharedFolderButton->IsEnabled = !m_emulationStarted;
	resetSharedFolderButton->IsEnabled = !m_emulationStarted && hasSharedFolder;
	chooseUsbImageButton->IsEnabled = !m_emulationStarted && usbEnabled && usbMassStorage;
	resetUsbImageButton->IsEnabled = !m_emulationStarted && hasUsbImage;
	resetBiosButton->IsEnabled = !m_emulationStarted;
	selectBiosButton->IsEnabled = !m_emulationStarted;
	resetVgaBiosButton->IsEnabled = !m_emulationStarted;
	selectVgaBiosButton->IsEnabled = !m_emulationStarted;
	memorySlider->IsEnabled = !m_emulationStarted;
	cpuModelComboBox->IsEnabled = !m_emulationStarted;
	diskImageModeComboBox->IsEnabled = !m_emulationStarted;
	disk2ImageModeComboBox->IsEnabled = !m_emulationStarted;
	bootDeviceComboBox->IsEnabled = !m_emulationStarted;
	bootDevice2ComboBox->IsEnabled = !m_emulationStarted;
	bootDevice3ComboBox->IsEnabled = !m_emulationStarted;
	soundToggleSwitch->IsEnabled = !m_emulationStarted;
	networkAdapterComboBox->IsEnabled = !m_emulationStarted;
	pciChipsetComboBox->IsEnabled = !m_emulationStarted;
	usbControllerComboBox->IsEnabled = !m_emulationStarted;
	usbDeviceComboBox->IsEnabled = !m_emulationStarted && usbEnabled;
	serialModeComboBox->IsEnabled = !m_emulationStarted;
	parallelToggleSwitch->IsEnabled = !m_emulationStarted;
	saveStateSlotComboBox->IsEnabled = !m_emulationStarted;
	startEmulatorButton->IsEnabled = !m_emulationStarted &&
		(hasDisk || hasDisk2 || hasFloppy || hasFloppyB || hasCdrom || hasCdrom2 || m_saveStateAvailable);
	pauseButton->IsEnabled = m_emulationStarted && !m_emulationPaused;
	resumeButton->IsEnabled = m_emulationStarted && m_emulationPaused;
	shutdownButton->IsEnabled = m_emulationStarted;
	saveButton->IsEnabled = m_emulationStarted;
	restoreButton->IsEnabled = m_saveStateAvailable;
	runtimeFloppyAButton->IsEnabled = m_emulationStarted;
	runtimeFloppyBButton->IsEnabled = m_emulationStarted;
	runtimeCdromButton->IsEnabled = m_emulationStarted;
	runtimeEjectButton->IsEnabled = m_emulationStarted;
	machineProfileComboBox->IsEnabled = !m_emulationStarted;
	displayLibraryComboBox->IsEnabled = !m_emulationStarted;
	rfbTimeoutSlider->IsEnabled = !m_emulationStarted && wcscmp(SelectedDisplayLibrary()->Data(), L"rfb") == 0;
	videoExtensionComboBox->IsEnabled = !m_emulationStarted;
	videoUpdateSlider->IsEnabled = !m_emulationStarted;
	videoRealtimeToggleSwitch->IsEnabled = !m_emulationStarted;
	vbeMemoryComboBox->IsEnabled = !m_emulationStarted && wcscmp(SelectedVideoExtension()->Data(), L"vbe") == 0;
	UpdatePlatformFeatureState();
	UpdateVideoLabel();
	UpdateDiskImageModeLabel();
	UpdateDiagnosticSummary();
	UpdateBochsrcPreview();
	UpdateCaptureIndicators();
	UpdateRfbStatusOverlay();
}

void DirectXPage::OnPointerPressed(Object^ sender, PointerRoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	if (!m_emulationStarted)
	{
		return;
	}
	FocusEmulatorSurface();
	if (m_rfbDisplayActive)
	{
		e->Handled = true;
		return;
	}
	PointerPoint^ point = e->GetCurrentPoint(swapChainPanel);
	m_lastPointerPosition = point->Position;
	m_pointerButtons = PointerButtonMask(point->Properties);

	if (!m_mouseCaptureUserReleased &&
		!BochsUwpBridge::IsMouseAbsolute() &&
		!BochsUwpBridge::IsMouseCaptured())
	{
		m_mouseCapturePending = true;
		m_main->RequestMouseCapture(true);
	}

	UpdateMouseCaptureFromCore();
	bool captured = m_mouseCaptured || m_mouseCapturePending;
	if (captured)
	{
		swapChainPanel->CapturePointer(e->Pointer);
		Window::Current->CoreWindow->PointerCursor = nullptr;
	}
	if (captured)
	{
		m_main->PointerPressed(0.0f, 0.0f, m_pointerButtons, false);
	}
	else
	{
		Point guestPoint = ScalePointerToGuest(point->Position);
		m_main->PointerPressed(guestPoint.X, guestPoint.Y, m_pointerButtons, true);
	}
	e->Handled = true;
}

void DirectXPage::OnPointerMoved(Object^ sender, PointerRoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	if (!m_emulationStarted)
	{
		return;
	}
	if (m_rfbDisplayActive)
	{
		e->Handled = true;
		return;
	}
	UpdateMouseCaptureFromCore();
	PointerPoint^ point = e->GetCurrentPoint(swapChainPanel);
	Point previousPoint = m_lastPointerPosition;
	m_pointerButtons = PointerButtonMask(point->Properties);
	m_lastPointerPosition = point->Position;
	if (m_mouseCaptured || m_mouseCapturePending)
	{
		float dx = static_cast<float>(point->Position.X - previousPoint.X);
		float dy = static_cast<float>(point->Position.Y - previousPoint.Y);
		if (dx != 0.0f || dy != 0.0f)
		{
			m_main->PointerMoved(dx, dy, m_pointerButtons, false);
		}
		e->Handled = true;
		return;
	}
	else
	{
		Point guestPoint = ScalePointerToGuest(point->Position);
		m_main->PointerMoved(guestPoint.X, guestPoint.Y, m_pointerButtons, true);
	}
	e->Handled = true;
}

void DirectXPage::OnPointerReleased(Object^ sender, PointerRoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	if (!m_emulationStarted)
	{
		return;
	}
	if (m_rfbDisplayActive)
	{
		e->Handled = true;
		return;
	}
	UpdateMouseCaptureFromCore();
	PointerPoint^ point = e->GetCurrentPoint(swapChainPanel);
	m_pointerButtons = PointerButtonMask(point->Properties);
	m_lastPointerPosition = point->Position;
	if (m_mouseCaptured || m_mouseCapturePending)
	{
		m_main->PointerReleased(0.0f, 0.0f, m_pointerButtons, false);
	}
	else
	{
		Point guestPoint = ScalePointerToGuest(point->Position);
		m_main->PointerReleased(guestPoint.X, guestPoint.Y, m_pointerButtons, true);
		swapChainPanel->ReleasePointerCaptures();
	}
	e->Handled = true;
}

void DirectXPage::OnPointerWheelChanged(Object^ sender, PointerRoutedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	if (!m_emulationStarted)
	{
		return;
	}
	if (m_rfbDisplayActive)
	{
		e->Handled = true;
		return;
	}
	UpdateMouseCaptureFromCore();
	PointerPoint^ point = e->GetCurrentPoint(swapChainPanel);
	m_pointerButtons = PointerButtonMask(point->Properties);
	if (m_mouseCaptured || m_mouseCapturePending)
	{
		m_lastPointerPosition = point->Position;
		m_main->PointerWheelChanged(0.0f, 0.0f,
			point->Properties->MouseWheelDelta,
			m_pointerButtons,
			false);
	}
	else
	{
		Point guestPoint = ScalePointerToGuest(point->Position);
		m_lastPointerPosition = point->Position;
		m_main->PointerWheelChanged(guestPoint.X, guestPoint.Y,
			point->Properties->MouseWheelDelta,
			m_pointerButtons,
			true);
	}
	e->Handled = true;
}

unsigned DirectXPage::PointerButtonMask(PointerPointProperties^ properties)
{
	unsigned buttons = 0;
	if (properties->IsLeftButtonPressed)
	{
		buttons |= 0x01;
	}
	if (properties->IsRightButtonPressed)
	{
		buttons |= 0x02;
	}
	if (properties->IsMiddleButtonPressed)
	{
		buttons |= 0x04;
	}
	if (properties->IsXButton1Pressed)
	{
		buttons |= 0x08;
	}
	if (properties->IsXButton2Pressed)
	{
		buttons |= 0x10;
	}
	return buttons;
}

Point DirectXPage::ScalePointerToGuest(Point point)
{
	double width = swapChainPanel->ActualWidth > 1.0 ? swapChainPanel->ActualWidth : 1.0;
	double height = swapChainPanel->ActualHeight > 1.0 ? swapChainPanel->ActualHeight : 1.0;
	float x = static_cast<float>(point.X * 0x7fff / width);
	float y = static_cast<float>(point.Y * 0x7fff / height);
	if (x < 0.0f)
	{
		x = 0.0f;
	}
	else if (x > 0x7fff)
	{
		x = static_cast<float>(0x7fff);
	}
	if (y < 0.0f)
	{
		y = 0.0f;
	}
	else if (y > 0x7fff)
	{
		y = static_cast<float>(0x7fff);
	}
	return Point(x, y);
}

unsigned DirectXPage::NormalizeVirtualKey(VirtualKey virtualKey, CorePhysicalKeyStatus keyStatus)
{
	unsigned nativeKey = static_cast<unsigned>(virtualKey);
	if (nativeKey == 0x0d || nativeKey == 0x10 || nativeKey == 0x11 || nativeKey == 0x12)
	{
		return NativeKeyFromVirtualKey(nativeKey, keyStatus);
	}
	if (nativeKey == 0x13)
	{
		return NativeKeyFromVirtualKey(nativeKey, keyStatus);
	}

	unsigned physicalKey = NativeKeyFromPhysicalScanCode(
		keyStatus.ScanCode,
		keyStatus.IsExtendedKey);
	if (physicalKey != NativeKeyNone)
	{
		return physicalKey;
	}

	return NativeKeyFromVirtualKey(nativeKey, keyStatus);
}

bool DirectXPage::HandleMouseCaptureShortcut(KeyEventArgs^ args, bool pressed)
{
	if (args == nullptr)
	{
		return false;
	}

	CoreWindow^ window = Window::Current->CoreWindow;
	bool controlDown = (window->GetKeyState(VirtualKey::Control) & CoreVirtualKeyStates::Down) == CoreVirtualKeyStates::Down;
	bool altDown = (window->GetKeyState(VirtualKey::Menu) & CoreVirtualKeyStates::Down) == CoreVirtualKeyStates::Down;
	bool isShortcutKey = args->VirtualKey == VirtualKey::M;
	bool shortcutDown = controlDown && altDown && isShortcutKey;

	if (pressed)
	{
		if (shortcutDown)
		{
			if (!m_mouseCaptureShortcutDown)
			{
				m_mouseCaptureShortcutDown = true;
				ToggleMouseCaptureFromShortcut();
			}
			return true;
		}
		return false;
	}

	if (isShortcutKey && m_mouseCaptureShortcutDown)
	{
		m_mouseCaptureShortcutDown = false;
		return true;
	}
	return false;
}

void DirectXPage::ToggleMouseCaptureFromShortcut()
{
	if (!m_emulationStarted || m_main == nullptr || m_rfbDisplayActive)
	{
		return;
	}

	UpdateMouseCaptureFromCore();
	if (m_mouseCaptured || m_mouseCapturePending || BochsUwpBridge::IsMouseCaptured())
	{
		m_mouseCaptureUserReleased = true;
		m_mouseCapturePending = false;
		m_main->RequestMouseCapture(false);
		ReleaseMouseCapture();
		startupStatusText->Text = "Mouse capture paused. Press Ctrl+Alt+M to continue.";
		return;
	}

	if (BochsUwpBridge::IsMouseAbsolute())
	{
		m_mouseCaptureUserReleased = false;
		startupStatusText->Text = "Absolute mouse is active; relative capture is not needed.";
		return;
	}

	m_mouseCaptureUserReleased = false;
	m_mouseCapturePending = true;
	m_main->RequestMouseCapture(true);
	UpdateMouseCaptureFromCore();
	startupStatusText->Text = "Mouse capture requested. Press Ctrl+Alt+M to pause.";
}

void DirectXPage::UpdateMouseCaptureFromCore()
{
	if (m_rfbDisplayActive)
	{
		ReleaseMouseCapture();
		return;
	}

	bool coreCaptured = BochsUwpBridge::IsMouseCaptured();
	bool coreAbsolute = BochsUwpBridge::IsMouseAbsolute();
	if (coreCaptured || coreAbsolute)
	{
		m_mouseCapturePending = false;
	}

	bool shouldCapture = m_emulationStarted &&
		!m_mouseCaptureUserReleased &&
		(coreCaptured || m_mouseCapturePending) &&
		!coreAbsolute;
	if (m_mouseCaptured == shouldCapture)
	{
		UpdateCaptureIndicators();
		return;
	}

	m_mouseCaptured = shouldCapture;
	if (m_mouseCaptured)
	{
		Window::Current->CoreWindow->PointerCursor = nullptr;
	}
	else
	{
		m_mouseCapturePending = false;
		swapChainPanel->ReleasePointerCaptures();
		m_pointerButtons = 0;
		Window::Current->CoreWindow->PointerCursor = ref new CoreCursor(CoreCursorType::Arrow, 0);
	}
	UpdateCaptureIndicators();
}

void DirectXPage::ReleaseMouseCapture()
{
	if (!m_mouseCaptured && !m_mouseCapturePending)
	{
		return;
	}

	m_mouseCaptured = false;
	m_mouseCapturePending = false;
	m_pointerButtons = 0;
	swapChainPanel->ReleasePointerCaptures();
	Window::Current->CoreWindow->PointerCursor = ref new CoreCursor(CoreCursorType::Arrow, 0);
	UpdateCaptureIndicators();
}

void DirectXPage::OnCompositionScaleChanged(SwapChainPanel^ sender, Object^ args)
{
	UNREFERENCED_PARAMETER(args);
	critical_section::scoped_lock lock(m_main->GetCriticalSection());
	if (m_emulationStarted)
	{
		m_main->PauseEmulation();
	}
	m_deviceResources->SetCompositionScale(sender->CompositionScaleX, sender->CompositionScaleY);
	m_main->CreateWindowSizeDependentResources();
	if (m_emulationStarted)
	{
		m_main->ResumeEmulation();
	}
}

void DirectXPage::OnSwapChainPanelSizeChanged(Object^ sender, SizeChangedEventArgs^ e)
{
	UNREFERENCED_PARAMETER(sender);
	critical_section::scoped_lock lock(m_main->GetCriticalSection());
	if (m_emulationStarted)
	{
		m_main->PauseEmulation();
	}
	m_deviceResources->SetLogicalSize(e->NewSize);
	m_main->CreateWindowSizeDependentResources();
	if (m_emulationStarted)
	{
		m_main->ResumeEmulation();
	}
}
