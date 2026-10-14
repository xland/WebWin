#include <shlobj.h>
#include <shlwapi.h>
#include <filesystem>
#include <fstream>

#include "App.h"
#include "Env.h"
#include "Win.h"
#include "Page.h"



Page::Page(Win* win, const ComPtr<ICoreWebView2>& webview) :win{ win }, webview{ webview }
{
    webview->AddWebResourceRequestedFilter(L"https://app.localhost/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
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

#ifdef _DEBUG
	// 调试：用 vite 开发服务器，改前端不必重新编译 exe
	webview->Navigate(L"http://localhost:5173");
#else
	// 发布：前端产物编进了 exe 资源（见 Resource.rc 里那块 dist 清单），走虚拟域名——
	// 请求由 onRequest 从资源里应答，一个 exe 就能独立跑，不依赖本机任何文件
	webview->Navigate(L"https://app.localhost/index.html");
#endif
}

void Page::emit(const JsonObject& eventData)
{
    std::wstring eventDataStr{ eventData.Stringify() };
    webview->PostWebMessageAsJson(eventDataStr.data());
}


HRESULT Page::onMsgReceived(ICoreWebView2* webview, ICoreWebView2WebMessageReceivedEventArgs* args)
{
    PWSTR jsonRaw;
    auto hr = args->get_WebMessageAsJson(&jsonRaw);
    if (FAILED(hr)) return S_OK;
    JsonObject param = JsonObject::Parse(jsonRaw);
    CoTaskMemFree(jsonRaw);
    auto method = param.GetNamedString(L"method");
    JsonObject result;
    result.SetNamedValue(L"id", JsonValue::CreateStringValue(param.GetNamedString(L"id")));
    if (method == L"showWindow") {
    }
    else {
        // 未知方法回一个 error：前端 Msg.invoke 会 reject，而不是静默 resolve(undefined)。
        // 之前"原生侧改了却忘了重新编译 exe"就是被静默吞掉的，补上这条能直接暴露出来
        std::wstring message = L"unknown method: " + std::wstring(method.c_str());
        result.SetNamedValue(L"error", JsonValue::CreateStringValue(message));
    }
    auto resultStr = result.Stringify();
    webview->PostWebMessageAsJson(resultStr.data());
    return S_OK;
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
