#include <memory>
#include <shlobj.h>
#include <shlwapi.h>
#include <dispatcherqueue.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Data.Json.h>
#include "App.h"
#include "Env.h"

namespace {
	std::unique_ptr<App> app;
}

App::App()
{
	initDataPath();
	Env::init();
}

App::~App()
{
}

void App::init()
{
	if (!app)
	{
        initDispatcherQueueCtrl();
        App* rawPtr = new App();
        app = std::unique_ptr<App>(rawPtr);
	}
}

App* App::get()
{
	return app.get();
}

void App::initDispatcherQueueCtrl()
{
    DispatcherQueueOptions options{sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
    static winrt::Windows::System::DispatcherQueueController controller{ nullptr };
    auto hr = CreateDispatcherQueueController(options,reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(controller)));
    if (FAILED(hr))
    {
        MessageBox(NULL, L"无法创建DispatcherQueueController", L"系统提示", MB_OK);
        ExitProcess(-1);
    }
}

void App::initDataPath()
{
    PWSTR pathTmp = nullptr;
    auto hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &pathTmp);
    if (FAILED(hr) || pathTmp == nullptr) {
        MessageBox(nullptr, L"无法获取应用数据目录", L"系统提示", MB_OK);
        ExitProcess(-1);
    }
    dataPath.assign(pathTmp);
    CoTaskMemFree(pathTmp);
	dataPath /= L"WebWin"; //todo 这里应该使用用户在package.json中配置的应用名称，而不是固定的 DraftDepot

    std::error_code ec;
    std::filesystem::create_directories(dataPath, ec);
    if (ec) {
        auto msg = L"无法创建应用数据目录\n\n" + dataPath.wstring() + L"\n\nerror_code = " + std::to_wstring(ec.value());
        MessageBox(nullptr, msg.c_str(), L"系统提示", MB_OK);
        ExitProcess(-1);
    }
}
std::filesystem::path App::getDataPath()
{
    return app->dataPath;
}