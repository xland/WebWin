#pragma once
#include <string>
// wrl.h 必须在 winrt 头之前（照 Tray.h / Menu.h 的规矩）：
// 少了它，下面那句 using namespace winrt::... 会报 C2760
#include <wrl.h>
#include <Windows.h>
#include <WebView2.h>
#include <winrt/Windows.Data.Json.h>

#include "Page.h"

using namespace Microsoft::WRL;
using namespace winrt::Windows::Data::Json;

/**
 * process 域：启动子进程。
 *
 * v1 对外只有一个静态方法 `Process.spawn()`，它返回的不是"一串返回值"，
 * 而是一个 **Process 实例** —— 一个进程对象。输出与退出都作为**这个对象**的
 * 事件推回（见 web/src/Process.ts）。这样以后往同一对象上加 kill() / write()
 * / stdin，消息协议和通道都不用动。
 *
 * 事件只推给**发起 spawn 的那个文档**（顶级文档或那个 iframe），并且 C++ 在
 * 建进程时就替它订好了 —— 子进程一启动就开始输出，要是等页面那头的 on()
 * 消息到了再订阅，开头几行就没了（顺序说明见 Process.cpp 的 spawn）
 */
class Process
{
public:
	// 处理 type=process 域的消息：按 method 分发到本域的具体动作
	static void handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame = nullptr);
	/**
	 * 页面没了：杀掉它启动的、没标 detached 的子进程（谁启动谁负责，不留孤儿进程）。
	 *
	 * 这里只"断开目标"，不停线程也不关句柄 —— 泵线程还阻塞在 ReadFile 上，
	 * 现在关句柄等于让它读一个已经关掉的东西；句柄交给等退出的那条线程收
	 */
	static void removeByPage(Page* page);
	/** 这个 iframe 没了：同上 */
	static void removeByFrame(Page* page, ICoreWebView2Frame2* frame);
private:
	/** 建子进程：拼命令行 → 建两条管道 → CreateProcess → 起泵线程。失败时把原因填进 result */
	static void spawn(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame);
};
