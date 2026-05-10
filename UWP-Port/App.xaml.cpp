#include "pch.h"
#include "DirectXPage.xaml.h"

using namespace UWP_Port;

using namespace Platform;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Storage;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Interop;
using namespace Windows::UI::Xaml::Navigation;

App::App() :
	m_directXPage(nullptr)
{
	InitializeComponent();
	Suspending += ref new SuspendingEventHandler(this, &App::OnSuspending);
	Resuming += ref new EventHandler<Object^>(this, &App::OnResuming);
}

void App::OnLaunched(LaunchActivatedEventArgs^ e)
{
#if _DEBUG
	if (IsDebuggerPresent())
	{
		DebugSettings->EnableFrameRateCounter = true;
	}
#endif

	auto rootFrame = dynamic_cast<Frame^>(Window::Current->Content);
	if (rootFrame == nullptr)
	{
		rootFrame = ref new Frame();
		rootFrame->NavigationFailed +=
			ref new NavigationFailedEventHandler(this, &App::OnNavigationFailed);
		Window::Current->Content = rootFrame;
	}

	if (rootFrame->Content == nullptr)
	{
		rootFrame->Navigate(TypeName(DirectXPage::typeid), e->Arguments);
	}

	if (m_directXPage == nullptr)
	{
		m_directXPage = dynamic_cast<DirectXPage^>(rootFrame->Content);
	}

	if (e->PreviousExecutionState == ApplicationExecutionState::Terminated &&
		m_directXPage != nullptr)
	{
		m_directXPage->LoadInternalState(ApplicationData::Current->LocalSettings->Values);
	}

	Window::Current->Activate();
}

void App::OnSuspending(Object^ sender, SuspendingEventArgs^ e)
{
	(void)sender;
	auto deferral = e->SuspendingOperation->GetDeferral();

	if (m_directXPage != nullptr)
	{
		m_directXPage->SaveInternalState(ApplicationData::Current->LocalSettings->Values);
	}

	deferral->Complete();
}

void App::OnResuming(Object^ sender, Object^ args)
{
	(void)sender;
	(void)args;

	if (m_directXPage != nullptr)
	{
		m_directXPage->LoadInternalState(ApplicationData::Current->LocalSettings->Values);
	}
}

void App::OnNavigationFailed(Object^ sender, NavigationFailedEventArgs^ e)
{
	(void)sender;
	throw ref new FailureException("Failed to load Page " + e->SourcePageType.Name);
}
