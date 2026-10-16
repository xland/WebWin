#pragma once
#include <string>
#include <vector>
#include <wrl.h>
#include <Windows.h>
#include <WebView2.h>
#include <winrt/Windows.Data.Json.h>

#include "Page.h"

using namespace Microsoft::WRL;
using namespace winrt::Windows::Data::Json;

/**
 * menu 域：右键菜单（窗口右键菜单；将来的托盘菜单复用同一套）。
 *
 * 每个 Menu 持有一个 HMENU。菜单项被点击后，事件推给**创建它的那个文档** ——
 * 菜单与窗口无关（一个菜单可以挂到任意窗口上），所以跟 os 域一样走"能力域绑创建者"。
 */
class Menu
{
public:
	// 需要弹菜单时投给窗口的消息：窗口过程收到后在正常的消息流里调 popup。
	// wParam = 菜单 id，lParam = 坐标（LOWORD x / HIWORD y，屏幕坐标）
	static constexpr UINT MSG_POPUP = WM_APP + 1;

	// 处理 type=menu 域的消息：按 method 分发到本域的具体动作
	static void handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame = nullptr);
	/**
	 * 弹出菜单。**只**在窗口过程里调用（WM_APP + 1 那条消息），
	 * 不在 WebView2 事件回调里调用 —— 回调里开模态循环等于"回调套回调"，容易重入
	 */
	static void popup(int menuId, HWND hwnd, int x, int y);
	/** 是否已有菜单在弹：弹窗期间拒绝新的 popup，不排队也不嵌套 */
	static bool isShowing();
	// 页面 / iframe 没了：销毁它建的菜单，否则点击会推给一个已经不存在的文档
	static void removeByPage(Page* page);
	static void removeByFrame(Page* page, ICoreWebView2Frame2* frame);
	~Menu();
private:
	enum Type { Normal, Radio, Checkbox };
	struct Item
	{
		std::wstring id;       // JS 侧的 id：点击事件按它回传
		UINT cmd{ 0 };         // Win32 菜单项的命令 id
		Type type{ Normal };
		int group{ 0 };        // radio 组号：同组互斥；0 = 不在任何组里
		bool checked{ false };
		HMENU owner{ nullptr }; // 该项所在的（子）菜单：改勾选状态要用
	};
	Menu(Page* page, ICoreWebView2Frame2* frame);
	/** 按 JS 给的结构造菜单；submenu 递归 */
	bool build(const JsonArray& spec, HMENU menu);
	void onClick(Item& item);
private:
	int id{ 0 };
	HMENU handle{ nullptr };
	Page* page{ nullptr };
	ComPtr<ICoreWebView2Frame2> frame; // 空 = 顶级文档
	std::vector<Item> items;
	UINT nextCmd{ 1 }; // 命令 id 从 1 开始：0 留给"取消"（TrackPopupMenu 取消时返回 0）
	int nextGroup{ 0 };
};
