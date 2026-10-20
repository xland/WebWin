#pragma once
#include <memory>
#include <wrl.h>
#include <string>
#include <Windows.h>
#include <WebView2.h>
#include <winrt/Windows.Data.Json.h>

#include "Page.h"

using namespace Microsoft::WRL;
using namespace winrt::Windows::Data::Json;
class Win{
	friend class Page;
public:
	// 订阅表里"与窗口无关"的事件用这个窗口 id（os 域这类单例能力）。
	// 页面里 winId 为 null 的消息，C++ 侧也是这个值
	static constexpr int NO_WINDOW = -1;
	enum class SizeMode { Normal, Maximize, FullScreen, FullDesktop };
	enum class PosMode { CenterScreen, Absolute };
	static Win* Create(const JsonObject& config);
	static Win* getFirst();
	static Win* getById(int id);
	int getId() const { return id; }
	// 事件订阅表：(窗口 id, 事件名) -> 订阅它的"文档"。
	// 一个文档 = 页面 + 页面里的某个 iframe；frame 为空表示页面的顶级文档。
	// 订阅后推，没人订阅就不发
	static void subscribeEvent(int winId, const std::wstring& name, Page* page, ICoreWebView2Frame2* frame = nullptr);
	static void unsubscribeEvent(int winId, const std::wstring& name, Page* page, ICoreWebView2Frame2* frame = nullptr);
	// iframe 销毁：把它在订阅表里的条目全部摘掉，否则会往一个失效的 frame 推事件
	static void unsubscribeFrame(Page* page, ICoreWebView2Frame2* frame);
	// 把窗口事件推给所有订阅者（可能跨页面：B 窗口的事件要送到 A 页面）
	static void fireEvent(int winId, const std::wstring& name, const JsonObject& payload);
	// 处理 type=win 域的消息：按 method 分发到本域的具体动作。
	// frame 是发消息的文档（iframe 或空=顶级文档），用于把结果送回原处
	static void handleApi(const std::wstring& method, const JsonObject& params, int winId, Page* page, JsonObject& result, ICoreWebView2Frame2* frame = nullptr);
	void createCtrl();
	~Win();
private:
	Win(const JsonObject& config);
	void readConfig(const JsonObject& config);
	std::wstring getPageOrigin();
	// 窗口几何变化后调用：让浏览器重算鼠标命中，清掉残留的 :hover
	void resetHoverState();
	void createNativeWindow();
	void applyDwmAttributes();
	std::wstring& getClsName();
	static LRESULT winProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	HRESULT onCtrlReady(HRESULT result, ICoreWebView2Controller* ctrl);
private:
	int id{ 0 };            // 窗口 id：消息的路由凭据
	static int lastId;
	HWND hwnd;
	ComPtr<ICoreWebView2Controller> ctrl;
	bool ctrlCreating{ false }; // 控制器是否已发起创建（避免重复创建）
	std::unique_ptr<Page> page;

	bool resizable{ true };
	SizeMode sizeMode{ SizeMode::Normal };
	int width{ 1000 };
	int height{ 800 };
	int minWidth{ 250 };
	int minHeight{ 200 };
	int maxWidth{ 0 };   // 0 表示未设置
	int maxHeight{ 0 };
	PosMode posMode{ PosMode::CenterScreen };
	int posX{ 0 };
	int posY{ 0 };
	bool visible{ true };
	bool frame{ true };
	bool shadow{ true };
	bool minimizable{ true };
	bool maximizable{ true };
	bool alwaysOnTop{ false };
	bool taskbar{ true };
	bool borderRadius{ true };
	bool animation{ true };
	bool transparent{ false };
	std::wstring title;          // 空表示使用网页中的 title
	std::wstring webviewPath{ L"." };
	bool allowWindowOpen{ true };
	bool allowRemoteSdk{ false }; // 是否允许把 SDK 注入到非应用自身来源的页面
	// 是否用 WebView2 自带的 JS 对话框（alert / confirm / prompt / beforeunload）。
	// false 时框架接管：关掉默认框，改弹 Windows 原生框（见 Page::onScriptDialog）
	bool defaultJsDialog{ false };
	bool webviewConfigured{ false }; // 是否读到了 webview 配置对象
	int menuId{ -1 };  // 窗口右键菜单的菜单 id；-1 = 没挂（走浏览器默认的右键菜单）
	// 是否正在系统原生的移动 / 缩放循环里（WM_ENTERSIZEMOVE ~ WM_EXITSIZEMOVE）。
	// 循环期间每帧都有 WM_SIZE，重活（重算 hover、通知 WebView 父窗口位移）必须让路
	bool inSizeMove{ false };
};

