#pragma once
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>
// wrl.h 必须在 winrt 头之前（照 Tray.h / Http.h 的顺序）：
// 少了它下面的 using namespace winrt::... 会报 C2760
#include <wrl.h>
#include <Windows.h>
#include <WebView2.h>
#include <winrt/Windows.Data.Json.h>
// 遍历 JsonObject 需要它（同 Http.h 里的说明）
#include <winrt/Windows.Foundation.Collections.h>

#include "Page.h"

using namespace Microsoft::WRL;
using namespace winrt::Windows::Data::Json;

/**
 * `后台线程跑活、完了回 UI 线程发响应` 的公共通道。
 *
 * Http 发请求、Fs 读写文件都走这里 —— 两者面临同一组约束：
 * 1. **活不能跑在 UI 线程上**。网络请求、网络盘上的文件、被杀毒软件拦住的写入
 *    都可能卡住几秒甚至更久，跑在 UI 线程上窗口就冻住了；
 * 2. **回包又必须在 UI 线程上**。WebView2 的 PostWebMessageAsJson /
 *    PostSharedBufferToScript 都要求调用方在 UI 线程上。
 *
 * 于是流程固定成这样：
 *
 *   run()（UI 线程）→ 登记"谁在等" → co_await resume_background()（WinRT 线程池）
 *                                       ↓
 *                                     work()（不许碰 winrt 对象）
 *                                       ↓
 *          co_await resume_foreground(App::dispatcherQueue())
 *                                       ↓
 *                                     finish()（已回到 UI 线程）→ page->send
 *
 * 去程用 **WinRT 线程池**而不是 std::thread：线程复用，不必每个请求开一条 OS 线程，
 * 也不用 detach（没有"进程在退、线程还在跑"那一档事）。
 *
 * 回程**明确指名** UI 线程的 DispatcherQueue，不用 `winrt::apartment_context` ——
 * 后者靠 C++/WinRT 的单元记账，而本进程是用 `CoInitializeEx` 起 COM 的（`main.cpp`），
 * 没走 `winrt::init_apartment`，那条路能不能正确回到本线程没有保证。
 * 一旦没回到，后果是请求永远不回包、页面里的 Promise 一直挂着且没有报错。
 *
 * 而"页面还在不在"这件事**必须在 UI 线程上查**：页面销毁（Win 的 WM_DESTROY
 * → Async::removeByPage）同样发生在 UI 线程，两者互斥，所以在 finish() 里查到的
 * Page* 一定是有效的。放在后台线程里查，就会有"刚查完、还没发，窗口没了"的窗口期。
 *
 * 因为约束 2，`work` 不能直接返回 WinRT 的 JsonObject —— 那等于在后台线程上
 * 激活 winrt 对象。所以让它返回"在 UI 线程上才调用的 data 回调"。
 */
struct AsyncResult
{
	/** 非空 = 失败。此时 data 不会被调用 */
	std::wstring error;
	/**
	 * 成功时产出 data 字段的回调。**只在 UI 线程上调**。
	 * 用 IJsonValue 而不是 JsonObject，有两个原因：
	 * data 不一定是个对象（fs.readFile 直接给字符串，fs.stat 才给对象）；
	 * 而且 JsonObject **不派生自** JsonValue —— 两者是并列实现 IJsonValue 的类，
	 * 写 JsonValue 会让返回 JsonObject 的 lambda（Http 那样）转不过去。
	 * 为 null 表示没有返回值（比如 unlink），resolve(undefined)
	 */
	std::function<IJsonValue()> data;
	/** 走共享内存回传的字节（页面拿到 ArrayBuffer），不进 JSON 信封 */
	std::vector<uint8_t> buffer;
	/**
	 * 是否走共享内存。**不能只看 buffer 是否为空来判断** ——
	 * 0 字节也是合法结果（HTTP 204、空文件），那时 buffer 是空的但**依然要给页面
	 * 一个空 ArrayBuffer**，退化成普通响应的话页面拿到的是一个没有 buffer 字段的对象
	 */
	bool viaBuffer{ false };

	/** 造一个失败结果 */
	static AsyncResult fail(std::wstring message);
	/** 造一个成功结果，data 回调在 UI 线程上被调一次 */
	static AsyncResult ok(std::function<IJsonValue()> data);
	/**
	 * 造一个走共享内存的结果。
	 * data 回调照常产出 data 字段（放 size 之类的元数据），
	 * 页面最终拿到的是 { ...data, buffer, release }
	 */
	static AsyncResult binary(std::vector<uint8_t> bytes, std::function<IJsonValue()> data);
};

class Async
{
public:
	/**
	 * 把 work 排到后台线程，跑完回到 UI 线程把结果装进 result 发回页面。
	 *
	 * result 是 Page::handleMessage 造好的信封（已带 id），本函数只往里补 ok / data / error。
	 * 返回 true = 已排上（响应稍后到，调用方不要再发一次）；
	 * 返回 false = 当场没排进去，**调用方必须自己回一个 error**，
	 * 否则页面里那个 Promise 会永远挂着而且没有任何报错
	 */
	static bool run(Page* page, JsonObject& result, ICoreWebView2Frame2* frame, std::function<AsyncResult()> work);

	/** 页面没了：丢掉它在等的所有任务（http 的、fs 的都在这里） */
	static void removeByPage(Page* page);
};
