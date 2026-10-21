#pragma once
#include <vector>
#include <map>
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
	/**
	 * 下行发消息：有 frame 就只发给那个 iframe，否则发给顶级文档。
	 *
	 * 能力域用它把事件 / 异步响应推回页面：menu 项被点击后回调、
	 * dialog 弹完之后回包，都走这里。放在公开区而不是给每个域加 friend ——
	 * 以后新域要下行发消息时，不必再回来改本文件的声明
	 */
	void send(const JsonObject& payload, ICoreWebView2Frame2* frame = nullptr);
	/**
	 * 用共享内存把大块数据发回某个文档（见 arch/07-web.md 第八节）。
	 *
	 * json 是随 buffer 一起带的附加数据（页面靠它把 buffer 关联回某次调用）；
	 * frame 为空表示发给顶级文档。返回 PostSharedBufferToScript 的 HRESULT
	 * —— 不做布尔化：共享内存这条链路上的失败原因（Runtime 太旧、buffer 状态不对 …）
	 * 光看 true/false 定位不出来，交给调用方带进错误信息里
	 */
	HRESULT postSharedBuffer(ICoreWebView2SharedBuffer* buffer, const wchar_t* json, ICoreWebView2Frame2* frame = nullptr);
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
	// 页面里弹 JS 对话框（alert / confirm / prompt / beforeunload）。
	// 只在 webview.defaultJsDialog 为 false 时才注册 —— 为 true 时走 WebView2 自带那套
	HRESULT onScriptDialog(ICoreWebView2* sender, ICoreWebView2ScriptDialogOpeningEventArgs* args);
	void injectSdk();
	void loadPage();
	/** 把窗口事件推给本页面里的某个文档；frame 为空表示推给顶级文档 */
	void emitEvent(const std::wstring& name, int winId, const JsonObject& payload, ICoreWebView2Frame2* frame = nullptr);
	std::wstring getContentType(const std::wstring& fileName);
	/** 校验消息来源：应用自身 origin、或 allowRemoteSdk 配置的远程域名放行；结果按 origin 缓存 */
	bool isOriginAuthorized(const std::wstring& sourceUrl);
	/** 处理一条上行消息。frame 是发消息的文档：iframe 传它自己，顶级文档传 nullptr */
	void handleMessage(const wchar_t* json, ICoreWebView2Frame2* frame);
	/** 下行发消息：有 frame 就只发给那个 iframe，否则发给顶级文档 */
	void postJson(const wchar_t* json, ICoreWebView2Frame2* frame);
private:
	std::wstring appOrigin_;                         // 应用自身 origin（构造时取），来源校验基准
	std::map<std::wstring, bool> originAuthCache_;   // origin -> 是否授权（同源消息复用判定结果）
	Win* win;
	ComPtr<ICoreWebView2> webview;
	// 本页面里的 iframe。Frame2 既能收消息也能发消息；顶级文档不在其中
	std::vector<ComPtr<ICoreWebView2Frame2>> frames;
};

