#include <shlobj.h>
#include <shlwapi.h>
#include <filesystem>
#include <fstream>

#include "App.h"
#include "Display.h"
#include "Env.h"
#include "Menu.h"
#include "Os.h"
#include "Win.h"
#include "Page.h"
#include "Json.h"
#include "Util.h"

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

void Page::emit(const JsonObject& eventData)
{
    auto eventDataStr = eventData.Stringify(); // hstring：直接拿它发，不必再拷一份 std::wstring
    webview->PostWebMessageAsJson(eventDataStr.c_str());
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
    if (type == L"win") Win::handleApi(method, params, winId, this, result, frame);
    else if (type == L"os") Os::handleApi(method, params, this, result, frame);
    else if (type == L"menu") Menu::handleApi(method, params, this, result, frame);
    else if (type == L"app") App::handleApi(method, params, this, result, frame);
    else if (type == L"display") sent = Display::handleApi(method, params, this, result, frame);
    else {
        // 未知域回一个 error：前端 invoke 会 reject，而不是静默 resolve(undefined)。
        // 之前"原生侧改了却忘了重新编译 exe"就是被静默吞掉的，补上这条能直接暴露出来
        Json::setError(result, L"unknown type: " + type);
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

HRESULT Page::onCloseWindow(ICoreWebView2* sender, IUnknown* args)
{
    PostMessage(win->hwnd, WM_CLOSE, 0, 0);
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
    // 不在 WebView2 的事件回调里弹菜单（那里开模态循环等于"回调套回调"）：
    // 只投一条消息，真正的弹出由窗口过程完成（见 Menu::popup）
    PostMessage(win->hwnd, Menu::MSG_POPUP, win->menuId,
        MAKELPARAM(static_cast<WORD>(static_cast<short>(pt.x)), static_cast<WORD>(static_cast<short>(pt.y))));
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
    std::wstring resName = url.substr(22, end - 22); //22是“https://app.localhost/”的长度
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
