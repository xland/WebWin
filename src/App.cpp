#include <memory>
#include <fstream>
#include <shlobj.h>
#include <shlwapi.h>
#include <dispatcherqueue.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Data.Json.h>
#include "App.h"
#include "Env.h"
#include "Win.h"
#include "Json.h"
#include "Util.h"

using namespace winrt::Windows::Data::Json;

namespace {
	std::unique_ptr<App> app;
}

App::App()
{
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
        app->initDataPath();
        app->readPackageJson(); // 尽早读配置并建窗口，不必等 WebView2 环境就绪
        Env::init();
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

const std::wstring& App::getId() const
{
	return id;
}

const std::wstring& App::getName() const
{
	return name;
}

const std::array<int, 3>& App::getVersion() const
{
	return version;
}

bool App::getQuitWhenNoWindow() const
{
	return quitWhenNoWindow;
}

void App::readPackageJson()
{
	JsonObject window;
	std::ifstream file(Util::getExeDir() / L"package.json", std::ios::binary);
	if (file) // package.json 不存在时保持各字段默认值，window 为空对象
	{
		std::string content{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
		JsonObject json = JsonObject::Parse(Util::utf8ToWide(content));

		// 有 webwin 就用 webwin 对象，没有就把 package.json 本身当作 webwin 内容
		JsonObject webwin = Json::getObject(json, L"webwin");
		if (!webwin) webwin = json;

		id = Json::getString(webwin, L"id", id);
		name = Json::getString(webwin, L"name", name);
		quitWhenNoWindow = Json::getBool(webwin, L"quitWhenNoWindow", quitWhenNoWindow);
		auto windowObj = Json::getObject(webwin, L"window");
		if (windowObj) window = windowObj;

		auto vers = Json::getIntArray(webwin, L"version");
		if (vers.size() >= 3) version = { vers[0], vers[1], vers[2] };
	}
	if (id.empty()) id = L"webwin"; // 显式配成空串也回落到默认值

	// 任务栏分组、跳转列表、通知归属都以 AppUserModelID 区分；
	// 同一份 exe 分发给不同应用时，靠它让每个应用有独立的任务栏身份。
	// 必须在窗口呈现之前调用
	SetCurrentProcessExplicitAppUserModelID(id.c_str());

	Win::Create(window);
}
