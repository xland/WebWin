#include <memory>
#include <vector>
#include <dwmapi.h>

#include "Win.h"
#include "Env.h"
#include "Json.h"
#include "App.h"

namespace {
    std::vector<std::unique_ptr<Win>> winDic;

	void removeWin(Win* win)
	{
		std::erase_if(winDic, [win](const std::unique_ptr<Win>& item) { return item.get() == win; });
	}
}


Win::Win(const JsonObject& config)
{
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
	}
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
		if (winObj->ctrl)
		{
			RECT bounds;
			GetClientRect(hwnd, &bounds);
			winObj->ctrl->put_Bounds(bounds);
		}
		return 0;
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
	case WM_DESTROY:
	{
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