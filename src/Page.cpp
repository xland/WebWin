#include <shlobj.h>
#include <shlwapi.h>
#include <deque>
#include <filesystem>
#include <fstream>

#include "App.h"
#include "Db.h"
#include "Dialog.h"
#include "Display.h"
#include "Env.h"
#include "Fs.h"
#include "Http.h"
#include "Menu.h"
#include "Net.h"
#include "Os.h"
#include "Process.h"
#include "Regedit.h"
#include "Res.h"
#include "Win.h"
#include "Page.h"
#include "Json.h"
#include "Util.h"
#include "Tray.h"

namespace {
	// 取 exe 内嵌的 RCDATA 资源（src/web/dist 下的产物，见 Res.rc）
	std::string loadTextResource(const wchar_t* name)
	{
		auto hRes = FindResource(nullptr, name, RT_RCDATA);
		if (!hRes) return {};
		auto hData = LoadResource(nullptr, hRes);
		if (!hData) return {};
		auto data = static_cast<const char*>(LockResource(hData));
		auto size = SizeofResource(nullptr, hRes);
		return { data, size };
	}

	// 事件回调给的是 ICoreWebView2Frame*，收发消息要用 ICoreWebView2Frame2*。
	// 同一个 COM 对象对同一个 IID 的 QueryInterface 结果恒等，
	// 所以可以用 Frame2 的指针值来认定"是不是同一个 iframe"
	ComPtr<ICoreWebView2Frame2> asFrame2(ICoreWebView2Frame* frame)
	{
		ComPtr<ICoreWebView2Frame> holder(frame);
		ComPtr<ICoreWebView2Frame2> frame2;
		holder.As(&frame2);
		return frame2;
	}

	/**
	 * 一次**还没弹**的 JS 对话框请求。
	 *
	 * 存在的唯一原因：弹框不能在 WebView2 回调里当场跑。MessageBox 和自建的输入框
	 * 窗口都是嵌套的模态消息循环，在回调栈上开循环会被 Runtime 判重入并
	 * `__debugbreak()`（见 doc/dialog.md「为什么不能当场弹」）。
	 * 所以先取 deferral 把事件挂起，等栈退干净了再弹。
	 */
	struct ScriptDialogTask
	{
		COREWEBVIEW2_SCRIPT_DIALOG_KIND kind{ COREWEBVIEW2_SCRIPT_DIALOG_KIND_ALERT };
		std::wstring message;     // alert / confirm / prompt 的提示；beforeunload 的文案
		std::wstring defaultText; // 仅 prompt：输入框的初始内容
		HWND owner{ nullptr };
		ComPtr<ICoreWebView2ScriptDialogOpeningEventArgs> args;
		ComPtr<ICoreWebView2Deferral> deferral;
	};

	std::deque<ScriptDialogTask> pendingScriptDialogs;

	/** 拿属主窗口的标题当对话框标题；没有（窗口已关 / 无标题）就用 fallback */
	std::wstring dialogTitle(HWND owner, const wchar_t* fallback)
	{
		if (!owner) return fallback;
		wchar_t buf[256]{};
		if (GetWindowText(owner, buf, 256) > 0) return buf;
		return fallback;
	}

	/** 真正弹框。由 App::runLater 排到下一次消息循环执行 */
	void runScriptDialog()
	{
		if (pendingScriptDialogs.empty()) return;
		auto task = std::move(pendingScriptDialogs.front());
		pendingScriptDialogs.pop_front();

		// 排队期间窗口可能已经关了：句柄失效就退化成无属主，别把野句柄递给 Win32
		HWND owner = IsWindow(task.owner) ? task.owner : nullptr;
		bool accepted = false; // 是否放行：confirm 的"是"、prompt 的"确定"

		switch (task.kind)
		{
		case COREWEBVIEW2_SCRIPT_DIALOG_KIND_ALERT:
			// alert 没有返回值，Accept 只是让那句 JS 继续往下走
			MessageBox(owner, task.message.c_str(), dialogTitle(owner, L"提示").c_str(),
				MB_OK | MB_ICONINFORMATION);
			accepted = true;
			break;

		case COREWEBVIEW2_SCRIPT_DIALOG_KIND_CONFIRM:
			accepted = MessageBox(owner, task.message.c_str(), dialogTitle(owner, L"确认").c_str(),
				MB_YESNO | MB_ICONQUESTION) == IDYES;
			break;

		case COREWEBVIEW2_SCRIPT_DIALOG_KIND_PROMPT:
		{
			std::wstring input;
			if (Dialog::inputBox(owner, dialogTitle(owner, L"输入").c_str(), task.message, task.defaultText, input))
			{
				task.args->put_ResultText(input.c_str());
				accepted = true;
			}
			break;
		}

		case COREWEBVIEW2_SCRIPT_DIALOG_KIND_BEFOREUNLOAD:
			// 网页在 onbeforeunload 里返回的那个字符串就是它想问的话，没给才用默认的
			accepted = MessageBox(owner,
				task.message.empty() ? L"确定要离开当前页面吗？" : task.message.c_str(),
				dialogTitle(owner, L"确认离开").c_str(),
				MB_YESNO | MB_ICONQUESTION) == IDYES;
			break;
		}

		// 不 Accept 就等于"用户点了取消"：confirm 拿 false、prompt 拿 null、导航被拦下
		if (accepted) task.args->Accept();
		// 不管结果如何都必须收尾，否则页面里那句 JS 会永远挂在等待上
		task.deferral->Complete();
	}
}


Page::Page(Win* win, const ComPtr<ICoreWebView2>& webview) :win{ win }, webview{ webview }
{
    // 用带 RequestSourceKinds 的新接口：老的 AddWebResourceRequestedFilter 已废弃，
    // 而且它只对主框架和同源 iframe 生效，其余 iframe 的请求拦不到
    ComPtr<ICoreWebView2_22> webview22;
    this->webview.As(&webview22);
    webview22->AddWebResourceRequestedFilterWithRequestSourceKinds(
        L"https://app.localhost/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL,
        COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_ALL);
    auto resRequestedCB = Callback<ICoreWebView2WebResourceRequestedEventHandler>(this, &Page::onRequest);
    webview->add_WebResourceRequested(resRequestedCB.Get(), nullptr);

    auto msgReceivedCB = Callback<ICoreWebView2WebMessageReceivedEventHandler>(this, &Page::onMsgReceived);
    webview->add_WebMessageReceived(msgReceivedCB.Get(), nullptr);

    auto reqPermissionCB = Callback<ICoreWebView2PermissionRequestedEventHandler>(this, &Page::onRequestPermission);
    webview->add_PermissionRequested(reqPermissionCB.Get(), nullptr);

    ComPtr<ICoreWebView2_2> webview2;
    this->webview.As(&webview2);
    auto domLoadedCB = Callback<ICoreWebView2DOMContentLoadedEventHandler>(this, &Page::onDomLoaded);
    webview2->add_DOMContentLoaded(domLoadedCB.Get(), nullptr);

    auto closeWindowCB = Callback<ICoreWebView2WindowCloseRequestedEventHandler>(this, &Page::onCloseWindow);
    webview->add_WindowCloseRequested(closeWindowCB.Get(), nullptr);

    // iframe 里的文档也要能跟 C++ 通信：CoreWebView2 的 WebMessageReceived
    // 只收顶级文档的消息，iframe 的得逐个 frame 订阅（见 onFrameCreated）
    ComPtr<ICoreWebView2_4> webview4;
    this->webview.As(&webview4);
    if (webview4)
    {
        auto frameCreatedCB = Callback<ICoreWebView2FrameCreatedEventHandler>(this, &Page::onFrameCreated);
        webview4->add_FrameCreated(frameCreatedCB.Get(), nullptr);
    }

    // 页面里点右键：挂了菜单就关掉浏览器默认的那个，改弹开发者建的菜单
    ComPtr<ICoreWebView2_11> webview11;
    this->webview.As(&webview11);
    auto contextMenuCB = Callback<ICoreWebView2ContextMenuRequestedEventHandler>(this, &Page::onContextMenuRequested);
    webview11->add_ContextMenuRequested(contextMenuCB.Get(), nullptr);

    // JS 对话框（alert / confirm / prompt / beforeunload）。
    // 关掉 WebView2 自带那个 —— 它的标题是"about:blank 显示"这种，放在桌面应用里很出戏。
    // **关掉的同时必须注册 ScriptDialogOpening**：不注册的话页面里这些调用会直接静默
    // 失效（alert 什么都不发生、confirm 永远 false），比弹默认框更难排查
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webview->get_Settings(&settings)) && settings)
        settings->put_AreDefaultScriptDialogsEnabled(win->defaultJsDialog ? TRUE : FALSE);
    if (!win->defaultJsDialog)
    {
        auto dialogCB = Callback<ICoreWebView2ScriptDialogOpeningEventHandler>(this, &Page::onScriptDialog);
        webview->add_ScriptDialogOpening(dialogCB.Get(), nullptr);
    }

    injectSdk(); // 必须在导航之前注册，之后创建的每个文档都会执行
    loadPage();
}

void Page::injectSdk()
{
	auto js = loadTextResource(L"webwin.js");
	if (js.empty())
	{
		// exe 里没有内嵌 SDK：忘了在 src/web 跑 npm run build:all，或资源没编进 exe。
		// 静默跳过最难排查（页面里只表现为 win 未定义），所以直接弹出来
		MessageBox(nullptr, L"exe 内未找到 SDK 资源 webwin.js", L"WebWin", MB_OK | MB_ICONERROR);
		return;
	}
	// 注意：Res.rc 里的资源名不要加引号。
	// 写成 "webwin.js" 时 RC 会把引号算进资源名，FindResource 按 webwin.js 查找会失败

	// SDK 据此判断"当前文档是不是应用自身的页面"
	JsonObject config;
	config.SetNamedValue(L"allowRemote", JsonValue::CreateBooleanValue(win->allowRemoteSdk));
	config.SetNamedValue(L"origin", JsonValue::CreateStringValue(win->getPageOrigin()));
	config.SetNamedValue(L"winId", JsonValue::CreateNumberValue(win->getId())); // 当前窗口 id
	// 窗口形态：SDK 据此决定要不要自动铺 resize 把手（无边框 + 可缩放才铺）
	config.SetNamedValue(L"frame", JsonValue::CreateBooleanValue(win->frame));
	config.SetNamedValue(L"resizable", JsonValue::CreateBooleanValue(win->resizable));
	// 应用基准目录（package.json 所在目录）：JS 的 path.resolve() / relative() 拿它当基准。
	// 框架里没有 cwd 概念（全库不调 GetCurrentDirectory），这里取代 Node 的 process.cwd()
	config.SetNamedValue(L"appPath", JsonValue::CreateStringValue(App::getPackageBaseDir().wstring()));
	auto script = L"window.__webwinConfig__=" + std::wstring{ config.Stringify().c_str() } + L";\n" + Util::utf8ToWide(js);
	webview->AddScriptToExecuteOnDocumentCreated(script.c_str(), nullptr);
}

void Page::loadPage()
{
	auto path = win->webviewPath;
	if (path.starts_with(L"http://") || path.starts_with(L"https://"))
	{
		webview->Navigate(path.c_str()); // path 是 url 地址，直接加载
		return;
	}

	// path 可以是目录（取其下 index.html），也可以直接指向某个 html 文件；
	// 相对路径一律以 package.json 所在目录为基准
	// （--path 启动时是 --path 那个目录，否则是 exe 同目录）
	auto target = std::filesystem::path{ path };
	if (target.is_relative()) target = App::getPackageBaseDir() / target;

	std::error_code ec;
	auto folder = target;
	auto page = std::wstring{ L"index.html" };
	if (!std::filesystem::is_directory(folder, ec))
	{
		folder = target.parent_path();
		page = target.filename().wstring();
	}

	// 目录映射为虚拟域名 https://{appId}/，页面地址即 https://{appId}/{page}
	auto appId = App::get()->getId(); // 未配置时为默认 ID，不会是空串
	if (win->webviewConfigured && std::filesystem::exists(folder / page, ec))
	{
		ComPtr<ICoreWebView2_3> webview3;
		webview.As(&webview3);
		webview3->SetVirtualHostNameToFolderMapping(appId.c_str(), folder.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
		webview->Navigate((L"https://" + appId + L"/" + page).c_str());
		return;
	}

	// 走到这里说明配置不可用：没读到 package.json / webwin / window / webview，
	// 或者 path 目录下没有 index.html —— 显示内置默认页面
	//（src/web/default.html 编译压缩后内嵌为 RCDATA 资源，见 Res.rc）
	auto html = Util::utf8ToWide(loadTextResource(L"default.html"));
	webview->NavigateToString(html.empty() ? L"<html><body>no config</body></html>" : html.c_str());
}

HWND Page::getHwnd() const
{
    return win->hwnd;
}

HRESULT Page::postSharedBuffer(ICoreWebView2SharedBuffer* buffer, const wchar_t* json, ICoreWebView2Frame2* frame)
{
    // 共享内存走独立通道：页面侧靠 sharedbufferreceived 事件收，不并入 { id, ok, data }
    if (frame)
    {
        ComPtr<ICoreWebView2Frame4> frame4;
        auto hr = frame->QueryInterface(IID_PPV_ARGS(&frame4));
        if (FAILED(hr)) return hr; // Runtime 太旧，不支持帧级共享内存
        return frame4->PostSharedBufferToScript(buffer, COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY, json);
    }
    ComPtr<ICoreWebView2_17> webview17;
    auto hr = webview.As(&webview17);
    if (FAILED(hr)) return hr; // Runtime 太旧，不支持共享内存
    return webview17->PostSharedBufferToScript(buffer, COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY, json);
}


void Page::emitEvent(const std::wstring& name, int winId, const JsonObject& payload, ICoreWebView2Frame2* frame)
{
    JsonObject msg;
    msg.SetNamedValue(L"name", JsonValue::CreateStringValue(name));
    // winId 小于 0 表示事件与窗口无关（os 域这类单例能力）：这时不下发 winId。
    // 页面侧订阅时用的也是"无 winId"，下发了反而对不上订阅表的键
    if (winId >= 0) msg.SetNamedValue(L"winId", JsonValue::CreateNumberValue(winId));
    // payload 本身就是一个 JSON 值，直接塞进去；
    // 不必先 Stringify 再 Parse —— 那是一次序列化 + 一次解析的白做功
    msg.SetNamedValue(L"payload", payload);
    auto msgStr = msg.Stringify();
    postJson(msgStr.c_str(), frame); // frame 非空说明是某个 iframe 订阅的
}

HRESULT Page::onMsgReceived(ICoreWebView2* webview, ICoreWebView2WebMessageReceivedEventArgs* args)
{
    PWSTR jsonRaw;
    auto hr = args->get_WebMessageAsJson(&jsonRaw);
    if (FAILED(hr)) return S_OK;
    handleMessage(jsonRaw, nullptr); // 顶级文档
    CoTaskMemFree(jsonRaw);
    return S_OK;
}

HRESULT Page::onFrameCreated(ICoreWebView2* sender, ICoreWebView2FrameCreatedEventArgs* args)
{
    ComPtr<ICoreWebView2Frame> frame;
    args->get_Frame(&frame);
    ComPtr<ICoreWebView2Frame2> frame2;
    frame.As(&frame2);
    if (!frame2) return S_OK; // 拿不到收发消息的接口（理论不会），放弃这个 frame

    // iframe 的消息不会走到 CoreWebView2::WebMessageReceived，必须按 frame 订阅
    auto msgCB = Callback<ICoreWebView2FrameWebMessageReceivedEventHandler>(this, &Page::onFrameMsg);
    frame2->add_WebMessageReceived(msgCB.Get(), nullptr);

    // frame 销毁时要把它的订阅摘掉，否则订阅表里会留下失效的 frame
    auto destroyedCB = Callback<ICoreWebView2FrameDestroyedEventHandler>(this, &Page::onFrameDestroyed);
    frame2->add_Destroyed(destroyedCB.Get(), nullptr);

    frames.push_back(frame2);
    return S_OK;
}

HRESULT Page::onFrameMsg(ICoreWebView2Frame* sender, ICoreWebView2WebMessageReceivedEventArgs* args)
{
    PWSTR jsonRaw;
    auto hr = args->get_WebMessageAsJson(&jsonRaw);
    if (FAILED(hr)) return S_OK;
    auto frame2 = asFrame2(sender);
    handleMessage(jsonRaw, frame2.Get());
    CoTaskMemFree(jsonRaw);
    return S_OK;
}

HRESULT Page::onFrameDestroyed(ICoreWebView2Frame* sender, IUnknown* args)
{
    auto frame2 = asFrame2(sender);
    Win::unsubscribeFrame(this, frame2.Get());
    Menu::removeByFrame(this, frame2.Get()); // 这个 iframe 建的菜单一起销毁
    Db::removeByFrame(this, frame2.Get());
    Process::removeByFrame(this, frame2.Get()); // 这个 iframe 启动的子进程一起结束
    std::erase_if(frames, [&](const ComPtr<ICoreWebView2Frame2>& item) { return item.Get() == frame2.Get(); });
    return S_OK;
}

void Page::handleMessage(const wchar_t* json, ICoreWebView2Frame2* frame)
{
    JsonObject param = JsonObject::Parse(json);

    // 协议见 arch/02-architecture.md：上行 { id, type, method, params, winId? }
    // type 是能力域，method 是该域下的具体动作
    auto type = Json::getString(param, L"type", L"");
    auto method = Json::getString(param, L"method", L"");
    auto params = Json::getObject(param, L"params");
    auto winId = Json::getInt(param, L"winId", -1); // -1 表示该消息与窗口无关

    JsonObject result;
    result.SetNamedValue(L"id", JsonValue::CreateNumberValue(Json::getInt(param, L"id", 0)));

    // 只按能力域分发：Page 认识域，具体方法交给各域自己处理。
    // 否则所有能力的方法都会堆在这一个文件里，加一个能力就膨胀一次
    bool sent = false; // 能力域已自行把响应发回（目前只有走共享内存的 screenshot）
    try
    {
        if (type == L"win") Win::handleApi(method, params, winId, this, result, frame);
        else if (type == L"os") Os::handleApi(method, params, this, result, frame);
        else if (type == L"menu") Menu::handleApi(method, params, this, result, frame);
        else if (type == L"app") App::handleApi(method, params, this, result, frame);
        else if (type == L"process") Process::handleApi(method, params, this, result, frame);
        else if (type == L"regedit") Regedit::handleApi(method, params, this, result, frame);
        else if (type == L"net") Net::handleApi(method, params, this, result, frame);
        else if (type == L"db") Db::handleApi(method, params, this, result, frame);
        else if (type == L"tray") Tray::handleApi(method, params, this, result, frame);
    // http 的请求跑在后台线程上，响应晚点到：handleApi 返回 true 时调用方不要再发
    else if (type == L"http") sent = Http::handleApi(method, params, this, result, frame);
    // fs 的文件操作同样跑在后台线程（网络盘 / 大文件会卡住 UI 线程），同 http 一条路
    else if (type == L"fs") sent = Fs::handleApi(method, params, this, result, frame);
    // res 域读写 exe 资源。写资源要重写整个 PE 文件，同样不能占着 UI 线程
    else if (type == L"res") sent = Res::handleApi(method, params, this, result, frame);
        else if (type == L"display") sent = Display::handleApi(method, params, this, result, frame);
        // 弹框不能当场执行（回调里开模态循环会被 Runtime 判重入），
        // 所以 handleApi 返回"是否已自行发回响应"，由它决定要不要再发一次信封
        else if (type == L"dialog") sent = Dialog::handleApi(method, params, this, result, frame);
        else {
            // 未知域回一个 error：前端 invoke 会 reject，而不是静默 resolve(undefined)。
            // 之前"原生侧改了却忘了重新编译 exe"就是被静默吞掉的，补上这条能直接暴露出来
            Json::setError(result, L"unknown type: " + type);
        }
    }
    // 兜底：整个 C++ 侧此前一个 catch 都没有，域处理器里抛出的异常会穿过
    // onMsgReceived（COM 回调）直接 std::terminate —— 整个进程崩掉。
    // 这里转成 error 响应发回去：JS 那边就是 promise reject，
    // 跟普通的"操作失败"一个待遇，进程照常活着
    catch (const winrt::hresult_error& e)
    {
        sent = false; // 共享内存那条路可能发了一半，信封还是得补上，否则调用方一直挂着
        Json::setError(result, L"C++ 抛出 WinRT 异常：" + std::wstring(e.message().c_str()));
    }
    catch (const std::exception& e)
    {
        sent = false;
        Json::setError(result, L"C++ 抛异常：" + Util::utf8ToWide(e.what()));
    }
    catch (...)
    {
        sent = false;
        Json::setError(result, L"C++ 抛出未知异常");
    }

    // 再发一次 JSON 信封会让页面那头同一次调用被 resolve 两遍，所以跳过
    if (sent) return;
    auto resultStr = result.Stringify();
    postJson(resultStr.c_str(), frame); // 结果送回发消息的那个文档
}

void Page::postJson(const wchar_t* json, ICoreWebView2Frame2* frame)
{
    // 顶级文档用 webview 发；iframe 必须发给它自己的 frame，
    // 发给 webview 的话 iframe 收不到（只会落到顶级文档）
    if (frame) frame->PostWebMessageAsJson(json);
    else webview->PostWebMessageAsJson(json);
}

void Page::send(const JsonObject& payload, ICoreWebView2Frame2* frame)
{
    postJson(payload.Stringify().c_str(), frame);
}

HRESULT Page::onCloseWindow(ICoreWebView2* sender, IUnknown* args)
{
    PostMessage(win->hwnd, WM_CLOSE, 0, 0);
    return S_OK;
}

HRESULT Page::onScriptDialog(ICoreWebView2* sender, ICoreWebView2ScriptDialogOpeningEventArgs* args)
{
    // 弹框不能在回调里当场跑（模态消息循环会被 Runtime 判重入，见 runScriptDialog 的注释）。
    // 取 deferral 把事件挂起，真正的弹框排到下一次消息循环去做
    ComPtr<ICoreWebView2Deferral> deferral;
    if (FAILED(args->GetDeferral(&deferral)) || !deferral) return S_OK;

    ScriptDialogTask task;
    if (FAILED(args->get_Kind(&task.kind))) task.kind = COREWEBVIEW2_SCRIPT_DIALOG_KIND_ALERT;

    // get_Message / get_DefaultText 给的都是 CoTaskMem 分配的串，必须自己释放
    LPWSTR msg = nullptr;
    if (SUCCEEDED(args->get_Message(&msg)) && msg)
    {
        task.message = msg;
        CoTaskMemFree(msg);
    }
    LPWSTR def = nullptr;
    if (SUCCEEDED(args->get_DefaultText(&def)) && def)
    {
        task.defaultText = def;
        CoTaskMemFree(def);
    }

    task.owner = getHwnd();
    task.args = args;          // ComPtr 会 AddRef，跨到下一次消息循环仍然有效
    task.deferral = deferral;
    pendingScriptDialogs.push_back(std::move(task));

    // 排不进去时当场收尾：App::runLater 返回 false 表示队列没初始化或已关，
    // 那条任务永远不会跑，而页面里那句 JS 会永远挂住、且没有任何报错
    if (!App::runLater([] { runScriptDialog(); }))
    {
        pendingScriptDialogs.back().deferral->Complete();
        pendingScriptDialogs.pop_back();
    }
    return S_OK;
}

HRESULT Page::onDomLoaded(ICoreWebView2* sender, ICoreWebView2DOMContentLoadedEventArgs* args)
{
    return S_OK;
}

HRESULT Page::onContextMenuRequested(ICoreWebView2* sender, ICoreWebView2ContextMenuRequestedEventArgs* args)
{
    if (win->menuId < 0) return S_OK; // 没挂菜单：走浏览器默认的右键菜单
    args->put_Handled(TRUE);          // 关掉默认的那个，改弹开发者的

    POINT pt;
    GetCursorPos(&pt); // 屏幕坐标：菜单就弹在鼠标处
    // 不在 WebView2 的事件回调里开模态循环（那里的嵌套循环 = 回调套回调）：
    // 排到下一次消息循环再弹（见 Menu::popup）
    App::runLater([menuId = win->menuId, hwnd = win->hwnd, pt] { Menu::popup(menuId, hwnd, pt.x, pt.y); });
    return S_OK;
}

HRESULT Page::onRequestPermission(ICoreWebView2* webview, ICoreWebView2PermissionRequestedEventArgs* args)
{
    args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
    return S_OK;
}


HRESULT Page::onRequest(ICoreWebView2* webview, ICoreWebView2WebResourceRequestedEventArgs* args)
{
    ComPtr<ICoreWebView2WebResourceRequest> request;
    args->get_Request(&request);
    LPWSTR rawUri = nullptr;
    request->get_Uri(&rawUri);
    std::wstring url(rawUri);
    CoTaskMemFree(rawUri);
    size_t queryPos = url.find(L'?');
    size_t end = (queryPos != std::wstring::npos) ? queryPos : url.length();
    std::wstring resName = url.substr(22, end - 22); //22是"https://app.localhost/"的长度
    // 光有域名（https://app.localhost/）或以 / 结尾的，都当要首页
    if (resName.empty() || resName.back() == L'/') resName += L"index.html";
    HRSRC hRes = FindResource(NULL, resName.data(), RT_RCDATA);
    if (!hRes) {
        return S_FALSE;
    }
    HGLOBAL hData = LoadResource(NULL, hRes);
    if (!hData) return S_OK;
    void* pData = LockResource(hData);
    DWORD size = SizeofResource(NULL, hRes);
    ComPtr<IStream> stream = SHCreateMemStream((const BYTE*)pData, size);
    auto ct = getContentType(resName);
    ComPtr<ICoreWebView2WebResourceResponse> response;
    Env::getEnv()->CreateWebResourceResponse(stream.Get(), 200, L"OK", ct.data(), &response);
    args->put_Response(response.Get());
    return S_OK;
}

std::wstring Page::getContentType(const std::wstring& fileName)
{
    static const std::unordered_map<std::string, std::wstring> mimeTypes = {
        {".html", L"Content-Type: text/html"},
        {".htm",  L"Content-Type: text/html"},
        {".js",   L"Content-Type: application/javascript"},
        {".css",  L"Content-Type: text/css"},
        {".json", L"Content-Type: application/json"},
        {".png",  L"Content-Type: image/png"},
        {".jpg",  L"Content-Type: image/jpeg"},
        {".jpeg", L"Content-Type: image/jpeg"},
        {".gif",  L"Content-Type: image/gif"},
        {".svg",  L"Content-Type: image/svg+xml"},
        {".ico",  L"Content-Type: image/x-icon"},
        {".woff", L"Content-Type: font/woff"},
        {".woff2",L"Content-Type: font/woff2"},
        {".ttf",  L"Content-Type: font/ttf"},
        {".eot",  L"Content-Type: application/vnd.ms-fontobject"},
        {".txt",  L"Content-Type: text/plain"},
        {".wasm", L"Content-Type: application/wasm"},
        {".mp3",  L"Content-Type: audio/mpeg"},
        {".mp4",  L"Content-Type: video/mp4"}
    };
    std::filesystem::path path(fileName);
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    auto it = mimeTypes.find(ext);
    if (it != mimeTypes.end()) {
        return it->second;
    }
    return L"Content-Type: application/octet-stream";
}
