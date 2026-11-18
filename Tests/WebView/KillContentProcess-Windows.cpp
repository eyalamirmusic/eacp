#include "KillContentProcess.h"

#include <eacp/WebView/WebView/WebViewPlatform-Windows.h>

#include <wrl.h>

namespace KillContentProcessWindows
{
using Microsoft::WRL::ComPtr;

constexpr auto killedProcessExitCode = UINT {1};

UINT32 getFrameId(ICoreWebView2FrameInfo* frame)
{
    auto frameWithId = ComPtr<ICoreWebView2FrameInfo2> {};
    auto id = UINT32 {0};

    if (SUCCEEDED(frame->QueryInterface(IID_PPV_ARGS(&frameWithId))))
        frameWithId->get_FrameId(&id);

    return id;
}

bool isRunningFrame(ICoreWebView2ProcessExtendedInfo* process, UINT32 frameId)
{
    auto frames = ComPtr<ICoreWebView2FrameInfoCollection> {};
    auto iterator = ComPtr<ICoreWebView2FrameInfoCollectionIterator> {};

    if (FAILED(process->get_AssociatedFrameInfos(&frames)))
        return false;

    if (FAILED(frames->GetIterator(&iterator)))
        return false;

    auto hasFrame = BOOL {FALSE};

    for (iterator->get_HasCurrent(&hasFrame); hasFrame;
         iterator->MoveNext(&hasFrame))
    {
        auto frame = ComPtr<ICoreWebView2FrameInfo> {};
        iterator->GetCurrent(&frame);

        if (getFrameId(frame.Get()) == frameId)
            return true;
    }

    return false;
}

void terminateProcess(ICoreWebView2ProcessExtendedInfo* process)
{
    auto info = ComPtr<ICoreWebView2ProcessInfo> {};
    auto processId = INT32 {0};

    if (FAILED(process->get_ProcessInfo(&info)))
        return;

    info->get_ProcessId(&processId);

    auto windowsProcessId = static_cast<DWORD>(processId);
    auto handle = OpenProcess(PROCESS_TERMINATE, FALSE, windowsProcessId);

    if (handle == nullptr)
        return;

    TerminateProcess(handle, killedProcessExitCode);
    CloseHandle(handle);
}

void terminateRendererOfFrame(ICoreWebView2ProcessExtendedInfoCollection* processes,
                              UINT32 frameId)
{
    auto count = UINT32 {0};
    processes->get_Count(&count);

    for (auto index = UINT32 {0}; index < count; ++index)
    {
        auto process = ComPtr<ICoreWebView2ProcessExtendedInfo> {};

        if (SUCCEEDED(processes->GetValueAtIndex(index, &process))
            && isRunningFrame(process.Get(), frameId))
        {
            terminateProcess(process.Get());
            return;
        }
    }
}

UINT32 getMainFrameId(ICoreWebView2* webView)
{
    auto withFrameId = ComPtr<ICoreWebView2_20> {};
    auto id = UINT32 {0};

    if (SUCCEEDED(webView->QueryInterface(IID_PPV_ARGS(&withFrameId))))
        withFrameId->get_FrameId(&id);

    return id;
}

ComPtr<ICoreWebView2Environment13> getEnvironment(ICoreWebView2* webView)
{
    auto withEnvironment = ComPtr<ICoreWebView2_2> {};
    auto environment = ComPtr<ICoreWebView2Environment> {};
    auto environmentWithProcesses = ComPtr<ICoreWebView2Environment13> {};

    if (SUCCEEDED(webView->QueryInterface(IID_PPV_ARGS(&withEnvironment)))
        && SUCCEEDED(withEnvironment->get_Environment(&environment)))
        environment.As(&environmentWithProcesses);

    return environmentWithProcesses;
}
} // namespace KillContentProcessWindows

// Finds the renderer through WebView2's own process list: the one running the
// view's main frame. Asynchronous, so the kill lands on a later loop turn.
void killContentProcess(eacp::Graphics::WebView& webView)
{
    using namespace KillContentProcessWindows;

    auto coreWebView = eacp::Graphics::detail::coreWebView2Of(webView);

    if (!coreWebView)
        return;

    auto environment = getEnvironment(coreWebView.Get());

    if (!environment)
        return;

    environment->GetProcessExtendedInfos(
        Microsoft::WRL::Callback<
            ICoreWebView2GetProcessExtendedInfosCompletedHandler>(
            [mainFrameId = getMainFrameId(coreWebView.Get())](
                HRESULT result,
                ICoreWebView2ProcessExtendedInfoCollection* processes)
            {
                if (SUCCEEDED(result) && processes != nullptr)
                    terminateRendererOfFrame(processes, mainFrameId);

                return S_OK;
            })
            .Get());
}
