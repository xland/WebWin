#pragma once
#include <WebView2.h>
#include <winrt/Windows.Data.Json.h>

using namespace winrt::Windows::Data::Json;

class Win;

class Page
{
public:
	Page(Win* win, const ComPtr<ICoreWebView2>& webview);
	void emit(const JsonObject& eventData);
private:
	HRESULT onRequest(ICoreWebView2* webview, ICoreWebView2WebResourceRequestedEventArgs* args);
	HRESULT onMsgReceived(ICoreWebView2* webview, ICoreWebView2WebMessageReceivedEventArgs* args);
	HRESULT onDomLoaded(ICoreWebView2* sender, ICoreWebView2DOMContentLoadedEventArgs* args);
	HRESULT onRequestPermission(ICoreWebView2* webview, ICoreWebView2PermissionRequestedEventArgs* args);
	HRESULT onCloseWindow(ICoreWebView2* sender, IUnknown* args);
	std::wstring getContentType(const std::wstring& fileName);
private:
	Win* win;
	ComPtr<ICoreWebView2> webview;
};

