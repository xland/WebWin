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
#include <chrono>

// JsonArray::Append 是 IVector 的方法，要显式带上集合头文件（Json.cpp / Display.cpp 同理）
#include <winrt/Windows.Foundation.Collections.h>

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

	// ---- 优雅退出用的隐藏消息窗口 + 定时器 ----
	// 退出流程（before-quit / will-quit）需要"延后一小段时间，给 JS 调用 stopQuit 的机会"，
	// 延期用定时器实现；SetTimer 需要一个真实 HWND 收 WM_TIMER，于是建一个隐藏的消息窗口。
	// 它随进程存活，不随订阅者增减销毁（参照 os / display / tray 那种隐藏窗口的做法）
	HWND appHwnd = nullptr;
	constexpr UINT TIMER_BEFORE_QUIT = 1;
	constexpr UINT TIMER_WILL_QUIT = 2;
	// before-quit / will-quit 下发后是异步到达 JS 的，框架给 JS 一个宽限窗口来调用 stopQuit；
	// 超过这个窗口仍未阻止，才继续推进退出。正常同步处理函数的往返时间远小于此值
	constexpr UINT QUIT_GRACE_MS = 200;

	LRESULT CALLBACK appWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (msg == WM_TIMER)
		{
			App::onQuitTimer(static_cast<UINT>(wParam));
			return 0;
		}
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}

	void createAppWindow()
	{
		if (appHwnd) return;
		WNDCLASS wc{};
		wc.lpfnWndProc = appWndProc;
		wc.hInstance = GetModuleHandle(nullptr);
		wc.lpszClassName = L"WebWinAppMsgWindow";
		RegisterClass(&wc);
		appHwnd = CreateWindowEx(0, wc.lpszClassName, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
	}

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
        app->initSingleInstance(); // 可能直接退出（被拒），也可能标记本进程为"被允许的额外实例"
        app->initDataPath();
        createAppWindow();
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
	// 事件订阅：app 与窗口无关，订阅走 NO_WINDOW 那条（见 Win::fireEvent）。
	// 页面侧 app.on('second-instance') 的首个订阅者会触发本分支把订阅登记进表，
	// 之后 Win::fireEvent(NO_WINDOW, ...) 才能找到订阅者并下发
	if (method == L"on" || method == L"off")
	{
		auto name = Json::getString(params, L"name", L"");
		if (name.empty()) { Json::setError(result, L"invalid event subscription"); return; }
		if (method == L"on") Win::subscribeEvent(Win::NO_WINDOW, name, page, frame);
		else Win::unsubscribeEvent(Win::NO_WINDOW, name, page, frame);
		Json::setOk(result);
		return;
	}

	// 页面在 second-instance 事件里调用：放行当前在等裁决的那一个第二实例。
	// 不调用则一律拒绝（server 侧 2 秒超时兜底），这是默认行为
	if (method == L"allowSecondInstanceOnce")
	{
		get()->allowSecondInstanceOnce();
		Json::setOk(result);
		return;
	}

	// 登记一次重启请求（不立即退出）；真正退出时 performRelaunch 才拉起新实例
	if (method == L"relaunch")
	{
		relaunch(params, result);
		return;
	}

	if (method == L"exit")
	{
		performRelaunch(); // 退出前先拉起登记过的新实例（若有），保证旧实例退出后新实例能起来
		PostQuitMessage(Json::getInt(params, L"exitCode", 0));
		Json::setOk(result);
		return;
	}

	// 优雅退出：先发 before-quit 给 JS 一个阻止的机会；没被阻止就关所有窗口，
	// 全关完再发 will-quit（同样可阻止），最后才 PostQuitMessage。
	// 已处于退出流程中则忽略重复调用
	if (method == L"quit")
	{
		if (quitPhase != QuitPhase::Idle) { Json::setOk(result); return; }
		quitPrevented = false;
		quitPhase = QuitPhase::BeforeQuit;
		JsonObject payload;
		Win::fireEvent(Win::NO_WINDOW, L"before-quit", payload);
		SetTimer(appHwnd, TIMER_BEFORE_QUIT, QUIT_GRACE_MS, nullptr);
		Json::setOk(result);
		return;
	}
	if (method == L"stopQuit")
	{
		// 仅在 before-quit / will-quit 事件处理函数里调用才有意义；其余情况忽略
		if (quitPhase == QuitPhase::BeforeQuit || quitPhase == QuitPhase::WillQuit) quitPrevented = true;
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

void App::onQuitTimer(UINT id)
{
	if (id == TIMER_BEFORE_QUIT)
	{
		KillTimer(appHwnd, TIMER_BEFORE_QUIT);
		if (quitPhase != QuitPhase::BeforeQuit) return;
		if (quitPrevented) { quitPhase = QuitPhase::Idle; return; } // 被 stopQuit 取消
		// 进入关窗阶段
		quitPhase = QuitPhase::Closing;
		quitPrevented = false;
		int n = Win::closeAll();
		if (n == 0) enterWillQuit(); // 没有窗口可关 → 直接进 will-quit
	}
	else if (id == TIMER_WILL_QUIT)
	{
		KillTimer(appHwnd, TIMER_WILL_QUIT);
		if (quitPhase != QuitPhase::WillQuit) return;
		if (quitPrevented) { quitPhase = QuitPhase::Idle; return; } // 被 stopQuit 取消：app 继续运行（窗口已关）
		performRelaunch(); // 退出前先拉起登记过的新实例（若有）
		PostQuitMessage(0);
	}
}

void App::enterWillQuit()
{
	if (quitPhase == QuitPhase::WillQuit) return; // 防重复触发
	quitPhase = QuitPhase::WillQuit;
	quitPrevented = false;
	JsonObject payload;
	Win::fireEvent(Win::NO_WINDOW, L"will-quit", payload);
	SetTimer(appHwnd, TIMER_WILL_QUIT, QUIT_GRACE_MS, nullptr);
}

bool App::inQuitSequence()
{
	return quitPhase == QuitPhase::Closing;
}

void App::relaunch(const JsonObject& params, JsonObject& result)
{
	relaunchExe_ = Json::getString(params, L"execPath", L"");
	relaunchArgs_.clear();
	auto args = Json::getArray(params, L"args");
	if (args) for (auto item : args)
	{
		if (item.ValueType() == JsonValueType::String) relaunchArgs_.push_back(item.GetString().c_str());
	}
	relaunchPending_ = true;
	Json::setOk(result);
}

void App::performRelaunch()
{
	if (!relaunchPending_) return;
	relaunchPending_ = false;

	// 单实例：第一实例持着互斥锁，直接在这里释放，让新实例能拿锁成为第一实例；
	// 否则新实例启动时会把自己当成"第二实例"、向即将退出的旧实例申请而被拒，relaunch 失败
	App* self = get();
	if (self && self->isFirstInstance && self->hSingleInstanceMutex)
	{
		ReleaseMutex(self->hSingleInstanceMutex);
		CloseHandle(self->hSingleInstanceMutex);
		self->hSingleInstanceMutex = nullptr;
		self->isFirstInstance = false;
		self->singleInstanceRunning = false; // 停掉接待管道 server，避免它把新实例当第二实例拒绝
	}

	// 新实例 exe：默认当前进程自身
	std::wstring exe = relaunchExe_.empty() ? Util::getExePath().wstring() : relaunchExe_;
	if (exe.empty()) return; // 拿不到路径就放弃（极端情况）

	std::wstring cmd = Util::quoteArg(exe);
	for (auto& a : relaunchArgs_) { cmd.push_back(L' '); cmd += Util::quoteArg(a); }

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	std::vector<wchar_t> buf(cmd.begin(), cmd.end());
	buf.push_back(L'\0');
	// 在当前进程仍存活时拉起新实例；之后旧实例才随 PostQuitMessage 退出，新实例独立存活
	CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
	if (pi.hProcess) CloseHandle(pi.hProcess);
	if (pi.hThread) CloseHandle(pi.hThread);
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

winrt::Windows::System::DispatcherQueue App::dispatcherQueue()
{
    // 声明见 App.h：调用方必须判空（Async::run 就是先判空再开工的）
    return dispatcherCtrl ? dispatcherCtrl.DispatcherQueue() : nullptr;
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

		// id 校验（T008）：必须是纯 ASCII 可见字符、不含空格、≤128 字符；
		// 缺失或非法都回落默认 "webwin"（非法时给一条调试输出，便于开发者发现配置写错）
		auto rawId = Json::getString(webwin, L"id", L"");
		if (!rawId.empty())
		{
			bool ok = rawId.size() <= 128;
			for (wchar_t c : rawId) { if (c < 0x21 || c > 0x7E) { ok = false; break; } }
			if (ok) id = rawId;
			else OutputDebugStringW((L"[WebWin] 配置 id 不合法（须纯 ASCII、不含空格、≤128 字符），已回落 \"webwin\"：\"" + rawId + L"\"\n").c_str());
		}
		name = Json::getString(webwin, L"name", name);
		quitWhenNoWindow = Json::getBool(webwin, L"quitWhenNoWindow", quitWhenNoWindow);
		// 整个 window 段落交给 Win 自己解释（字段逐个回落见 Win::readConfig）；
		// 没配这个段落时是空对象，readConfig 读不到任何键，于是全走成员默认值
		windowConfig = Json::getObject(webwin, L"window");

		// 单实例开关在**顶层**（不在 webwin 段内），所以从根对象 json 读
		secondInstanceNotify = Json::getBool(json, L"secondInstanceNotify", secondInstanceNotify);

		// version 校验（T008）：必须是 [major, minor, patch] 三个非负整数；
		// 否则回落默认 {1,1,1}
		auto vers = Json::getIntArray(webwin, L"version");
		if (vers.size() == 3 && vers[0] >= 0 && vers[1] >= 0 && vers[2] >= 0)
			version = { vers[0], vers[1], vers[2] };
		else if (!vers.empty())
			OutputDebugStringW(L"[WebWin] 配置 version 不合法（须为 [major, minor, patch] 三个整数），已回落默认值\n");
	}
	if (id.empty()) id = L"webwin"; // 显式配成空串也回落到默认值

	// 任务栏分组、跳转列表、通知归属都以 AppUserModelID 区分；
	// 同一份 exe 分发给不同应用时，靠它让每个应用有独立的任务栏身份。
	// 必须在窗口呈现之前调用
	SetCurrentProcessExplicitAppUserModelID(id.c_str());
	return windowConfig;
}

void App::initSingleInstance()
{
	if (!secondInstanceNotify) return; // 不开启单例：完全可多开
	singleInstanceKey = L"WebWinSingleInstance_" + id;

	// 先看是否已有第一实例（同名互斥量已存在）。
	// 第二实例只 Open 不 Create，因此不会干扰第一实例对互斥量的持有
	if (HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, singleInstanceKey.c_str()))
	{
		CloseHandle(h);
		// 是第二（及以后）实例：向第一实例申请，等裁决
		bool allow = requestSecondInstanceFromFirst();
		if (!allow)
		{
			// 被拒、或第一实例无响应（超时 / 页面没监听 second-instance）：直接退出，
			// 不创建任何窗口。用 ExitProcess 而非 return，让 main 的消息循环根本不启动
			ExitProcess(0);
		}
		// 被允许继续：本进程正常跑，但不持互斥量、不起管道 server（第一实例仍负责接待）
		isFirstInstance = false;
		return;
	}

	// 是第一实例：持有互斥量（进程退出时自动释放）+ 起管道 server 接待后续实例
	hSingleInstanceMutex = CreateMutexW(nullptr, FALSE, singleInstanceKey.c_str());
	isFirstInstance = true;
	startSecondInstanceServer();
}

bool App::requestSecondInstanceFromFirst()
{
	// 收集本进程的启动参数与工作目录，发给第一实例（同 Electron 的 second-instance）
	auto argv = Util::getCommandLineArgs();
	JsonArray argvJson{ nullptr };
	for (auto& a : argv) argvJson.Append(JsonValue::CreateStringValue(a));

	wchar_t cwd[MAX_PATH]{};
	GetCurrentDirectoryW(MAX_PATH, cwd);

	JsonObject req;
	req.SetNamedValue(L"argv", argvJson);
	req.SetNamedValue(L"cwd", JsonValue::CreateStringValue(cwd));

	// 命名管道名 = \\.\pipe\ + 互斥量后缀。CallNamedPipe 一步完成 连接→写→读→断开，
	// 最后一个参数是整体超时（毫秒）。第一实例无响应就当拒绝 —— 宁可杀掉也不允许多开
	std::wstring pipeName = L"\\\\.\\pipe\\" + singleInstanceKey;
	auto reqStr = req.Stringify();
	auto reqBuf = reqStr.c_str();
	DWORD reqLen = (DWORD)(reqStr.size() * sizeof(wchar_t));

	wchar_t resBuf[64]{};
	DWORD resLen = 0;
	if (!CallNamedPipeW(pipeName.c_str(), (LPVOID)reqBuf, reqLen,
			resBuf, sizeof(resBuf) - sizeof(wchar_t), &resLen, 3000))
		return false;

	resBuf[resLen / sizeof(wchar_t)] = 0;
	auto res = JsonObject::Parse(resBuf);
	if (!res) return false;
	return Json::getBool(res, L"allow", false);
}

void App::startSecondInstanceServer()
{
	singleInstanceRunning = true;
	std::thread(secondInstanceServerLoop, this).detach();
}

void App::secondInstanceServerLoop(App* self)
{
	std::wstring pipeName = L"\\\\.\\pipe\\" + self->singleInstanceKey;
	while (self->singleInstanceRunning)
	{
		HANDLE hPipe = CreateNamedPipeW(pipeName.c_str(),
			PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
			PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
		if (hPipe == INVALID_HANDLE_VALUE) break;

		if (ConnectNamedPipe(hPipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED)
		{
			wchar_t buf[4096]{};
			DWORD cb = 0;
			if (ReadFile(hPipe, buf, sizeof(buf) - sizeof(wchar_t), &cb, nullptr) && cb > 0)
			{
				buf[cb / sizeof(wchar_t)] = 0;
				auto req = JsonObject::Parse(buf);
				if (req)
				{
					// reqId 由第一实例统一生成，保证全局唯一：多个第二实例并发启动时不会撞键
					int reqId = self->nextSecondInstanceReqId.fetch_add(1);
					req.SetNamedValue(L"reqId", JsonValue::CreateNumberValue((double)reqId));
					// 挂起等页面裁决：先登记 promise，再排到 UI 线程把事件推给页面。
					// 页面若调用 app.allowSecondInstanceOnce() 则 set 这个 promise 放行；
					// 不调用 / 超时则视为拒绝（默认就是拒绝，这是防多开的安全默认）
					std::promise<bool> p;
					auto f = p.get_future();
					{
						std::lock_guard lock(self->secondInstanceMtx);
						self->pendingSecondInstance[reqId] = std::move(p);
					}
					App::runLater([self, reqId, req] {
						Win::fireEvent(Win::NO_WINDOW, L"second-instance", req);
					});
					// 等裁决，最多 2 秒；超时视为拒绝（页面没回包就别放过多开）
					bool allow = false;
					if (f.wait_for(std::chrono::seconds(2)) == std::future_status::ready)
						allow = f.get();

					// 无论是否被放行都要清掉这条 pending，否则超时后若有人调用
					// allowSecondInstanceOnce 会 set 一个没人等的 promise（泄漏）
					{
						std::lock_guard lock(self->secondInstanceMtx);
						self->pendingSecondInstance.erase(reqId);
					}

					// 写回裁决
					JsonObject res;
					res.SetNamedValue(L"allow", JsonValue::CreateBooleanValue(allow));
					auto resStr = res.Stringify();
					DWORD wcb = 0;
					WriteFile(hPipe, resStr.c_str(), (DWORD)(resStr.size() * sizeof(wchar_t)), &wcb, nullptr);
				}
			}
		}
		CloseHandle(hPipe);
	}
}

void App::allowSecondInstanceOnce()
{
	std::lock_guard lock(secondInstanceMtx);
	// 放行当前在等待裁决的那一个第二实例（通常唯一；并发时也只放最早的一个）。
	// 没有在等的请求（事件之外误调）则忽略 —— 默认就是拒绝，无需动作
	if (pendingSecondInstance.empty()) return;
	auto it = pendingSecondInstance.begin();
	it->second.set_value(true);
	pendingSecondInstance.erase(it);
}
