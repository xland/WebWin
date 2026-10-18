#pragma once
#include <string>
// wrl.h 必须在 winrt 头之前：C++/WinRT 的命名空间要靠它先铺好，
// 少了下面那句 using namespace winrt::... 会报 C2760（Tray.h / Menu.h 都是这个顺序）
#include <wrl.h>
#include <Windows.h>
#include <WebView2.h>
#include <winrt/Windows.Data.Json.h>
// 遍历 JsonObject（它是 IIterable<IKeyValuePair>）必须带这个：
// 少了它，begin() 的 auto 返回类型推不出来，会报 C3779
#include <winrt/Windows.Foundation.Collections.h>
// winhttp.h 必须在 Windows.h 之后（同 Tray.h 里 shellapi.h 的规矩）
#include <winhttp.h>

#include "Page.h"

using namespace Microsoft::WRL;
using namespace winrt::Windows::Data::Json;

/**
 * http 域：发 HTTP / HTTPS 请求。
 *
 * **为什么不让 JS 自己 fetch**：页面跑在虚拟主机 `https://{appId}` 上
 * （Page::loadPage 里的 SetVirtualHostNameToFolderMapping），这是个全世界都不
 * 认识的 origin —— 浏览器会带上 `Origin: https://webwin` 走标准 CORS 检查，
 * 服务端不白名单就拒。走 C++ 的 WinHTTP 不经过渲染进程，根本没有 CORS 这回事。
 *
 * **请求跑在后台线程上**：网络请求不能在 WebMessageReceived 的回调栈里同步跑，
 * 那会把 UI 线程卡死（窗口拖都拖不动）。线程和"页面还在不在"的判定统一由
 * Async 管（见 Async.h），本域只管 WinHTTP 本身。
 *
 * 本域目前只有 fetch。将来的 createServer（监听端口）也归这里 —— 那件事不可能
 * 纯 JS 做。见 arch/99-todo.md
 */
class Http
{
public:
	/**
	 * 处理 type=http 域的消息。
	 * 返回 true 表示响应稍后由本域发回、调用方别再发一次；
	 * 未知方法、或当场就错的（比如 url 为空）返回 false，让调用方照常回包（同 Dialog）
	 */
	static bool handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame = nullptr);

private:
	/**
	 * 发一个请求：登记挂起 → 开后台线程 → 线程里跑 WinHTTP。
	 * 返回 true = 已在后台跑起来了，响应稍后到；false = 当场就失败（result 已带 error）
	 */
	static bool fetch(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame);
};
