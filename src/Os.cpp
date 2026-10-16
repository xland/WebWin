// winsock2 必须排在 Windows.h 之前（Os.h 会带上 Windows.h），否则两个版本的
// socket 定义会打架。iphlpapi / wtsapi32 依赖它，所以整组都放在最前面
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <wtsapi32.h>

#include <cstring>
#include <intrin.h>
#include <vector>

// JsonArray::Append 是 IVector 的方法，要显式带上集合头文件（Json.cpp 同理）
#include <winrt/Windows.Foundation.Collections.h>

#include "Os.h"
#include "Json.h"
#include "Util.h"
#include "Win.h"

namespace {
	typedef LONG (WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);

	// 本域唯一支持的事件
	constexpr wchar_t EVENT_OS_LOCK[] = L"osLock";

	HWND sessionWindow{ nullptr }; // 接锁屏通知用的隐藏消息窗口

	/** COLORREF 是 0x00BBGGRR，转成 CSS 用的 #RRGGBB */
	std::wstring colorToHex(COLORREF color)
	{
		wchar_t buf[8];
		swprintf_s(buf, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
		return buf;
	}

	/** LANGID → 区域名（如 zh-CN） */
	bool langName(LANGID langId, std::wstring& out)
	{
		wchar_t buf[LOCALE_NAME_MAX_LENGTH];
		if (!LCIDToLocaleName(MAKELCID(langId, SORT_DEFAULT), buf, LOCALE_NAME_MAX_LENGTH, 0)) return false;
		out = buf;
		return true;
	}

	/**
	 * 锁屏通知只能发给窗口（WTSRegisterSessionNotification 要 HWND），
	 * 而 os 域与窗口无关，所以自己开一个隐藏的消息窗口。
	 * 它随进程存活：没人订阅时 fireEvent 找不到订阅者，本来就不会推事件，
	 * 所以不需要在"最后一个订阅者撤走"时再注销一遍
	 */
	LRESULT CALLBACK sessionProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (msg == WM_WTSSESSION_CHANGE)
		{
			JsonObject payload;
			payload.SetNamedValue(L"locked", JsonValue::CreateBooleanValue(wParam == WTS_SESSION_LOCK));
			Win::fireEvent(Win::NO_WINDOW, EVENT_OS_LOCK, payload);
			return 0;
		}
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}

	bool ensureSessionWindow()
	{
		if (sessionWindow) return true;
		constexpr wchar_t clsName[] = L"WebWinOsSessionWindow";
		WNDCLASSW cls{};
		cls.lpfnWndProc = sessionProc;
		cls.hInstance = GetModuleHandleW(nullptr);
		cls.lpszClassName = clsName;
		if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
		sessionWindow = CreateWindowExW(0, clsName, nullptr, 0, 0, 0, 0, 0,
			HWND_MESSAGE, nullptr, cls.hInstance, nullptr); // 消息窗口：不显示、不占任务栏
		if (!sessionWindow) return false;
		return WTSRegisterSessionNotification(sessionWindow, NOTIFY_FOR_THIS_SESSION) != FALSE;
	}
}

void Os::handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	if (method == L"on") { on(page, params, result, frame); return; }
	if (method == L"off") { off(page, params, result, frame); return; }

	if (method == L"getOsVer") getOsVer(result);
	else if (method == L"getCpuId") getCpuId(result);
	else if (method == L"getDiskId") getDiskId(result);
	else if (method == L"getUserLang") getUserLang(result);
	else if (method == L"getOsLang") getOsLang(result);
	else if (method == L"getOsColor") getOsColor(result);
	else if (method == L"getIpAddr") getIpAddr(result);
	else if (method == L"preventSleep") preventSleep(result);
	else if (method == L"stopPreventSleep") stopPreventSleep(result);
	else Json::setError(result, L"unknown method: " + method);
}

void Os::getOsVer(JsonObject& result)
{
	// 用 RtlGetVersion 而不是 GetVersionEx：后者受 manifest 里的兼容性声明影响，
	// 新系统上不加对应的 supportedOS 就会报成旧版本
	auto ntdll = GetModuleHandleW(L"ntdll.dll");
	if (!ntdll) { Json::setError(result, L"加载 ntdll.dll 失败"); return; }
	auto rtl = reinterpret_cast<RtlGetVersionPtr>(GetProcAddress(ntdll, "RtlGetVersion"));
	if (!rtl) { Json::setError(result, L"取不到 RtlGetVersion"); return; }

	RTL_OSVERSIONINFOW info{};
	info.dwOSVersionInfoSize = sizeof(info);
	if (rtl(&info) != 0) { Json::setError(result, L"读取系统版本失败"); return; }

	JsonObject data;
	data.SetNamedValue(L"major", JsonValue::CreateNumberValue(info.dwMajorVersion));
	data.SetNamedValue(L"minor", JsonValue::CreateNumberValue(info.dwMinorVersion));
	data.SetNamedValue(L"build", JsonValue::CreateNumberValue(info.dwBuildNumber));
	// versionName 只给 Windows 10 / 11：更早的系统不猜，让调用方自己看 build
	if (info.dwMajorVersion == 10 && info.dwMinorVersion == 0)
	{
		if (info.dwBuildNumber >= 22000) data.SetNamedValue(L"versionName", JsonValue::CreateStringValue(L"win11"));
		else if (info.dwBuildNumber >= 10240) data.SetNamedValue(L"versionName", JsonValue::CreateStringValue(L"win10"));
	}
	Json::setOk(result);
	result.SetNamedValue(L"data", data);
}

void Os::getCpuId(JsonObject& result)
{
	int cpuInfo[4] = { 0 };
	__cpuid(cpuInfo, 1); // 1 号功能号：eax 是处理器签名，edx 是特性位
	wchar_t buf[17];
	swprintf_s(buf, L"%08x%08x", static_cast<unsigned>(cpuInfo[0]), static_cast<unsigned>(cpuInfo[3]));
	Json::setOk(result);
	result.SetNamedValue(L"data", JsonValue::CreateStringValue(buf));
}

void Os::getDiskId(JsonObject& result)
{
	wchar_t windowsDir[MAX_PATH];
	if (!GetWindowsDirectoryW(windowsDir, MAX_PATH)) { Json::setError(result, L"取 Windows 目录失败"); return; }
	auto root = std::wstring{ windowsDir }.substr(0, 3); // 形如 C:\

	DWORD serial = 0;
	if (!GetVolumeInformationW(root.c_str(), nullptr, 0, &serial, nullptr, nullptr, nullptr, 0))
	{
		Json::setError(result, L"取系统盘序列号失败");
		return;
	}
	wchar_t buf[9];
	swprintf_s(buf, L"%08x", serial);
	Json::setOk(result);
	result.SetNamedValue(L"data", JsonValue::CreateStringValue(buf));
}

void Os::getUserLang(JsonObject& result)
{
	std::wstring name;
	if (!langName(GetUserDefaultLangID(), name)) { Json::setError(result, L"取用户语言失败"); return; }
	Json::setOk(result);
	result.SetNamedValue(L"data", JsonValue::CreateStringValue(name));
}

void Os::getOsLang(JsonObject& result)
{
	std::wstring name;
	if (!langName(GetSystemDefaultLangID(), name)) { Json::setError(result, L"取系统语言失败"); return; }
	Json::setOk(result);
	result.SetNamedValue(L"data", JsonValue::CreateStringValue(name));
}

void Os::getOsColor(JsonObject& result)
{
	JsonObject data;
	data.SetNamedValue(L"window", JsonValue::CreateStringValue(colorToHex(GetSysColor(COLOR_WINDOW))));
	data.SetNamedValue(L"button", JsonValue::CreateStringValue(colorToHex(GetSysColor(COLOR_BTNFACE))));
	data.SetNamedValue(L"text", JsonValue::CreateStringValue(colorToHex(GetSysColor(COLOR_WINDOWTEXT))));
	data.SetNamedValue(L"caption", JsonValue::CreateStringValue(colorToHex(GetSysColor(COLOR_CAPTIONTEXT))));
	data.SetNamedValue(L"border", JsonValue::CreateStringValue(colorToHex(GetSysColor(COLOR_ACTIVEBORDER))));
	data.SetNamedValue(L"desktop", JsonValue::CreateStringValue(colorToHex(GetSysColor(COLOR_BACKGROUND))));
	Json::setOk(result);
	result.SetNamedValue(L"data", data);
}

void Os::getIpAddr(JsonObject& result)
{
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { Json::setError(result, L"初始化 Winsock 失败"); return; }

	// 先按经验值申请，不够时 GetAdaptersAddresses 会回填所需大小，再取一次
	ULONG size = 15000;
	std::vector<char> buffer(size);
	auto adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
	auto code = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &size);
	if (code == ERROR_BUFFER_OVERFLOW)
	{
		buffer.resize(size);
		adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
		code = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &size);
	}
	if (code != ERROR_SUCCESS)
	{
		WSACleanup();
		Json::setError(result, L"取网卡信息失败");
		return;
	}

	JsonArray list;
	for (auto adapter = adapters; adapter; adapter = adapter->Next)
	{
		if (adapter->OperStatus != IfOperStatusUp) continue; // 未启用的网卡
		if (adapter->PhysicalAddressLength == 0) continue;   // 没有 MAC：回环、虚拟网卡之类

		JsonObject item;
		item.SetNamedValue(L"name", JsonValue::CreateStringValue(adapter->FriendlyName));
		wchar_t mac[18];
		swprintf_s(mac, L"%02X-%02X-%02X-%02X-%02X-%02X",
			adapter->PhysicalAddress[0], adapter->PhysicalAddress[1], adapter->PhysicalAddress[2],
			adapter->PhysicalAddress[3], adapter->PhysicalAddress[4], adapter->PhysicalAddress[5]);
		item.SetNamedValue(L"mac", JsonValue::CreateStringValue(mac));

		JsonArray addresses;
		for (auto unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next)
		{
			auto addr = unicast->Address.lpSockaddr;
			char text[INET6_ADDRSTRLEN];
			if (addr->sa_family == AF_INET)
			{
				auto ipv4 = reinterpret_cast<sockaddr_in*>(addr);
				if (!inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text))) continue;
				if (std::strcmp(text, "127.0.0.1") == 0) continue; // 回环地址不算
			}
			else if (addr->sa_family == AF_INET6)
			{
				auto ipv6 = reinterpret_cast<sockaddr_in6*>(addr);
				if (!inet_ntop(AF_INET6, &ipv6->sin6_addr, text, sizeof(text))) continue;
				if (std::strcmp(text, "::1") == 0) continue;
			}
			else continue;
			addresses.Append(JsonValue::CreateStringValue(Util::utf8ToWide(text)));
		}
		item.SetNamedValue(L"addresses", addresses);
		list.Append(item);
	}

	WSACleanup();
	Json::setOk(result);
	result.SetNamedValue(L"data", list);
}

void Os::preventSleep(JsonObject& result)
{
	// ES_CONTINUOUS 表示"一直生效"，直到下一次调用改掉它
	SetThreadExecutionState(ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED | ES_CONTINUOUS);
	Json::setOk(result);
}

void Os::stopPreventSleep(JsonObject& result)
{
	SetThreadExecutionState(ES_CONTINUOUS); // 恢复默认：按电源策略自行休眠
	Json::setOk(result);
}

void Os::on(Page* page, const JsonObject& params, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto name = Json::getString(params, L"name", L"");
	if (name != EVENT_OS_LOCK) { Json::setError(result, L"unknown event: " + name); return; }
	if (!ensureSessionWindow()) { Json::setError(result, L"注册锁屏监听失败"); return; }
	Win::subscribeEvent(Win::NO_WINDOW, name, page, frame);
	Json::setOk(result);
}

void Os::off(Page* page, const JsonObject& params, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto name = Json::getString(params, L"name", L"");
	if (name != EVENT_OS_LOCK) { Json::setError(result, L"unknown event: " + name); return; }
	Win::unsubscribeEvent(Win::NO_WINDOW, name, page, frame);
	Json::setOk(result);
}
