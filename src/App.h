#pragma once

#include <array>
#include <functional>
#include <string>
#include <filesystem>
#include <map>
#include <mutex>
#include <future>
#include <atomic>
#include <thread>
#include <Windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <winrt/Windows.Data.Json.h>
// DispatcherQueue：App 启动时给 UI 线程建了个控制器（见 runLater 的说明）。
// 把它暴露出来，是因为 Async 要用 resume_foreground 明确排回这个队列
#include <winrt/Windows.System.h>

#include "Page.h"

using namespace winrt::Windows::Data::Json;

class App
{
public:
	~App();
	static void init();
	static App* get();
	/**
	 * 把 fn 排进 UI 线程的**下一次消息循环**执行，不在调用点当场跑。
	 *
	 * 存在的原因是 WebView2 的重入限制：MessageBox / IFileDialog::Show /
	 * TrackPopupMenu 内部都是嵌套的模态消息循环，而这些调用点常常就在
	 * WebMessageReceived、ContextMenuRequested 这类事件回调的栈上 ——
	 * 在那里开循环等于"回调套回调"，Runtime 会直接 __debugbreak()
	 *（报 "Running a message loop synchronously in an event handler ...
	 * can cause reentrancy issue"）。迟一步执行，handler 返回、栈退干净了就没事。
	 *
	 * 底层是启动时建的 DispatcherQueueController（见 initDispatcherQueueCtrl）：
	 * 它只是给当前线程装了个任务队列，真正的分派由 main.cpp 那个消息泵驱动
	 *（Windows SDK 的 DispatcherQueue.h 原话："It is the responsibility of the
	 * caller to run a NTUSER Message pump loop so as to dispatch DispatcherQueue's
	 * tasks onto this thread."）。所以 fn 里既能安全弹框，也能安全调 WebView2。
	 *
	 * 返回 false = 没排进去（还没初始化 / 队列已关）。调用方必须处理：
	 * 否则请求永远不回包，页面里那个 Promise 会一直挂着，而且没有任何报错
	 */
	static bool runLater(std::function<void()> fn);
	/**
	 * UI 线程的任务队列本体（`runLater` 内部就是往它里面 TryEnqueue）。
	 *
	 * 单独暴露是给"想用协程回到 UI 线程"的调用方用的（Async 用
	 * `co_await winrt::resume_foreground(App::dispatcherQueue())`）。
	 * **没初始化时是 nullptr** —— 调用方必须先判空，往 nullptr 上 resume 没有意义。
	 *
	 * 为什么不用 `winrt::apartment_context` 那种写法：它靠 C++/WinRT 的单元记账，
	 * 而本进程是用 `CoInitializeEx` 起 COM 的（`main.cpp`），没走 `winrt::init_apartment`，
	 * 那条路能不能正确回到本线程没有保证。明确指名 DispatcherQueue 就没有这个疑问
	 */
	static winrt::Windows::System::DispatcherQueue dispatcherQueue();
	// 处理 type=app 域的消息：按 method 分发到本域的具体动作
	static void handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame = nullptr);
	// 优雅退出流程：app.quit 发 before-quit → 关所有窗口 → 全关完发 will-quit → 退出，
	// 两个事件里都能用 stopQuit 阻止。下面两个是 Win::winProc 在"最后一个窗口销毁"时用的钩子
	static bool inQuitSequence();   // 是否处于 app.quit() 发起的关窗阶段
	static void enterWillQuit();    // 所有窗口已关：发 will-quit 并进入待退出闸门（Win::winProc 调用）
	static void onQuitTimer(UINT id); // 隐藏窗口的 WM_TIMER 回调：按阶段推进退出 / 检查 stopQuit（App 隐藏窗口的 appWndProc 调用）
	static std::filesystem::path getDataPath();
	// 启动命令行 `--path=...` 指明的 package.json 所在目录，
	// 必须在 init() 之前调用；不调用则走默认（exe 同目录）
	static void setPackagePath(std::filesystem::path path);
	// 实际生效的 package.json 基准目录（命令行为主，否则 exe 同目录）；
	// 入口 HTML 相对路径、webview 资源目录都按它解析
	static std::filesystem::path getPackageBaseDir();
	// 读 package.json 填好本实例各个字段，并**返回** window 段落（没配就是空对象），
	// 由 init() 交给 Win::Create 去解释。不把它存在 App 里：只被用一次，存下来纯属多余
	JsonObject readPackageJson();
	const std::wstring& getId() const;
	const std::wstring& getName() const;
	const std::array<int, 3>& getVersion() const;
	bool getQuitWhenNoWindow() const;
private:
	App();
	static void initDispatcherQueueCtrl();
	void initDataPath();
private:
	std::filesystem::path dataPath;
	std::wstring id{ L"webwin" }; // 未配置时的默认应用 ID
	std::wstring name;
	std::array<int, 3> version{ 1, 1, 1 };
	bool quitWhenNoWindow{ true };

	// ---- 单实例（secondInstanceNotify）相关 ----
	// package.json 顶层 secondInstanceNotify：为 true 时按 app.id 单例，第二个实例
	// 启动会向第一个实例"申请"，由页面监听 second-instance 事件裁决是否放行
	bool secondInstanceNotify{ false };
	bool isFirstInstance{ true };     // 本进程是否"第一个实例"（持 mutex + 起管道 server）
	std::wstring singleInstanceKey;   // mutex / 命名管道名都按它派生（含 app.id）
	HANDLE hSingleInstanceMutex{ nullptr };
	std::atomic<bool> singleInstanceRunning{ false }; // 管道 server 线程的停止标志

	// 第二个实例申请时，第一实例把请求挂起等页面裁决：reqId → 裁决结果
	std::mutex secondInstanceMtx;
	std::map<int, std::promise<bool>> pendingSecondInstance;
	std::atomic<int> nextSecondInstanceReqId{ 1 };

	// 在 readPackageJson 之后调用：检测多开、必要时向第一实例申请或起管道 server
	void initSingleInstance();
	// 第二实例：连第一实例的管道发 argv+cwd，等裁决；返回 true=允许继续
	bool requestSecondInstanceFromFirst();
	// 第一实例：起一个线程监听管道，把申请转成 second-instance 事件推给页面
	void startSecondInstanceServer();
	static void secondInstanceServerLoop(App* self);
	// 页面在 second-instance 事件里调用：放行当前在等裁决的那一个第二实例
	void allowSecondInstanceOnce();

	// ---- 优雅退出（app.quit / before-quit / will-quit / stopQuit）----
	enum class QuitPhase { Idle, BeforeQuit, Closing, WillQuit };
	static inline QuitPhase quitPhase{ QuitPhase::Idle };
	static inline bool quitPrevented{ false };
};

