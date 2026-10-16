#pragma once
#include <vector>
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
	friend class Menu; // Menu 要把菜单项点击推给创建它的文档
public:
	Page(Win* win, const ComPtr<ICoreWebView2>& webview);
	void emit(const JsonObject& eventData);
	/** 本页面所在窗口的句柄：弹菜单需要一个属主窗口 */
	HWND getHwnd() const;
private:
	// 页面里点右键：挂了菜单就关掉浏览器默认的那个，改弹我们的
	HRESULT onContextMenuRequested(ICoreWebView2* sender, ICoreWebView2ContextMenuRequestedEventArgs* args);
	HRESULT onRequest(ICoreWebView2* webview, ICoreWebView2WebResourceRequestedEventArgs* args);
	HRESULT onMsgReceived(ICoreWebView2* webview, ICoreWebView2WebMessageReceivedEventArgs* args);
	// iframe 相关：CoreWebView2 的 WebMessageReceived 只收顶级文档的消息，
	// iframe 必须逐个 frame 订阅才能收到，回消息也要发回那个 frame
	HRESULT onFrameCreated(ICoreWebView2* sender, ICoreWebView2FrameCreatedEventArgs* args);
	HRESULT onFrameMsg(ICoreWebView2Frame* sender, ICoreWebView2WebMessageReceivedEventArgs* args);
	HRESULT onFrameDestroyed(ICoreWebView2Frame* sender, IUnknown* args);
	HRESULT onDomLoaded(ICoreWebView2* sender, ICoreWebView2DOMContentLoadedEventArgs* args);
	HRESULT onRequestPermission(ICoreWebView2* webview, ICoreWebView2PermissionRequestedEventArgs* args);
	HRESULT onCloseWindow(ICoreWebView2* sender, IUnknown* args);
	void injectSdk();
	void loadPage();
	/** 把窗口事件推给本页面里的某个文档；frame 为空表示推给顶级文档 */
	void emitEvent(const std::wstring& name, int winId, const JsonObject& payload, ICoreWebView2Frame2* frame = nullptr);
	std::wstring getContentType(const std::wstring& fileName);
	/** 处理一条上行消息。frame 是发消息的文档：iframe 传它自己，顶级文档传 nullptr */
	void handleMessage(const wchar_t* json, ICoreWebView2Frame2* frame);
	/** 下行发消息：有 frame 就只发给那个 iframe，否则发给顶级文档 */
	void postJson(const wchar_t* json, ICoreWebView2Frame2* frame);
private:
	Win* win;
	ComPtr<ICoreWebView2> webview;
	// 本页面里的 iframe。Frame2 既能收消息也能发消息；顶级文档不在其中
	std::vector<ComPtr<ICoreWebView2Frame2>> frames;
};

