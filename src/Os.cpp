// winsock2 必须排在 Windows.h 之前（Os.h 会带上 Windows.h），否则两个版本的
// socket 定义会打架。iphlpapi / wtsapi32 依赖它，所以整组都放在最前面
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <wtsapi32.h>

#include <cstring>
#include <intrin.h>
#include <vector>
#include <shlobj.h>  // SHGetKnownFolderPath / FOLDERID_*
#include <wincred.h> // CredWrite / CredRead / CredDelete / CredEnumerate

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

	// 凭据一律用"普通凭据"（CRED_TYPE_GENERIC）：凭据管理器里"Windows 凭据"那一栏，
	// 存的是一段不透明的值，不绑定用户名
	constexpr DWORD CRED_KIND = CRED_TYPE_GENERIC;

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

	/** getPath 的目录名 → KnownFolder 的 GUID；不认识的名字返回 nullptr */
	const GUID* folderIdOf(const std::wstring& name)
	{
		if (name == L"home") return &FOLDERID_Profile;
		if (name == L"appData") return &FOLDERID_RoamingAppData;
		if (name == L"desktop") return &FOLDERID_Desktop;
		if (name == L"documents") return &FOLDERID_Documents;
		if (name == L"downloads") return &FOLDERID_Downloads;
		if (name == L"music") return &FOLDERID_Music;
		if (name == L"pictures") return &FOLDERID_Pictures;
		if (name == L"videos") return &FOLDERID_Videos;
		if (name == L"recent") return &FOLDERID_Recent;
		return nullptr;
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

	if (method == L"getVersion") getVersion(result);
	else if (method == L"getCpuId") getCpuId(result);
	else if (method == L"getDiskId") getDiskId(result);
	else if (method == L"getLang") getLang(params, result);
	else if (method == L"getColor") getColor(result);
	else if (method == L"getIpAddr") getIpAddr(result);
	else if (method == L"getPath") getPath(params, result);
	else if (method == L"preventSleep") preventSleep(result);
	else if (method == L"stopPreventSleep") stopPreventSleep(result);
	else if (method == L"credWrite") credWrite(params, result);
	else if (method == L"credRead") credRead(params, result);
	else if (method == L"credDelete") credDelete(params, result);
	else if (method == L"credList") credList(params, result);
	else Json::setError(result, L"unknown method: " + method);
}

void Os::getVersion(JsonObject& result)
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

void Os::getLang(const JsonObject& params, JsonObject& result)
{
	// 只有两种取值：写错了直接报错，不猜一个默认值 ——
	// 这两个语言在多用户机器上不同，悄悄取错比报错更难查
	auto scope = Json::getString(params, L"scope", L"");
	auto langId = LANGID{ 0 };
	if (scope == L"user") langId = GetUserDefaultLangID();
	else if (scope == L"os") langId = GetSystemDefaultLangID();
	else { Json::setError(result, L"scope 只能是 'user' 或 'os'"); return; }

	std::wstring name;
	if (!langName(langId, name)) { Json::setError(result, L"取语言失败"); return; }
	Json::setOk(result);
	result.SetNamedValue(L"data", JsonValue::CreateStringValue(name));
}

void Os::getColor(JsonObject& result)
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

void Os::getPath(const JsonObject& params, JsonObject& result)
{
	auto name = Json::getString(params, L"name", L"");
	if (name.empty()) { Json::setError(result, L"missing name"); return; }

	std::wstring dir;
	// temp 不在 KnownFolder 里，走 GetTempPath。它给的路径带尾部的反斜杠
	if (name == L"temp")
	{
		wchar_t buf[MAX_PATH + 1];
		auto len = GetTempPathW(MAX_PATH + 1, buf);
		if (len == 0 || len > MAX_PATH + 1) { Json::setError(result, L"取临时目录失败"); return; }
		dir.assign(buf, len);
		if (dir.size() > 3 && dir.back() == L'\\') dir.pop_back(); // 与其它目录一致：不带尾部分隔符
	}
	else
	{
		auto folder = folderIdOf(name);
		if (!folder) { Json::setError(result, L"unknown name: " + name); return; }

		PWSTR path = nullptr;
		auto hr = SHGetKnownFolderPath(*folder, 0, nullptr, &path);
		if (FAILED(hr) || !path) { Json::setError(result, L"取目录失败: " + name); return; }
		dir = path;
		CoTaskMemFree(path);
	}

	Json::setOk(result);
	result.SetNamedValue(L"data", JsonValue::CreateStringValue(dir));
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

/**
 * 三个凭据方法共用的参数校验：key 必填且不能是空串。
 *
 * 空串也拒绝 —— 凭据管理器允许空名字，但那没有任何意义，
 * 只会让人在"凭据管理器"里看到一条看不出是什么的条目
 */
static bool credKey(const JsonObject& params, std::wstring& key)
{
	key = Json::getString(params, L"key", L"");
	return !key.empty();
}

void Os::credWrite(const JsonObject& params, JsonObject& result)
{
	std::wstring key;
	if (!credKey(params, key)) { Json::setError(result, L"missing key"); return; }
	auto value = Json::getString(params, L"value", L""); // 允许存空值

	// TargetName 是**可写**的 LPWSTR（结构体里不是 const），所以拷一份
	auto name = std::vector<wchar_t>(key.begin(), key.end());
	name.push_back(L'\0');

	CREDENTIALW cred{};
	cred.Type = CRED_KIND;
	cred.TargetName = name.data();
	// 值按 UTF-16 原样存：JS 的字符串本来就是 UTF-16，
	// 这样任何字符都能原样往返（转成 UTF-8 存，孤立的代理项会坏掉）
	cred.CredentialBlobSize = static_cast<DWORD>(value.size() * sizeof(wchar_t));
	cred.CredentialBlob = reinterpret_cast<LPBYTE>(value.data());
	// 留在本机：不随用户配置漫游到域里的其它机器，只对当前用户可见
	cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
	// 不给 UserName：普通凭据只有一段值，用户名/密码那套是"域凭据"的形态

	if (!CredWriteW(&cred, 0)) { Json::setError(result, L"写凭据失败：" + Util::winErr(GetLastError())); return; }
	Json::setOk(result);
}

void Os::credRead(const JsonObject& params, JsonObject& result)
{
	std::wstring key;
	if (!credKey(params, key)) { Json::setError(result, L"missing key"); return; }

	PCREDENTIALW cred = nullptr;
	if (!CredReadW(key.c_str(), CRED_KIND, 0, &cred))
	{
		// "没存过"不算故障：按 fs.access 的口径给 null，调用方不必为"还没存"写 try/catch
		if (GetLastError() == ERROR_NOT_FOUND)
		{
			Json::setOk(result);
			result.SetNamedValue(L"data", JsonValue::CreateNullValue());
			return;
		}
		Json::setError(result, L"读凭据失败：" + Util::winErr(GetLastError()));
		return;
	}

	// CredentialBlobSize 给的是**字节数**，这里要的是 wchar_t 个数
	std::wstring value(reinterpret_cast<wchar_t*>(cred->CredentialBlob),
		cred->CredentialBlobSize / sizeof(wchar_t));
	CredFree(cred);

	Json::setOk(result);
	result.SetNamedValue(L"data", JsonValue::CreateStringValue(value));
}

void Os::credDelete(const JsonObject& params, JsonObject& result)
{
	std::wstring key;
	if (!credKey(params, key)) { Json::setError(result, L"missing key"); return; }
	// 删的关键是"删完之后不存在"，所以本来就没有也算达成 —— 不报错（幂等）
	if (!CredDeleteW(key.c_str(), CRED_KIND, 0) && GetLastError() != ERROR_NOT_FOUND)
	{
		Json::setError(result, L"删凭据失败：" + Util::winErr(GetLastError()));
		return;
	}
	Json::setOk(result);
}

void Os::credList(const JsonObject& params, JsonObject& result)
{
	auto prefix = Json::getString(params, L"prefix", L"");
	// filter 是前缀匹配，末尾必须带 *（CredEnumerate 的规矩）。
	// 调用方按"以它开头"给前缀就行，这里的 * 自动补上
	auto filter = prefix.empty() ? std::wstring{} : (prefix.back() == L'*' ? prefix : prefix + L'*');

	DWORD count = 0;
	PCREDENTIALW* list = nullptr;
	if (!CredEnumerateW(filter.empty() ? nullptr : filter.c_str(), 0, &count, &list))
	{
		// 一条都没有时同样报 ERROR_NOT_FOUND：那是空集合，不是故障
		if (GetLastError() == ERROR_NOT_FOUND)
		{
			Json::setOk(result);
			result.SetNamedValue(L"data", JsonArray{});
			return;
		}
		Json::setError(result, L"列凭据失败：" + Util::winErr(GetLastError()));
		return;
	}

	// 只给 key（TargetName），不给值：要值就 credRead(key) 逐个读，拿的一定是最新的
	JsonArray keys;
	for (DWORD i = 0; i < count; ++i) keys.Append(JsonValue::CreateStringValue(list[i]->TargetName));
	CredFree(list);

	Json::setOk(result);
	result.SetNamedValue(L"data", keys);
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
