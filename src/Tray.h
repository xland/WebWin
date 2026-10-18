#pragma once
#include <memory>
#include <string>
// wrl.h 必须在 winrt 头之前：C++/WinRT 的命名空间要靠它先铺好，
// 少了它下面那句 using namespace winrt::... 会报 C2760（Menu.h 是这么写的）
#include <wrl.h>
#include <Windows.h>
#include <WebView2.h>
#include <winrt/Windows.Data.Json.h>
// NOTIFYICONDATAW / Shell_NotifyIconW。**必须在 Windows.h 之后**：
// 它直接用 EXTERN_C / DECLSPEC_IMPORT / HDROP 这些，放前面会整条头文件链报错
#include <shellapi.h>

#include "Page.h"

// 顺序照 Menu.h / Db.h：先 WRL 再 winrt。winrt 的命名空间要 WRL 先铺好，
// 单独一句 using namespace winrt::... 会报 C2760（语法错误: 出现意外的"."）
using namespace Microsoft::WRL;
using namespace winrt::Windows::Data::Json;

/**
 * tray 域：系统托盘图标。
 *
 * 一个进程**允许建多个**托盘图标（Windows 本来就允许，这里不做限制），
 * 每个 Tray 占一个 uID。
 *
 * 图标归**进程**而不是归页面：所有实例共用一个隐藏窗口（懒建、随进程存活）
 * 来收 Shell_NotifyIcon 的回调。不能像 Menu 那样挂在某个 Win 的窗口过程上 ——
 * 那样窗口一关回调就没了，而"关掉窗口、应用留在托盘里"恰恰是托盘的主场景。
 *
 * 事件走 os / display 那套"与窗口无关"的订阅：Win::fireEvent(Win::NO_WINDOW, ...)。
 * 所以 JS 侧 `tray.on(...)` 用的是 Ipc 继承来的 on/off，本域只要认领事件名即可。
 */
class Tray
{
public:
	// 处理 type=tray 域的消息：按 method 分发到本域的具体动作
	static void handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame = nullptr);
	/**
	 * 是否还有托盘图标活着。
	 *
	 * 唯一的用处是 Win 的 WM_DESTROY：quitWhenNoWindow 为 true 时最后一个窗口
	 * 关掉就 PostQuitMessage，托盘图标会跟着瞬间消失。所以有托盘图标就不自动退，
	 * 由应用自己在菜单项里放个"退出"来决定何时结束。
	 */
	static bool hasInstance();

	// ---- 下面三个是内部用的：由隐藏窗口的窗口过程回调，不出现在 JS API 里 ----
	/** 把事件推给订阅者（走 NO_WINDOW 那条订阅，同 os / display） */
	void fire(const wchar_t* name);
	/** 右键时调：有菜单就弹出来 */
	void popupMenu(HWND hwnd);
	/** explorer.exe 重启后图标会被清掉，重新加一遍 */
	void reAdd();

	~Tray();
private:
	Tray();
	/**
	 * 建一个托盘图标：造实例 → 填 NOTIFYICONDATA → NIM_ADD → 入注册表，
	 * 成功时把新 id 写进 result.data。
	 *
	 * 静态是因为它干的是"造一个 Tray"（工厂），不是"某个 Tray 的动作"；
	 * 定义在 .cpp 里是因为它要用到匿名命名空间里的隐藏窗口和实例注册表
	 */
	static void create(const JsonObject& params, JsonObject& result);
	/** 把 tooltip 填进 nid.szTip；超过容量的截掉（系统只留 128 个 WCHAR） */
	void fillTip();
	/** 改提示文字并 NIM_MODIFY；返回是否成功 */
	bool setTip(const std::wstring& text);
private:
	int id{ 0 };
	int menuId{ -1 };       // create 时带的菜单；-1 = 没带
	std::wstring tooltip;
	NOTIFYICONDATAW nid{};  // uID 记的是 id，回调消息带回同一个 id
};
