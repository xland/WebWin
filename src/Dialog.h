#pragma once
#include <string>
#include <Windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <winrt/Windows.Data.Json.h>

#include "Page.h"

using namespace Microsoft::WRL;
using namespace winrt::Windows::Data::Json;

/**
 * dialog 域：系统对话框（选文件 / 存文件 / 消息框 / 在文件夹中显示）。
 *
 * 和 os / app / display 一样对外是**全局单例** `window.dialog`（见 webwin.ts），
 * 但对话框必须挂在某个窗口上才模态（禁用属主、层级正确、任务栏不闪），
 * 所以每个方法都要先拿到属主窗口句柄 —— 走 `page->getHwnd()` 取
 * "发这条消息的页面所在的窗口"。SDK 安装时把当前窗口 id 绑进 dialog，
 * 于是"在哪儿调就模态到哪儿"。
 *
 * **弹框不能在 WebView2 的事件回调里跑**：本域的入口 `handleApi` 是在
 * `WebMessageReceived` 的回调栈上被调用的，而 IFileDialog::Show / MessageBox
 * 内部是嵌套的模态消息循环 —— 等于"回调里套回调"。Runtime 会判为重入并直接
 * `__debugbreak()`（报 "Running a message loop synchronously in an event
 * handler ... can cause reentrancy issue"）。
 *
 * 所以 handleApi 只做两件事：把请求暂存进 pending 队列、用 App::runLater
 * 把弹框动作排到下一次消息循环；真正的弹框由 run() 在那里执行。
 * 响应也因此要等弹完才发回 —— handleApi 返回 true 告诉 Page 别再发一次。
 *
 * 不上后台线程：WebView2 的接口必须在 UI 线程上调，跨线程回传结果反而多一层出错面；
 * 而且跨线程传 HWND 当属主，模态层级和禁用关系都不如同线程可靠。
 *
 * 各方法把结果放进 result 的 data 字段，信封见 Json::setOk / setError
 *（协议见 arch/02-architecture.md）。
 */
class Dialog
{
public:
	// 处理 type=dialog 域的消息。已知方法会被暂存起来、排进下一次消息循环，
	// 此时返回 true —— 响应由 run() 发回，调用方不要再发一次；
	// 未知方法返回 false，让调用方照常回 unknown method
	static bool handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame = nullptr);
	/** 执行队列头上的一条请求并发回响应。**只**由 App::runLater 排下的任务调用 */
	static void run();
	/** 页面没了：丢掉它排着的请求，否则 run() 会用到悬挂的 Page* */
	static void removeByPage(Page* page);
	/**
	 * 弹一个**带输入框**的原生对话框。返回 false = 用户取消（此时不动 out）。
	 *
	 * 不是给页面调的 API，是框架内部接 JS 的 `prompt()` 用的（见 Page::onScriptDialog）。
	 * 调用方必须已经处在"栈退干净"的时机（App::runLater 排下来的任务里）——
	 * 它内部是同步的模态循环，在 WebView2 回调里调会被判重入
	 */
	static bool inputBox(HWND owner, const std::wstring& title, const std::wstring& msg,
		const std::wstring& defaultText, std::wstring& out);
private:
	/** 选文件 / 选文件夹。data = { cancel: boolean, data: string[] } */
	static void openPath(const JsonObject& params, HWND owner, JsonObject& result);
	/** 选保存位置。data = { cancel: boolean, data: string } —— 取消时 data 为空串 */
	static void savePath(const JsonObject& params, HWND owner, JsonObject& result);
	/** 消息框。data = 'ok' | 'cancel' | 'yes' | 'no' | 'abort' | 'retry' | 'ignore' */
	static void msgBox(const JsonObject& params, HWND owner, JsonObject& result);
	/** 打开资源管理器并选中指定文件；无返回值 */
	static void itemInFolder(const JsonObject& params, JsonObject& result);
};
