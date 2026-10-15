#pragma once
#include <wrl.h>
#include <string>
#include <WebView2.h>
#include <winrt/Windows.Data.Json.h>

using namespace Microsoft::WRL;
using namespace winrt::Windows::Data::Json;

class Win;

class Page
{
	friend class Win; // Win::fireEvent 需要把事件推给订阅页面
public:
	Page(Win* win, const ComPtr<ICoreWebView2>& webview);
	void emit(const JsonObject& eventData);
private:
	HRESULT onRequest(ICoreWebView2* webview, ICoreWebView2WebResourceRequestedEventArgs* args);
	HRESULT onMsgReceived(ICoreWebView2* webview, ICoreWebView2WebMessageReceivedEventArgs* args);
	HRESULT onDomLoaded(ICoreWebView2* sender, ICoreWebView2DOMContentLoadedEventArgs* args);
	HRESULT onRequestPermission(ICoreWebView2* webview, ICoreWebView2PermissionRequestedEventArgs* args);
	HRESULT onCloseWindow(ICoreWebView2* sender, IUnknown* args);
	void injectSdk();
	void loadPage();
	/** 把窗口事件推给本页面；事件由订阅表决定发给谁（见 Win::fireEvent） */
	void emitEvent(const std::wstring& name, int winId, const JsonObject& payload);
	std::wstring getContentType(const std::wstring& fileName);
private:
	Win* win;
	ComPtr<ICoreWebView2> webview;
};

