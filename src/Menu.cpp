#include "Menu.h"

#include <map>
#include <memory>
#include <winrt/Windows.Foundation.Collections.h>

#include "App.h"
#include "Json.h"

namespace {
	std::map<int, std::unique_ptr<Menu>> menus;
	int lastId = 0;
	// 已有菜单在弹：TrackPopupMenu 是模态循环，期间不再弹第二个
	bool showing = false;
}

Menu::Menu(Page* page, ICoreWebView2Frame2* frame) : page{ page }, frame{ frame }
{
	handle = CreatePopupMenu();
}

Menu::~Menu()
{
	if (handle) DestroyMenu(handle); // 子菜单会被一并销毁
}

void Menu::handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	if (method == L"create")
	{
		auto spec = Json::getArray(params, L"items");
		if (!spec) { Json::setError(result, L"menu items missing"); return; }

		auto menu = std::unique_ptr<Menu>(new Menu(page, frame));
		if (!menu->handle) { Json::setError(result, L"创建菜单失败"); return; }
		if (!menu->build(spec, menu->handle)) { Json::setError(result, L"菜单项格式不对"); return; }

		auto id = ++lastId;
		menu->id = id;
		menus.emplace(id, std::move(menu));
		Json::setOk(result);
		result.SetNamedValue(L"data", JsonValue::CreateNumberValue(id));
		return;
	}

	auto id = Json::getInt(params, L"id", -1);
	auto it = menus.find(id);
	if (it == menus.end()) { Json::setError(result, L"menu not found: " + std::to_wstring(id)); return; }

	if (method == L"popup")
	{
		if (showing) { Json::setError(result, L"已有菜单在显示"); return; }
		POINT pt;
		// 没给坐标就弹在鼠标处
		if (params && params.HasKey(L"x") && params.HasKey(L"y"))
		{
			pt.x = Json::getInt(params, L"x", 0);
			pt.y = Json::getInt(params, L"y", 0);
		}
		else GetCursorPos(&pt);

		auto hwnd = page->getHwnd();
		if (!hwnd) { Json::setError(result, L"页面所在的窗口已经没了"); return; }
		// 不在 WebView2 的回调里开模态循环（那里的嵌套循环 = 回调套回调，会被判重入）：
		// 排到下一次消息循环再弹
		if (!App::runLater([id, hwnd, pt] { Menu::popup(id, hwnd, pt.x, pt.y); }))
		{
			Json::setError(result, L"无法显示菜单：调度失败");
			return;
		}
		Json::setOk(result);
		return;
	}

	if (method == L"destroy")
	{
		menus.erase(it); // 忘调就一直占着 HMENU：这是开发者的责任（T007 之前没有动态改菜单）
		Json::setOk(result);
		return;
	}

	Json::setError(result, L"unknown method: " + method);
}

bool Menu::build(const JsonArray& spec, HMENU menu)
{
	// radio 分组：连在一起的 radio 算同一组，遇到分隔线或其它类型的项就断开
	bool lastRadio = false;

	for (uint32_t i = 0; i < spec.Size(); ++i)
	{
		auto value = spec.GetAt(i);
		if (value.ValueType() != JsonValueType::Object) return false;
		auto specItem = value.GetObject();

		auto type = Json::getString(specItem, L"type", L"normal");
		auto label = Json::getString(specItem, L"label", L"");

		if (type == L"separator")
		{
			AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
			lastRadio = false; // 分隔线断开 radio 组
			continue;
		}

		auto submenu = Json::getArray(specItem, L"submenu");
		if (submenu && submenu.Size() > 0)
		{
			auto sub = CreatePopupMenu();
			if (!sub) return false;
			if (!build(submenu, sub)) { DestroyMenu(sub); return false; }
			// 子菜单项本身不可点击：它只负责展开，给它配 id / onclick 是无效的
			AppendMenuW(menu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(sub), label.c_str());
			lastRadio = false;
			continue;
		}

		Item item;
		item.id = Json::getString(specItem, L"id", L"");
		item.cmd = nextCmd++;
		item.owner = menu;
		item.checked = Json::getBool(specItem, L"checked", false);
		if (type == L"radio")
		{
			item.type = Radio;
			if (!lastRadio) ++nextGroup; // 新的一组
			item.group = nextGroup;
		}
		else if (type == L"checkbox") item.type = Checkbox;

		UINT flags = MF_STRING;
		if (item.checked) flags |= MF_CHECKED;
		if (Json::getBool(specItem, L"disabled", false)) flags |= MF_GRAYED;
		AppendMenuW(menu, flags, item.cmd, label.c_str());

		items.push_back(item);
		lastRadio = (item.type == Radio);
	}
	return true;
}

void Menu::popup(int menuId, HWND hwnd, int x, int y)
{
	auto it = menus.find(menuId);
	if (it == menus.end() || showing) return;
	auto menu = it->second.get();

	showing = true;
	// TPM_RETURNCMD：选中的项直接作为返回值，不走 WM_COMMAND 回到窗口过程（少一条重入路径）
	// TPM_NONOTIFY：不因为菜单项点击给窗口发通知消息
	auto cmd = TrackPopupMenu(menu->handle, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_RIGHTBUTTON,
		x, y, 0, hwnd, nullptr);
	showing = false;

	if (cmd == 0) return; // 取消了：点了空白处或按了 Esc
	// 弹菜单期间它自己可能已经被销毁了（模态循环照样派发消息：窗口被关、
	// 或者 JS 在这期间调了 destroy）。这时不能再碰 menu 指向的东西
	if (menus.find(menuId) == menus.end()) return;
	for (auto& item : menu->items)
	{
		if (item.cmd == cmd) { menu->onClick(item); return; }
	}
}

bool Menu::isShowing()
{
	return showing;
}

void Menu::onClick(Item& item)
{
	bool checked = item.checked;
	if (item.type == Radio)
	{
		// radio：同组只留一个勾选（"连在一起的就是同一组"，见 build）
		for (auto& other : items)
		{
			if (other.group != item.group || other.group == 0) continue;
			other.checked = (&other == &item);
			CheckMenuItem(other.owner, other.cmd, MF_BYCOMMAND | (other.checked ? MF_CHECKED : MF_UNCHECKED));
		}
		checked = true;
	}
	else if (item.type == Checkbox)
	{
		checked = !item.checked;
		item.checked = checked;
		CheckMenuItem(item.owner, item.cmd, MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
	}

	// 推给创建这个菜单的文档（顶级文档或那个 iframe）
	JsonObject msg;
	msg.SetNamedValue(L"menu", JsonValue::CreateNumberValue(id));
	msg.SetNamedValue(L"item", JsonValue::CreateStringValue(item.id));
	// checked 只有 radio / checkbox 才有：点击回调靠它同步 UI
	if (item.type == Radio || item.type == Checkbox)
	{
		msg.SetNamedValue(L"checked", JsonValue::CreateBooleanValue(checked));
	}
	page->send(msg, frame.Get());
}

void Menu::removeByPage(Page* target)
{
	// 窗口被关时可能正好弹着菜单：先把它收掉。
	// 否则 TrackPopupMenu 还在跑，而它的 HMENU 紧接着就被销毁了
	if (showing) EndMenu();
	for (auto it = menus.begin(); it != menus.end();)
	{
		if (it->second->page == target) it = menus.erase(it);
		else ++it;
	}
}

void Menu::removeByFrame(Page* target, ICoreWebView2Frame2* targetFrame)
{
	for (auto it = menus.begin(); it != menus.end();)
	{
		if (it->second->page == target && it->second->frame.Get() == targetFrame) it = menus.erase(it);
		else ++it;
	}
}
