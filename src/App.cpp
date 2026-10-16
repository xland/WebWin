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
	// DispatcherQueueController：给当前 UI 线程装一个任务队列（见 App::runLater）。
	// 它必须活过所有排进来的任务，所以放在文件作用域，不做成某个函数里的静态
	winrt::Windows::System::DispatcherQueueController dispatcherCtrl{ nullptr };
	// 命令行 --path=... 指明的 package.json 所在目录；空则走默认（exe 同目录）
	std::filesystem::path packagePathOverride;
	// 实际生效的基准目录：命令行为主，否则 exe 同目录
	std::filesystem::path packageBaseDir;
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
        // 真正的依赖只有一条：读配置拿到应用 id → 数据目录按 id 建 →
        // Env::init 用那个目录创建 WebView2 环境（见 Env.cpp）。
        // 建窗口不碰数据目录（createCtrl 环境未就绪时会内部返回，等 Env::onEnvReady 补），
        // 把它排在 Env::init 之前，是为了让窗口尽早出现，不必等 WebView2 就绪
        auto windowConfig = app->readPackageJson();
        app->initDataPath();
        Win::Create(windowConfig); // 空对象也行：Win::readConfig 每个字段都会回落到默认值
        Env::init();
	}
}

App* App::get()
{
	return app.get();
}

void App::handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	// 立即退出：不关窗口、不弹确认、也不发退出前的事件（框架目前没有这类事件）。
	// 只投一条 WM_QUIT，消息循环退出后由 main 把它作为进程退出码返回（见 main.cpp）
	if (method == L"exit")
	{
		PostQuitMessage(Json::getInt(params, L"exitCode", 0));
		Json::setOk(result);
		return;
	}

	std::wstring data;
	if (method == L"getAppPath") data = getPackageBaseDir().wstring(); // package.json 所在目录
	else if (method == L"getExePath") data = Util::getExeDir().wstring();
	else if (method == L"getDataPath") data = getDataPath().wstring(); // %APPDATA%\WebWin\{应用id}
	else if (method == L"getVersion")
	{
		auto ver = get()->getVersion();
		data = std::to_wstring(ver[0]) + L"." + std::to_wstring(ver[1]) + L"." + std::to_wstring(ver[2]);
	}
	else if (method == L"getName") data = get()->getName();
	else if (method == L"getID") data = get()->getId();
	else { Json::setError(result, L"unknown method: " + method); return; }

	Json::setOk(result);
	result.SetNamedValue(L"data", JsonValue::CreateStringValue(data));
}

void App::setPackagePath(std::filesystem::path path)
{
	packagePathOverride = std::move(path);
}

std::filesystem::path App::getPackageBaseDir()
{
	return packageBaseDir;
}

void App::initDispatcherQueueCtrl()
{
    DispatcherQueueOptions options{sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
    auto hr = CreateDispatcherQueueController(options,reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(dispatcherCtrl)));
    if (FAILED(hr))
    {
        MessageBox(NULL, L"无法创建DispatcherQueueController", L"系统提示", MB_OK);
        ExitProcess(-1);
    }
}

bool App::runLater(std::function<void()> fn)
{
    if (!dispatcherCtrl) return false; // 还没建 / 已关掉：调用方得自己回个错
    // 只是"排进去"，不是当场调。任务由本线程的消息泵分派，执行时
    // WebView2 的回调栈已经退干净了 —— 所以 fn 里弹框、调 WebView2 都安全
    return dispatcherCtrl.DispatcherQueue().TryEnqueue([fn = std::move(fn)] { fn(); });
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
	// 数据目录：%APPDATA%\WebWin\{应用id}
	// 厂商目录 WebWin 保证不同框架用户的数据不会散落在 Roaming 根下；
	// 再按应用 id 分一层，同一份 exe 分发给不同应用时数据互不干扰
	// （id 缺失时已回落为默认值 webwin，见 readPackageJson）
	dataPath /= L"WebWin";
	dataPath /= id;

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

JsonObject App::readPackageJson()
{
	JsonObject windowConfig{ nullptr }; // package.json 没配 window 段落时就保持空
	// 命令行 --path=... 指明了 package.json 所在目录时，优先使用；
	// 否则按原约定走 exe 同目录。
	// 记录下"实际生效"的基准目录，供入口 HTML / webview 资源解析时复用
	packageBaseDir = packagePathOverride.empty() ? Util::getExeDir() : packagePathOverride;
	std::ifstream file(packageBaseDir / L"package.json", std::ios::binary);
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
		// 整个 window 段落交给 Win 自己解释（字段逐个回落见 Win::readConfig）；
		// 没配这个段落时是空对象，readConfig 读不到任何键，于是全走成员默认值
		windowConfig = Json::getObject(webwin, L"window");

		auto vers = Json::getIntArray(webwin, L"version");
		if (vers.size() >= 3) version = { vers[0], vers[1], vers[2] };
	}
	if (id.empty()) id = L"webwin"; // 显式配成空串也回落到默认值

	// 任务栏分组、跳转列表、通知归属都以 AppUserModelID 区分；
	// 同一份 exe 分发给不同应用时，靠它让每个应用有独立的任务栏身份。
	// 必须在窗口呈现之前调用
	SetCurrentProcessExplicitAppUserModelID(id.c_str());
	return windowConfig;
}
