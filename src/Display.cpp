#include <cstring>
#include <cwchar>
#include <vector>

// JsonArray::Append 是 IVector 的方法，要显式带上集合头文件（Json.cpp 同理）
#include <winrt/Windows.Foundation.Collections.h>

// GetDpiForMonitor 在 shellscalingapi.h（shcore.lib），PNG 编码用 gdiplus（gdiplus.lib）
#include <shellscalingapi.h>
#include <gdiplus.h>

#include "Display.h"
#include "Env.h"
#include "Json.h"
#include "Win.h"

namespace {
	// 本域支持的事件：只做插拔两块，DPI 变化暂不实现（见 arch/99-todo.md T012）
	constexpr wchar_t EVENT_ADDED[] = L"display-added";
	constexpr wchar_t EVENT_REMOVED[] = L"display-removed";

	/** 一块显示器在某次枚举时的快照；add / remove 事件靠两次快照比对得出 */
	struct DisplayInfo
	{
		std::wstring id;      // 设备名，形如 \\.\DISPLAY1
		RECT monitor{};       // 屏幕矩形（物理像素）
		RECT work{};          // 工作区矩形：屏幕矩形扣掉任务栏等
		bool primary{ false };
		int scaleFactor{ 100 }; // 缩放百分比：100 / 125 / 150 ...
	};

	HWND displayWindow{ nullptr };      // 接显示器变化通知用的隐藏窗口
	std::vector<DisplayInfo> snapshot;  // 上一次枚举的结果，用于 diff

	/** HRESULT 转成 0xXXXXXXXX，出错时能直接看到原因 */
	std::wstring hexOf(HRESULT hr)
	{
		wchar_t buf[16];
		swprintf_s(buf, L"0x%08X", static_cast<unsigned int>(hr));
		return buf;
	}

	/** COLORREF 是 0x00BBGGRR，转成 CSS 用的 #RRGGBB */
	std::wstring colorToHex(COLORREF color)
	{
		wchar_t buf[8];
		swprintf_s(buf, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
		return buf;
	}

	/** 读一块显示器的信息。GetDpiForMonitor 拿不到时按 96 DPI（100%）处理 */
	bool infoOf(HMONITOR hMonitor, DisplayInfo& out)
	{
		MONITORINFOEX mi{};
		mi.cbSize = sizeof(mi);
		if (!GetMonitorInfo(hMonitor, &mi)) return false;

		out.id = mi.szDevice;
		out.monitor = mi.rcMonitor;
		out.work = mi.rcWork;
		out.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

		UINT dpiX = 0, dpiY = 0;
		if (FAILED(GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) || dpiX == 0) dpiX = 96;
		out.scaleFactor = MulDiv(static_cast<int>(dpiX), 100, 96);
		return true;
	}

	BOOL CALLBACK enumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData)
	{
		DisplayInfo info;
		if (infoOf(hMonitor, info)) reinterpret_cast<std::vector<DisplayInfo>*>(dwData)->push_back(std::move(info));
		return TRUE; // 继续枚举下一块
	}

	std::vector<DisplayInfo> enumDisplays()
	{
		std::vector<DisplayInfo> list;
		EnumDisplayMonitors(nullptr, nullptr, enumProc, reinterpret_cast<LPARAM>(&list));
		return list;
	}

	JsonObject rectToJson(const RECT& r)
	{
		JsonObject obj;
		obj.SetNamedValue(L"x", JsonValue::CreateNumberValue(r.left));
		obj.SetNamedValue(L"y", JsonValue::CreateNumberValue(r.top));
		obj.SetNamedValue(L"width", JsonValue::CreateNumberValue(r.right - r.left));
		obj.SetNamedValue(L"height", JsonValue::CreateNumberValue(r.bottom - r.top));
		return obj;
	}

	JsonObject toJson(const DisplayInfo& d)
	{
		JsonObject obj;
		obj.SetNamedValue(L"id", JsonValue::CreateStringValue(d.id));
		obj.SetNamedValue(L"primary", JsonValue::CreateBooleanValue(d.primary));
		obj.SetNamedValue(L"scaleFactor", JsonValue::CreateNumberValue(d.scaleFactor));
		obj.SetNamedValue(L"bounds", rectToJson(d.monitor));
		obj.SetNamedValue(L"workArea", rectToJson(d.work));
		return obj;
	}

	const DisplayInfo* findById(const std::vector<DisplayInfo>& list, const std::wstring& id)
	{
		for (auto& item : list) if (item.id == id) return &item;
		return nullptr;
	}

	/** 两个矩形的重叠面积；不相交为 0。用 long long 是怕 4K 双屏相乘溢出 int */
	long long overlapArea(const RECT& a, const RECT& b)
	{
		auto left = a.left > b.left ? a.left : b.left;
		auto top = a.top > b.top ? a.top : b.top;
		auto right = a.right < b.right ? a.right : b.right;
		auto bottom = a.bottom < b.bottom ? a.bottom : b.bottom;
		if (right <= left || bottom <= top) return 0;
		return static_cast<long long>(right - left) * (bottom - top);
	}

	/** 矩形中心与本块屏幕中心的距离平方，只用来比大小，不必开根 */
	double centerDistance(const RECT& r, const DisplayInfo& d)
	{
		auto cx = (r.left + r.right) / 2.0;
		auto cy = (r.top + r.bottom) / 2.0;
		auto mx = (d.monitor.left + d.monitor.right) / 2.0;
		auto my = (d.monitor.top + d.monitor.bottom) / 2.0;
		return (mx - cx) * (mx - cx) + (my - cy) * (my - cy);
	}

	/**
	 * 重新枚举，和上一份快照比对后推事件。
	 * Windows 没有"显示器插拔"的专用消息：分辨率变化、排列变化、插拔都会广播
	 * WM_DISPLAYCHANGE（改缩放/主题还会带 WM_SETTINGCHANGE），所以统一在这里 diff。
	 * 没有实际增删时一个事件都不推 —— 同一条消息来多次也不会重复推。
	 */
	void checkDisplayChange()
	{
		auto now = enumDisplays();
		for (auto& item : now)
		{
			if (findById(snapshot, item.id)) continue;
			Win::fireEvent(Win::NO_WINDOW, EVENT_ADDED, toJson(item));
		}
		for (auto& item : snapshot)
		{
			// 已拔掉的屏幕拿不到新信息，只能把最后一次看到的快照原样推给页面
			if (findById(now, item.id)) continue;
			Win::fireEvent(Win::NO_WINDOW, EVENT_REMOVED, toJson(item));
		}
		snapshot = std::move(now);
	}

	LRESULT CALLBACK displayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (msg == WM_DISPLAYCHANGE || msg == WM_SETTINGCHANGE)
		{
			checkDisplayChange();
			return 0;
		}
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}

	/**
	 * 显示器变化是**广播消息**，而广播只发给顶层窗口 —— os 域那种 HWND_MESSAGE
	 * 消息窗口收不到，所以这里建一个不显示的顶层窗口（WS_EX_TOOLWINDOW 让它
	 * 不进任务栏和 Alt+Tab）。它随进程存活：没人订阅时 fireEvent 找不到订阅者，
	 * 本来就不会推事件，所以不需要在最后一个订阅者撤走时再销毁
	 */
	bool ensureDisplayWindow()
	{
		if (displayWindow) return true;
		constexpr wchar_t clsName[] = L"WebWinDisplayWindow";
		WNDCLASSW cls{};
		cls.lpfnWndProc = displayProc;
		cls.hInstance = GetModuleHandleW(nullptr);
		cls.lpszClassName = clsName;
		if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
		displayWindow = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, clsName, nullptr,
			WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, cls.hInstance, nullptr);
		if (!displayWindow) return false;
		snapshot = enumDisplays(); // 先记一份基线：之后的变化才算"新增/移除"
		return true;
	}

	/** PNG 编码器的 CLSID，查一次就缓存 */
	const CLSID& pngClsid()
	{
		static const CLSID clsid = [] {
			UINT count = 0, size = 0;
			Gdiplus::GetImageEncodersSize(&count, &size);
			if (size == 0) return CLSID_NULL;
			std::vector<BYTE> buffer(size);
			auto codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
			if (Gdiplus::GetImageEncoders(count, size, codecs) != Gdiplus::Ok) return CLSID_NULL;
			for (UINT i = 0; i < count; ++i)
				if (std::wcscmp(codecs[i].MimeType, L"image/png") == 0) return codecs[i].Clsid;
			return CLSID_NULL;
		}();
		return clsid;
	}

	/** HBITMAP → PNG 字节流。bitmap 不能还选在 DC 里，否则 GDI+ 读不到内容 */
	bool bitmapToPng(HBITMAP bitmap, std::vector<BYTE>& out)
	{
		ULONG_PTR token = 0;
		Gdiplus::GdiplusStartupInput input;
		if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) return false;

		IStream* stream = nullptr;
		auto ok = false;
		if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
		{
			Gdiplus::Bitmap bmp(bitmap, nullptr);
			auto clsid = pngClsid();
			if (clsid != CLSID_NULL && bmp.GetLastStatus() == Gdiplus::Ok && bmp.Save(stream, &clsid, nullptr) == Gdiplus::Ok)
			{
				STATSTG stat{};
				if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart > 0)
				{
					out.resize(static_cast<size_t>(stat.cbSize.QuadPart));
					LARGE_INTEGER zero{};
					stream->Seek(zero, STREAM_SEEK_SET, nullptr);
					ULONG read = 0;
					stream->Read(out.data(), static_cast<ULONG>(out.size()), &read);
					out.resize(read);
					ok = read > 0;
				}
			}
			stream->Release();
		}
		Gdiplus::GdiplusShutdown(token);
		return ok;
	}
}

bool Display::handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	if (method == L"on") { on(page, params, result, frame); return false; }
	if (method == L"off") { off(page, params, result, frame); return false; }

	if (method == L"getAll") getAll(result);
	else if (method == L"getPrimary") getPrimary(result);
	else if (method == L"getByPos") getByPos(params, result);
	else if (method == L"getByRect") getByRect(params, result);
	else if (method == L"getColor") getColor(params, result);
	else if (method == L"getImage") return getImage(params, page, result, frame);
	else Json::setError(result, L"unknown method: " + method);
	return false;
}

void Display::getAll(JsonObject& result)
{
	JsonArray list;
	for (auto& item : enumDisplays()) list.Append(toJson(item));
	Json::setOk(result);
	result.SetNamedValue(L"data", list);
}

void Display::getPrimary(JsonObject& result)
{
	for (auto& item : enumDisplays())
	{
		if (!item.primary) continue;
		Json::setOk(result);
		result.SetNamedValue(L"data", toJson(item));
		return;
	}
	Json::setError(result, L"找不到主显示器");
}

void Display::getByPos(const JsonObject& params, JsonObject& result)
{
	POINT pt{ Json::getInt(params, L"x", 0), Json::getInt(params, L"y", 0) };
	// DEFAULTTONEAREST：坐标落在多屏之间的空隙里（或全在屏外）时也要给出一块，
	// 而不是返回 null 让调用方自己兜底
	auto hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
	DisplayInfo info;
	if (!hMonitor || !infoOf(hMonitor, info)) { Json::setError(result, L"取显示器失败"); return; }
	Json::setOk(result);
	result.SetNamedValue(L"data", toJson(info));
}

void Display::getByRect(const JsonObject& params, JsonObject& result)
{
	RECT rect{};
	rect.left = Json::getInt(params, L"x", 0);
	rect.top = Json::getInt(params, L"y", 0);
	rect.right = rect.left + Json::getInt(params, L"width", 0);
	rect.bottom = rect.top + Json::getInt(params, L"height", 0);

	auto displays = enumDisplays();
	if (displays.empty()) { Json::setError(result, L"没有显示器"); return; }

	// 先看重叠面积：窗口跨两块屏时归到"占得最多"的那块（Electron 的口径）
	const DisplayInfo* best = nullptr;
	long long bestArea = 0;
	for (auto& item : displays)
	{
		auto area = overlapArea(rect, item.monitor);
		if (!best || area > bestArea) { best = &item; bestArea = area; }
	}
	// 一块都不重叠（矩形被拖到所有屏幕之外）：退化为中心点最近的那块，保证有返回
	if (bestArea == 0)
	{
		best = &displays[0];
		auto bestDist = centerDistance(rect, *best);
		for (auto& item : displays)
		{
			auto dist = centerDistance(rect, item);
			if (dist < bestDist) { best = &item; bestDist = dist; }
		}
	}
	Json::setOk(result);
	result.SetNamedValue(L"data", toJson(*best));
}

void Display::getColor(const JsonObject& params, JsonObject& result)
{
	auto hdcScreen = GetDC(nullptr);
	if (!hdcScreen) { Json::setError(result, L"取屏幕 DC 失败"); return; }

	POINT pt;
	// 不给坐标就取鼠标当前位置：页面常用的"取色器"场景
	if (Json::hasKey(params, L"x") && Json::hasKey(params, L"y"))
	{
		pt.x = Json::getInt(params, L"x", 0);
		pt.y = Json::getInt(params, L"y", 0);
	}
	else GetCursorPos(&pt);

	auto color = GetPixel(hdcScreen, pt.x, pt.y);
	ReleaseDC(nullptr, hdcScreen);
	Json::setOk(result);
	result.SetNamedValue(L"data", JsonValue::CreateStringValue(colorToHex(color)));
}

bool Display::getImage(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto x = Json::getInt(params, L"x", 0);
	auto y = Json::getInt(params, L"y", 0);
	auto width = Json::getInt(params, L"width", 0);
	auto height = Json::getInt(params, L"height", 0);
	if (width <= 0 || height <= 0) { Json::setError(result, L"截图区域无效"); return false; }

	auto hdcScreen = GetDC(nullptr);
	if (!hdcScreen) { Json::setError(result, L"取屏幕 DC 失败"); return false; }
	auto hdcMem = CreateCompatibleDC(hdcScreen);
	auto hBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
	auto oldObj = SelectObject(hdcMem, hBitmap);
	BitBlt(hdcMem, 0, 0, width, height, hdcScreen, x, y, SRCCOPY);
	SelectObject(hdcMem, oldObj); // 必须先摘出来：还选在 DC 里 GDI+ 读不到内容
	DeleteDC(hdcMem);
	ReleaseDC(nullptr, hdcScreen);

	std::vector<BYTE> png;
	auto encoded = bitmapToPng(hBitmap, png);
	DeleteObject(hBitmap);
	if (!encoded) { Json::setError(result, L"PNG 编码失败"); return false; }

	// 大块数据走共享内存，不进 JSON 信封（见 arch/07-web.md 第八节）
	ComPtr<ICoreWebView2Environment12> env12;
	auto env = Env::getEnv();
	if (!env || FAILED(env->QueryInterface(IID_PPV_ARGS(&env12)))) { Json::setError(result, L"WebView2 环境不支持共享内存"); return false; }

	ComPtr<ICoreWebView2SharedBuffer> buffer;
	if (FAILED(env12->CreateSharedBuffer(png.size(), &buffer))) { Json::setError(result, L"创建共享内存失败"); return false; }
	BYTE* data = nullptr;
	if (FAILED(buffer->get_Buffer(&data)) || !data) { Json::setError(result, L"写共享内存失败"); return false; }
	std::memcpy(data, png.data(), png.size());

	JsonObject imageData;
	imageData.SetNamedValue(L"width", JsonValue::CreateNumberValue(width));
	imageData.SetNamedValue(L"height", JsonValue::CreateNumberValue(height));
	Json::setOk(result);
	result.SetNamedValue(L"data", imageData);

	// 信封本身当 additionalData 一起带过去：页面靠其中的 id 把 buffer 关联回本次调用
	auto json = result.Stringify();
	auto hr = page->postSharedBuffer(buffer.Get(), json.c_str(), frame);
	// 顺序不能反：先 Post 再 Close。已经 Close 过的 buffer 再 Post 会直接返回 RO_E_CLOSED
	buffer->Close(); // 关掉本进程对这段内存的映射，之后由渲染进程持有；页面调 releaseBuffer 才真正释放
	if (FAILED(hr))
	{
		Json::setError(result, L"发送共享内存失败 HRESULT=" + hexOf(hr)); // 覆盖上面的 ok，改走普通响应
		return false;
	}
	return true; // 已自行发回：调用方不要再发一次 JSON
}

void Display::on(Page* page, const JsonObject& params, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto name = Json::getString(params, L"name", L"");
	if (name != EVENT_ADDED && name != EVENT_REMOVED) { Json::setError(result, L"unknown event: " + name); return; }
	if (!ensureDisplayWindow()) { Json::setError(result, L"注册显示器变化监听失败"); return; }
	Win::subscribeEvent(Win::NO_WINDOW, name, page, frame);
	Json::setOk(result);
}

void Display::off(Page* page, const JsonObject& params, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto name = Json::getString(params, L"name", L"");
	if (name != EVENT_ADDED && name != EVENT_REMOVED) { Json::setError(result, L"unknown event: " + name); return; }
	Win::unsubscribeEvent(Win::NO_WINDOW, name, page, frame);
	Json::setOk(result);
}
