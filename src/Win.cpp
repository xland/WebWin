#include <cwchar>
#include <map>
#include <memory>
#include <set>
#include <vector>
#include <dwmapi.h>

#include "Win.h"
#include "Env.h"
#include "Json.h"
#include "Menu.h"
#include "App.h"

namespace {
    std::vector<std::unique_ptr<Win>> winDic;

	// 订阅表里的一条：一个"文档"= 某个页面 + 页面里的某个 iframe。
	// frame 为空表示页面的顶级文档。iframe 也要能单独订阅，
	// 因为 C++ 往回推消息时必须发到具体的那个 frame（见 Page::postJson）
	struct EventSub
	{
		Page* page{ nullptr };
		ComPtr<ICoreWebView2Frame2> frame; // 空 = 顶级文档
		bool operator<(const EventSub& other) const
		{
			if (page != other.page) return page < other.page;
			return frame.Get() < other.frame.Get();
		}
	};
	// 同一个窗口的同一事件可以被多个文档同时订阅（A 页面、C 页面、以及某个 iframe）
	std::map<std::pair<int, std::wstring>, std::set<EventSub>> eventSubs;

	void removeWin(Win* win)
	{
		std::erase_if(winDic, [win](const std::unique_ptr<Win>& item) { return item.get() == win; });
	}

	// Chromium 的窗口类名：真正接收鼠标输入的是 Chrome_RenderWidgetHostHWND，
	// 外层是 Chrome_WidgetWin_0。WebView2 没有公开 API 拿这些句柄，只能按类名找，
	// 所以这里一律"尽力而为"——找不到就什么都不做，hover 残留只是观感问题，
	// 不能因为它让功能出错（将来 WebView2 换了窗口层级也只是退回今天的行为）
	bool isWebViewWindow(HWND hwnd)
	{
		wchar_t cls[64];
		if (!GetClassNameW(hwnd, cls, 64)) return false;
		return std::wcscmp(cls, L"Chrome_WidgetWin_0") == 0 || std::wcscmp(cls, L"Chrome_RenderWidgetHostHWND") == 0;
	}

	BOOL CALLBACK postMouseLeave(HWND hwnd, LPARAM found)
	{
		if (!isWebViewWindow(hwnd)) return TRUE;
		// 告诉浏览器"鼠标已经离开"：它会清掉 :hover 并派发一次 mouseleave，
		// 下次真实移动再重新命中。比 WM_MOUSEMOVE 省掉坐标换算与 DPI 处理。
		// EnumChildWindows 本身是递归的，外层和内层都会命中
		PostMessage(hwnd, WM_MOUSELEAVE, 0, 0);
		*reinterpret_cast<bool*>(found) = true;
		return TRUE;
	}
}

int Win::lastId = 0;


Win::Win(const JsonObject& config)
{
	id = ++lastId; // 自增整数，由 C++ 分配：跨页面、跨窗口都不会撞
	readConfig(config);
}

Win::~Win()
{
}

Win* Win::Create(const JsonObject& config) {
    auto win = std::unique_ptr<Win>(new Win(config));
	auto ptr = win.get();
    winDic.push_back(std::move(win)); 
	ptr->createNativeWindow();
	ptr->createCtrl(); // 环境未就绪时内部直接返回，等 Env::onEnvReady 补调
    return ptr;
}

void Win::readConfig(const JsonObject& config)
{
	resizable = Json::getBool(config, L"resizable", resizable);
	visible = Json::getBool(config, L"visible", visible);
	frame = Json::getBool(config, L"frame", frame);
	shadow = Json::getBool(config, L"shadow", shadow);
	minimizable = Json::getBool(config, L"minimizable", minimizable);
	maximizable = Json::getBool(config, L"maximizable", maximizable);
	alwaysOnTop = Json::getBool(config, L"alwaysOnTop", alwaysOnTop);
	taskbar = Json::getBool(config, L"taskbar", taskbar);
	borderRadius = Json::getBool(config, L"borderRadius", borderRadius);
	animation = Json::getBool(config, L"animation", animation);
	transparent = Json::getBool(config, L"transparent", transparent);

	auto sizeModeStr = Json::getString(config, L"size", L"");
	if (sizeModeStr == L"maximize") sizeMode = SizeMode::Maximize;
	else if (sizeModeStr == L"fullScreen") sizeMode = SizeMode::FullScreen;
	else if (sizeModeStr == L"fullDesktop") sizeMode = SizeMode::FullDesktop;

	auto size = Json::getObject(config, L"size");
	if (size)
	{
		width = Json::getInt(size, L"w", width);
		height = Json::getInt(size, L"h", height);
	}

	auto miniSize = Json::getObject(config, L"miniSize");
	if (miniSize)
	{
		minWidth = Json::getInt(miniSize, L"w", minWidth);
		minHeight = Json::getInt(miniSize, L"h", minHeight);
	}

	auto maxSize = Json::getObject(config, L"maxSize");
	if (maxSize)
	{
		maxWidth = Json::getInt(maxSize, L"w", maxWidth);
		maxHeight = Json::getInt(maxSize, L"h", maxHeight);
	}

	if (Json::getString(config, L"pos", L"centerScreen") == L"centerScreen") posMode = PosMode::CenterScreen;
	auto pos = Json::getObject(config, L"pos");
	if (pos)
	{
		posMode = PosMode::Absolute;
		posX = Json::getInt(pos, L"x", posX);
		posY = Json::getInt(pos, L"y", posY);
	}

	title = Json::getString(config, L"title", title);

	auto webview = Json::getObject(config, L"webview");
	if (webview)
	{
		webviewConfigured = true;
		webviewPath = Json::getString(webview, L"path", webviewPath);
		allowWindowOpen = Json::getBool(webview, L"allowWindowOpen", allowWindowOpen);
		allowRemoteSdk = Json::getBool(webview, L"allowRemoteSdk", allowRemoteSdk);
	}
}

std::wstring Win::getPageOrigin()
{
	// 页面自身的来源：目录映射为虚拟域名 https://{id}，URL 取其 origin
	// SDK 靠它判断"当前文档是不是应用自己的页面"
	//（URL 指向开发服务器时，取出来的就是 http://host:port，调试场景同样成立）
	auto schemeEnd = webviewPath.find(L"://");
	if (schemeEnd == std::wstring::npos) return L"https://" + App::get()->getId();
	auto hostStart = schemeEnd + 3;
	auto hostEnd = webviewPath.find(L'/', hostStart);
	if (hostEnd == std::wstring::npos) hostEnd = webviewPath.length();
	return webviewPath.substr(0, hostEnd);
}

void Win::createNativeWindow()
{
	auto clsName = getClsName();

	DWORD style = 0;
	DWORD exStyle = 0;
	if (frame)
	{
		style |= WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
		if (resizable) style |= WS_THICKFRAME;
		if (minimizable) style |= WS_MINIMIZEBOX;
		if (maximizable) style |= WS_MAXIMIZEBOX;
	}
	else
	{
		// 无边框窗口：非客户区在 WM_NCCALCSIZE 中裁掉；
		// 阴影由 applyDwmAttributes() 里的 DWM 调用负责
		style |= WS_POPUP;
		// transparent 未落地：与子窗口 HWND 集成方式存在冲突，见 03-tech-stack.md 的 Airspace 说明
	}
	exStyle |= taskbar ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW;
	if (alwaysOnTop) exStyle |= WS_EX_TOPMOST;

	int x = posX;
	int y = posY;
	int w = width;
	int h = height;
	MONITORINFO monitorInfo{ sizeof(MONITORINFO) };
	GetMonitorInfo(MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY), &monitorInfo);
	RECT screenRect = monitorInfo.rcMonitor;
	if (sizeMode == SizeMode::FullDesktop)
	{
		// 覆盖所有屏幕
		screenRect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
		screenRect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
		screenRect.right = screenRect.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
		screenRect.bottom = screenRect.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
	}
	if (sizeMode == SizeMode::FullScreen || sizeMode == SizeMode::FullDesktop)
	{
		// 覆盖任务栏：去边框并置顶
		style = WS_POPUP;
		exStyle |= WS_EX_TOPMOST;
		x = screenRect.left;
		y = screenRect.top;
		w = screenRect.right - screenRect.left;
		h = screenRect.bottom - screenRect.top;
	}
	else if (posMode == PosMode::CenterScreen)
	{
		x = screenRect.left + (screenRect.right - screenRect.left - w) / 2;
		y = screenRect.top + (screenRect.bottom - screenRect.top - h) / 2;
	}
	hwnd = CreateWindowEx(exStyle, clsName.data(), title.c_str(), style, x, y, w, h, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
	SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
	applyDwmAttributes();
}

void Win::applyDwmAttributes()
{
	if (!frame && shadow)
	{
		// 无边框窗口的阴影：把边框扩展进客户区 1px 让 DWM 参与绘制，
		// 再关掉系统非客户区绘制，只留下外阴影
		MARGINS margins = { 1, 1, 1, 1 };
		DwmExtendFrameIntoClientArea(hwnd, &margins);
		int value = 2; // DWMNCRP_DISABLED / TRUE
		DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &value, sizeof(value));
		DwmSetWindowAttribute(hwnd, DWMWA_ALLOW_NCPAINT, &value, sizeof(value));
	}

	// 圆角：true 时用系统圆角，false 时强制方角（Windows 11 起生效，低版本系统忽略）
	DWM_WINDOW_CORNER_PREFERENCE corner = borderRadius ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
	DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

	// 窗口动画（最小化 / 最大化的过渡效果）：false 时禁用
	BOOL disableTransition = animation ? FALSE : TRUE;
	DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disableTransition, sizeof(disableTransition));
}

void Win::createCtrl()
{
	if (ctrl || ctrlCreating) return;
	auto wvEnv = Env::getEnv();
	if (!wvEnv) return; // 环境尚未就绪，等 Env::onEnvReady 再调用
	ctrlCreating = true;
	auto ctrlReadyCB = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(this, &Win::onCtrlReady);
	wvEnv->CreateCoreWebView2Controller(hwnd, ctrlReadyCB.Get());

	if (sizeMode == SizeMode::Maximize) ShowWindow(hwnd, SW_MAXIMIZE);
	else if (visible) ShowWindow(hwnd, SW_SHOW);
}

Win* Win::getFirst()
{
	if (winDic.empty()) return nullptr;
	return winDic.front().get();
}

Win* Win::getById(int id)
{
	for (auto& win : winDic)
	{
		if (win->id == id) return win.get();
	}
	return nullptr; // 窗口已关闭
}

void Win::subscribeEvent(int winId, const std::wstring& name, Page* page, ICoreWebView2Frame2* frame)
{
	EventSub sub;
	sub.page = page;
	sub.frame = frame; // 为空 = 顶级文档
	eventSubs[{ winId, name }].insert(sub);
}

void Win::unsubscribeEvent(int winId, const std::wstring& name, Page* page, ICoreWebView2Frame2* frame)
{
	auto it = eventSubs.find({ winId, name });
	if (it == eventSubs.end()) return;
	EventSub sub;
	sub.page = page;
	sub.frame = frame;
	it->second.erase(sub);
	if (it->second.empty()) eventSubs.erase(it);
}

void Win::unsubscribeFrame(Page* page, ICoreWebView2Frame2* frame)
{
	// iframe 销毁后必须摘掉它的订阅：否则事件会推给一个已经不存在的 frame
	for (auto it = eventSubs.begin(); it != eventSubs.end();)
	{
		std::erase_if(it->second, [&](const EventSub& sub) {
			return sub.page == page && sub.frame.Get() == frame;
		});
		if (it->second.empty()) it = eventSubs.erase(it);
		else ++it;
	}
}

void Win::fireEvent(int winId, const std::wstring& name, const JsonObject& payload)
{
	auto it = eventSubs.find({ winId, name });
	if (it == eventSubs.end()) return; // 没人订阅：不发
	auto subscribers = it->second; // 拷贝：回调过程中订阅表可能变动
	for (auto& sub : subscribers) sub.page->emitEvent(name, winId, payload, sub.frame.Get());
}

void Win::handleApi(const std::wstring& method, const JsonObject& params, int winId, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	// 事件订阅：登记"哪个文档要听哪个窗口的哪个事件"，之后才推（订阅后推）
	if (method == L"on" || method == L"off")
	{
		auto name = Json::getString(params, L"name", L"");
		if (winId < 0 || name.empty()) { Json::setError(result, L"invalid event subscription"); return; }
		if (method == L"on") subscribeEvent(winId, name, page, frame);
		else unsubscribeEvent(winId, name, page, frame);
		Json::setOk(result);
		return;
	}

	// 新建窗口：配置复用 package.json 里 window 那套结构，返回新窗口的 id
	if (method == L"create")
	{
		auto created = Create(params ? params : JsonObject{});
		Json::setOk(result);
		result.SetNamedValue(L"data", JsonValue::CreateNumberValue(created->getId()));
		return;
	}

	// 其余方法都要落到具体窗口上：winId 是唯一的路由凭据。
	// 窗口已关闭时 getById 拿不到，直接返回错误字符串
	auto target = getById(winId);
	if (!target) { Json::setError(result, L"window not found: " + std::to_wstring(winId)); return; }
	auto hwnd = target->hwnd;

	// 改变窗口几何，并清掉残留的 :hover（见 resetHoverState）。
	// 清理时机分三种，目的都是"只清一次，不做重复劳动"：
	//   - minimize / hide：在**之前**清。窗口一旦隐藏，浏览器可能不再处理鼠标消息
	//   - show / move：在**之后**清。它们不改变尺寸，不会触发 WM_SIZE
	//   - maximize / restore / resize：**这里不写**。它们会同步触发 WM_SIZE，
	//     由 winProc 统一处理，这里再调一次就是白做一遍
	if (method == L"maximize") ShowWindow(hwnd, SW_MAXIMIZE);
	else if (method == L"minimize") { target->resetHoverState(); ShowWindow(hwnd, SW_MINIMIZE); }
	else if (method == L"restore") ShowWindow(hwnd, SW_RESTORE);
	else if (method == L"hide") { target->resetHoverState(); ShowWindow(hwnd, SW_HIDE); }
	else if (method == L"show") { ShowWindow(hwnd, SW_SHOW); target->resetHoverState(); }
	else if (method == L"close") PostMessage(hwnd, WM_CLOSE, 0, 0);     // 窗口没了，不用清
	else if (method == L"destroy") DestroyWindow(hwnd);                 // 同上
	else if (method == L"resize")
	{
		RECT rect;
		GetWindowRect(hwnd, &rect);
		SetWindowPos(hwnd, nullptr, rect.left, rect.top, Json::getInt(params, L"width", 0), Json::getInt(params, L"height", 0), SWP_NOZORDER | SWP_NOACTIVATE);
	}
	else if (method == L"move")
	{
		RECT rect;
		GetWindowRect(hwnd, &rect);
		SetWindowPos(hwnd, nullptr, Json::getInt(params, L"x", 0), Json::getInt(params, L"y", 0), rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
		target->resetHoverState();
	}
	else if (method == L"setContextMenu")
	{
		// 挂窗口右键菜单：传 menuId < 0 表示解绑（重新走浏览器默认的右键菜单）
		target->menuId = Json::getInt(params, L"menuId", -1);
	}
	else { Json::setError(result, L"unknown method: " + method); return; }
	Json::setOk(result);
}

void Win::resetHoverState()
{
	if (!hwnd || !ctrl) return;
	ctrl->NotifyParentWindowPositionChanged(); // 官方 API：父窗口几何变了要同步
	HWND hwndWebView = FindWindowEx(hwnd, nullptr, L"Chrome_WidgetWin_0", nullptr);
	PostMessage(hwndWebView, WM_MOUSEMOVE, 0, 0);
	PostMessage(hwndWebView, WM_MOUSELEAVE, 0, 0);
	HWND hwndInner = FindWindowEx(hwndWebView, nullptr, NULL, nullptr);
	PostMessage(hwndInner, WM_MOUSEMOVE, 0, 0);
	PostMessage(hwndInner, WM_MOUSELEAVE, 0, 0);
}

std::wstring& Win::getClsName()
{
    static std::wstring clsName = [] {
        WNDCLASSEXW wcex;
        wcex.cbSize = sizeof(WNDCLASSEX);
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = &Win::winProc;
        wcex.cbClsExtra = 0;
        wcex.cbWndExtra = 0;
        wcex.hInstance = GetModuleHandle(nullptr);
        wcex.hIcon = LoadIcon(wcex.hInstance, (LPCTSTR)IDI_WINLOGO);
        wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wcex.lpszMenuName = nullptr;
        wcex.lpszClassName = L"WebWin";
        wcex.hIconSm = LoadIcon(wcex.hInstance, (LPCTSTR)IDI_WINLOGO);
        RegisterClassExW(&wcex);
        return wcex.lpszClassName;
        }();
    return clsName;
}

LRESULT Win::winProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto winObj = reinterpret_cast<Win*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
	if (winObj == nullptr) return DefWindowProc(hwnd, msg, wParam, lParam);

	switch (msg)
	{
	case WM_SIZE:
	{
		RECT bounds;
		GetClientRect(hwnd, &bounds);
		if (winObj->ctrl) winObj->ctrl->put_Bounds(bounds);
		// 窗口大小变化：推给所有订阅者（可能是别的页面持有的这个窗口对象）
		JsonObject payload;
		payload.SetNamedValue(L"width", JsonValue::CreateNumberValue(bounds.right - bounds.left));
		payload.SetNamedValue(L"height", JsonValue::CreateNumberValue(bounds.bottom - bounds.top));
		Win::fireEvent(winObj->id, L"resize", payload);
		// 覆盖原生触发的几何变化：拖边缘、双击标题栏、Win+方向键分屏、最大化 / 还原
		// （鼠标没动，:hover 会留在原来的元素上）。也覆盖 handleApi 里的
		// maximize / restore / resize —— 它们同步触发本消息，那边就不必再清一次。
		// 最小化例外：WM_SYSCOMMAND 的 SC_MINIMIZE 更早且窗口尚未隐藏，那时已经清过
		if (wParam != SIZE_MINIMIZED) winObj->resetHoverState();
		return 0;
	}
	case WM_MOVE:
	{
		RECT rect;
		GetWindowRect(hwnd, &rect);
		JsonObject payload;
		payload.SetNamedValue(L"x", JsonValue::CreateNumberValue(rect.left));
		payload.SetNamedValue(L"y", JsonValue::CreateNumberValue(rect.top));
		Win::fireEvent(winObj->id, L"move", payload);
		return 0;
	}
	case WM_SYSCOMMAND:
	{
		// 原生最小化（标题栏按钮 / 系统菜单 / 任务栏点击）在这里比 WM_SIZE 更早到达，
		// 此时窗口还没隐藏，清 hover 才有效。不 return，交给 DefWindowProc 真正执行
		if ((wParam & 0xFFF0) == SC_MINIMIZE) winObj->resetHoverState();
		break;
	}
	case WM_GETMINMAXINFO:
	{
		auto mmi = reinterpret_cast<MINMAXINFO*>(lParam);
		if (winObj->minWidth > 0) mmi->ptMinTrackSize.x = winObj->minWidth;
		if (winObj->minHeight > 0) mmi->ptMinTrackSize.y = winObj->minHeight;
		if (winObj->maxWidth > 0) mmi->ptMaxTrackSize.x = winObj->maxWidth;
		if (winObj->maxHeight > 0) mmi->ptMaxTrackSize.y = winObj->maxHeight;
		return 0;
	}
	case WM_NCCALCSIZE:
	{
		// 无边框：让客户区等于整个窗口矩形，裁掉标题栏与边框。
		// 带 WS_CAPTION / WS_THICKFRAME 时 DWM 仍会在窗口外侧绘制阴影
		if (wParam == TRUE && !winObj->frame) return WVR_REDRAW;
		break;
	}
	case Menu::MSG_POPUP:
	{
		// 菜单在这里弹：窗口过程本身就是消息流，不是 WebView2 的事件回调，
		// 在这里开模态循环不会有"回调里套回调"的重入问题。
		// 发起方（右键回调 / popup 请求）都只是投了本消息，没有直接调 Menu::popup
		Menu::popup(static_cast<int>(wParam), hwnd,
			static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
		return 0;
	}
	case WM_DESTROY:
	{
		// 窗口没了：它作为被订阅对象的条目、以及它自己订阅的条目，全部清理，
		// 否则订阅表会留下悬挂的 Page* 与失效的窗口 id
		for (auto it = eventSubs.begin(); it != eventSubs.end();)
		{
			// 本页面的所有文档（顶级 + 它的全部 iframe）都要摘掉
			const auto page = winObj->page.get();
			std::erase_if(it->second, [page](const EventSub& sub) { return sub.page == page; });
			if (it->first.first == winObj->id || it->second.empty()) it = eventSubs.erase(it);
			else ++it;
		}
		Menu::removeByPage(winObj->page.get()); // 本页建的菜单一起销毁：点击已无处可推
		removeWin(winObj);
		if (App::get()->getQuitWhenNoWindow() && winDic.empty()) PostQuitMessage(0);
		return 0;
	}
	}

    return DefWindowProc(hwnd, msg, wParam, lParam);
}
HRESULT Win::onCtrlReady(HRESULT result, ICoreWebView2Controller* ctrl)
{
    this->ctrl = ctrl;
    ComPtr<ICoreWebView2> webview;
    ctrl->get_CoreWebView2(&webview);
    RECT bounds;
    GetClientRect(hwnd, &bounds);
    ctrl->put_Bounds(bounds);
    page = std::make_unique<Page>(this, webview);
    return S_OK;
}