#pragma once

#include <array>
#include <functional>
#include <string>
#include <filesystem>
#include <Windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <winrt/Windows.Data.Json.h>

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
	// 处理 type=app 域的消息：按 method 分发到本域的具体动作
	static void handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame = nullptr);
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
};

