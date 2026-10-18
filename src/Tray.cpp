#include "Tray.h"

#include <cwchar>
#include <map>
#include <memory>
#include <string>

#include "Json.h"
#include "Menu.h"
#include "Win.h"

namespace
{
	std::map<int, std::unique_ptr<Tray>> trays;
	int lastId = 0;

	// Shell_NotifyIcon 的回调消息。WM_APP 以上是应用私有的，不会被系统占用
	constexpr UINT WM_TRAY_CALLBACK = WM_APP + 1;
	// 单击延迟派发的定时器：等双击窗口过去再发 click，否则双击会连带出一次单击
	constexpr UINT_PTR TIMER_CLICK = 1;

	constexpr wchar_t clsName[] = L"WebWinTrayWindow";
	HWND trayWindow{ nullptr };

	// 托盘图标回调不带坐标，也不带"这是不是双击"的标记，所以单击要压后派发，
	// 全靠下面这两个状态消歧。按 id 记，多个托盘各点各的不会互相干扰
	int pendingClickId{ 0 };   // 等着发 click 的那个托盘；0 = 没有待发
	int swallowUpId{ 0 };      // 双击后面还跟着一次 UP，这个 id 的那次要吃掉

	// explorer.exe 崩掉或重启后会广播这条：之前加的图标全被清了，得重新加一遍，
	// 否则托盘图标会永久消失
	UINT msgTaskbarCreated{ 0 };

	constexpr wchar_t EVENT_CLICK[] = L"click";
	constexpr wchar_t EVENT_RIGHT_CLICK[] = L"rightClick";
	constexpr wchar_t EVENT_DOUBLE_CLICK[] = L"doubleClick";

	/**
	 * 托盘图标一律用 **exe 自己的图标**：资源 100，见 src/Res.rc 的 `100 ICON`。
	 *
	 * 这个 100 是**约定**，RC 里改了这里就得跟着改，所以两边都把出处写清楚。
	 * v1 不支持自定义图标（见 arch/99-todo.md）。
	 */
	HICON loadAppIcon()
	{
		return reinterpret_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
			MAKEINTRESOURCEW(100), IMAGE_ICON,
			GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
			LR_DEFAULTCOLOR));
	}

	LRESULT CALLBACK trayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam); // 定义在下面

	/**
	 * 收 Shell_NotifyIcon 回调的隐藏窗口，懒建、随进程存活。
	 *
	 * 必须是**顶层**窗口而不是 os 域那种 HWND_MESSAGE：托盘菜单要靠
	 * SetForegroundWindow 才能在点菜单外时正常收起，而消息窗口当不了前台窗口。
	 * WS_EX_TOOLWINDOW 让它既不进任务栏也不进 Alt+Tab。
	 */
	bool ensureTrayWindow()
	{
		if (trayWindow) return true;
		WNDCLASSW cls{};
		cls.lpfnWndProc = trayProc;
		cls.hInstance = GetModuleHandleW(nullptr);
		cls.lpszClassName = clsName;
		if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
		trayWindow = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, clsName, nullptr,
			WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, cls.hInstance, nullptr);
		if (!trayWindow) return false;
		msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
		return true;
	}

	LRESULT CALLBACK trayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (msgTaskbarCreated && msg == msgTaskbarCreated)
		{
			for (auto& pair : trays) pair.second->reAdd();
			return 0;
		}

		// 双击窗口过去了还没等到 DBLCLK：这次单击是真的，补发
		if (msg == WM_TIMER && wParam == TIMER_CLICK)
		{
			KillTimer(hwnd, TIMER_CLICK);
			auto id = pendingClickId;
			pendingClickId = 0;
			auto it = trays.find(id);
			if (it != trays.end()) it->second->fire(EVENT_CLICK);
			return 0;
		}

		if (msg != WM_TRAY_CALLBACK) return DefWindowProc(hwnd, msg, wParam, lParam);

		// NOTIFYICONDATA 没设 uVersion（即 NOTIFYICON_VERSION 之前的默认版本）：
		// wParam 是 uID，lParam 是鼠标消息
		auto id = static_cast<int>(wParam);
		auto it = trays.find(id);
		if (it == trays.end()) return 0;
		auto tray = it->second.get();

		if (lParam == WM_LBUTTONUP)
		{
			// 双击的消息序列是 UP → DBLCLK → UP，所以单击不能当场发：
			// 挂个定时器压后，期间来了 DBLCLK 就把这次单击吃掉。
			// swallowUpId 是吃掉双击**后面**那次多出来的 UP
			if (swallowUpId == id) { swallowUpId = 0; return 0; }
			pendingClickId = id;
			// 用系统自己的双击间隔（默认 500ms），不要写死一个值：
			// 用户在控制面板里把阈值调大之后，写死的值会小于系统阈值，
			// 那样"慢速双击"会既发 click 又发 doubleClick
			SetTimer(hwnd, TIMER_CLICK, GetDoubleClickTime(), nullptr);
			return 0;
		}

		if (lParam == WM_LBUTTONDBLCLK)
		{
			if (pendingClickId == id) { KillTimer(hwnd, TIMER_CLICK); pendingClickId = 0; }
			swallowUpId = id;
			tray->fire(EVENT_DOUBLE_CLICK);
			return 0;
		}

		if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
		{
			// 两个都给：事件照发，有菜单也照弹。调用方想只听事件就不传菜单，
			// 想只弹菜单就不订阅 —— 不替它在本域里做这个取舍
			tray->fire(EVENT_RIGHT_CLICK);
			tray->popupMenu(hwnd);
			return 0;
		}
		return 0;
	}
}

Tray::Tray()
{
}

Tray::~Tray()
{
	// cbSize 是快到 NIM_ADD 时才填的：建到一半就失败的实例（比如图标没加载出来）
	// 不该拿一个没填好的 NOTIFYICONDATA 去调 Shell_NotifyIcon
	if (nid.cbSize) Shell_NotifyIconW(NIM_DELETE, &nid);
	if (nid.hIcon) DestroyIcon(nid.hIcon);
}

void Tray::handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	// 建图标这一步比较长（造实例 + 填 NOTIFYICONDATA + NIM_ADD + 入注册表），
	// 抽成 Tray::create，让 handleApi 保持"只是个分发"
	if (method == L"create")
	{
		Tray::create(params, result);
		return;
	}

	auto id = Json::getInt(params, L"id", -1);
	auto it = trays.find(id);
	if (it == trays.end()) { Json::setError(result, L"tray not found: " + std::to_wstring(id)); return; }
	auto tray = it->second.get();

	if (method == L"setToolTip")
	{
		if (!tray->setTip(Json::getString(params, L"text", L"")))
		{
			Json::setError(result, L"修改托盘提示失败");
			return;
		}
		Json::setOk(result);
		return;
	}

	if (method == L"destroy")
	{
		trays.erase(it); // 析构里 NIM_DELETE
		Json::setOk(result);
		return;
	}

	// 事件订阅：托盘是与窗口无关的能力，挂在 NO_WINDOW 下（同 os / display）
	if (method == L"on" || method == L"off")
	{
		auto name = Json::getString(params, L"name", L"");
		if (name != EVENT_CLICK && name != EVENT_RIGHT_CLICK && name != EVENT_DOUBLE_CLICK)
		{
			Json::setError(result, L"unknown event: " + name);
			return;
		}
		if (method == L"on") Win::subscribeEvent(Win::NO_WINDOW, name, page, frame);
		else Win::unsubscribeEvent(Win::NO_WINDOW, name, page, frame);
		Json::setOk(result);
		return;
	}

	Json::setError(result, L"unknown method: " + method);
}

void Tray::create(const JsonObject& params, JsonObject& result)
{
	if (!ensureTrayWindow()) { Json::setError(result, L"创建托盘窗口失败"); return; }

	auto tray = std::unique_ptr<Tray>(new Tray());
	tray->id = ++lastId;
	tray->menuId = Json::getInt(params, L"menuId", -1);
	tray->nid.hIcon = loadAppIcon();
	if (!tray->nid.hIcon) { Json::setError(result, L"加载托盘图标失败（资源 100）"); return; }

	tray->tooltip = Json::getString(params, L"tooltip", L"");
	tray->fillTip();
	tray->nid.cbSize = sizeof(tray->nid);
	tray->nid.hWnd = trayWindow;
	tray->nid.uID = static_cast<UINT>(tray->id);
	tray->nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
	tray->nid.uCallbackMessage = WM_TRAY_CALLBACK;
	if (!Shell_NotifyIconW(NIM_ADD, &tray->nid))
	{
		Json::setError(result, L"添加托盘图标失败");
		return;
	}

	auto id = tray->id;
	trays.emplace(id, std::move(tray));
	Json::setOk(result);
	result.SetNamedValue(L"data", JsonValue::CreateNumberValue(id));
}

bool Tray::hasInstance()
{
	return !trays.empty();
}

void Tray::fire(const wchar_t* name)
{
	JsonObject payload;
	Win::fireEvent(Win::NO_WINDOW, name, payload);
}

void Tray::popupMenu(HWND hwnd)
{
	if (menuId < 0) return;
	POINT pt;
	GetCursorPos(&pt); // 托盘回调里不带坐标，只能取当前鼠标位置

	// 托盘菜单必须先 SetForegroundWindow，否则点菜单外的地方它不消失。
	// 这里**不用** App::runLater：本调用点在隐藏窗口的窗口过程里，是消息泵
	// 派发下来的，不在 WebView2 的事件回调栈上，不存在"回调套回调"的重入
	SetForegroundWindow(hwnd);
	Menu::popup(menuId, hwnd, pt.x, pt.y);
	// 收尾：让菜单窗口彻底退掉，焦点还回来
	PostMessage(hwnd, WM_NULL, 0, 0);
}

void Tray::reAdd()
{
	nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
	Shell_NotifyIconW(NIM_ADD, &nid);
}

void Tray::fillTip()
{
	// szTip 只有 128 个 WCHAR：超了必须自己截，不然 wcsncpy_s 会失败（整串留空）
	constexpr size_t cap = sizeof(nid.szTip) / sizeof(nid.szTip[0]);
	auto n = tooltip.size();
	if (n > cap - 1) n = cap - 1;
	wcsncpy_s(nid.szTip, cap, tooltip.c_str(), n);
	nid.szTip[n] = L'\0';
}

bool Tray::setTip(const std::wstring& text)
{
	tooltip = text;
	fillTip();
	// 只改提示：uFlags 只带 NIF_TIP，改完复位成"图标 + 回调消息 + 提示"那一套
	nid.uFlags = NIF_TIP;
	auto ok = Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
	nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
	return ok;
}
