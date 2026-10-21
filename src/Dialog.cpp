#include <deque>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// JsonArray::Append 是 IVector 的方法，要显式带上集合头文件（Json.cpp 同理）
#include <winrt/Windows.Foundation.Collections.h>

#include <Windows.h>
#include <shellapi.h> // ShellExecute：在文件夹中显示文件
#include <shobjidl.h> // IFileOpenDialog / IFileSaveDialog / IShellItem

#include "App.h"
#include "Dialog.h"
#include "Json.h"

namespace
{
	/** 一个过滤器：显示名 + 通配符（"图片" / "*.png;*.jpg"） */
	using Filter = std::pair<std::wstring, std::wstring>;
	using FilterList = std::vector<Filter>;

	/**
	 * 一次**还没执行**的对话框请求。
	 * 存在的唯一原因：弹框不能在 WebView2 回调里跑（见 Dialog 类注释），得先存着。
	 */
	struct Task
	{
		std::wstring method;
		JsonObject params;
		JsonObject result; // 进来时已带 id，弹完补上 ok / data
		Page* page{ nullptr };
		ComPtr<ICoreWebView2Frame2> frame; // 空 = 顶级文档
	};

	// 排队等着弹的请求。App::runLater 每次排下一个任务，run() 从头取一条执行。
	// 用 deque 存值而不是把 Task* 塞进 LPARAM：窗口可能在消息被派发前就销毁了，
	// 那时指针就成了悬挂的，而队列里的条目能被 removeByPage 干净地清掉
	std::deque<Task> pending;

	bool isSupported(const std::wstring& method)
	{
		return method == L"openPath" || method == L"savePath"
			|| method == L"msgBox" || method == L"itemInFolder";
	}

	/** 读 params.filter：形如 [["图片","*.png;*.jpg"], ...]。没给就返回空列表 */
	FilterList readFilters(const JsonObject& params)
	{
		FilterList list;
		auto arr = Json::getArray(params, L"filter");
		if (!arr) return list;
		for (auto&& item : arr)
		{
			// 每一项必须是两元素的字符串数组，坏数据直接跳过而不是让整个调用失败
			if (!item || item.ValueType() != JsonValueType::Array) continue;
			auto spec = item.GetArray();
			if (spec.Size() < 2) continue;
			if (spec.GetAt(0).ValueType() != JsonValueType::String) continue;
			if (spec.GetAt(1).ValueType() != JsonValueType::String) continue;
			list.emplace_back(spec.GetAt(0).GetString(), spec.GetAt(1).GetString());
		}
		return list;
	}

	/** IShellItem → 文件系统路径；取不到返回空串 */
	std::wstring pathOf(IShellItem* item)
	{
		PWSTR raw{ nullptr };
		if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) return {};
		std::wstring path{ raw };
		CoTaskMemFree(raw);
		return path;
	}

	/** 通配符转扩展名：`*.png;*.jpg` → `.png`。给存文件框自动补后缀用 */
	std::wstring extensionOf(const std::wstring& pattern)
	{
		auto star = pattern.find(L'*');
		if (star == std::wstring::npos) return {};
		auto ext = pattern.substr(star + 1);
		auto end = ext.find_first_of(L";,"); // 一个过滤器里可能写了多个后缀，取第一个
		if (end != std::wstring::npos) ext = ext.substr(0, end);
		return ext;
	}

	/** 对话框的公共设置：标题、确认按钮文字。单项失败不中断，只记下来 */
	bool applyCommonOptions(IFileDialog* dlg, const JsonObject& params)
	{
		auto ok = true;
		auto title = Json::getString(params, L"title", L"");
		if (!title.empty() && FAILED(dlg->SetTitle(title.c_str()))) ok = false;
		auto okText = Json::getString(params, L"okBtnText", L"");
		if (!okText.empty() && FAILED(dlg->SetOkButtonLabel(okText.c_str()))) ok = false;
		return ok;
	}

	/**
	 * 设置初始目录 / 默认文件名。
	 *
	 * defaultPath 既可能是目录也可能是文件，所以先看它是不是已存在的目录：
	 * - 是：整个当作初始目录
	 * - 否：按最后一个分隔符拆成"目录 + 文件名"
	 *
	 * forSave 时才用文件名：选文件框没有"预填文件名"这回事（只有初始目录）。
	 * 存文件框用 SetFolder（而不是 SetDefaultFolder），因为 FOS_PATHMUSTEXIST
	 * 要求真的切到那个目录，SetDefaultFolder 只在对话框内部导航历史里生效。
	 */
	void applyDefaultPath(IFileDialog* dlg, const std::wstring& defaultPath, bool forSave)
	{
		if (defaultPath.empty()) return;
		std::error_code ec; // 路径不合法时返回 false 而不是抛异常
		std::wstring dir, name;
		if (std::filesystem::is_directory(defaultPath, ec))
		{
			dir = defaultPath;
		}
		else
		{
			auto lastSlash = defaultPath.find_last_of(L"\\/");
			if (lastSlash == std::wstring::npos) dir = defaultPath;
			else
			{
				dir = defaultPath.substr(0, lastSlash);
				name = defaultPath.substr(lastSlash + 1);
			}
		}

		ComPtr<IShellItem> folder;
		if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder)
		{
			if (forSave) dlg->SetFolder(folder.Get());
			else dlg->SetDefaultFolder(folder.Get());
		}
		if (forSave && !name.empty()) dlg->SetFileName(name.c_str());
	}

	/**
	 * 统一的返回构造：{ ok: true, data: { cancel, data } }。
	 * 用户点"取消"是正常结果、不是错误，所以不走 setError —— 走 data.cancel
	 */
	void finish(JsonObject& result, bool cancelled, const IJsonValue& payload)
	{
		JsonObject data;
		data.SetNamedValue(L"cancel", JsonValue::CreateBooleanValue(cancelled));
		data.SetNamedValue(L"data", payload);
		Json::setOk(result);
		result.SetNamedValue(L"data", data);
	}
}

bool Dialog::handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	if (!isSupported(method)) return false; // 交给调用方回 unknown method

	auto hwnd = page ? page->getHwnd() : nullptr; // 对话框要挂在这个窗口上才模态
	if (!hwnd) { Json::setError(result, L"dialog.windowGone", L"页面所在的窗口已经没了"); return false; }

	Task task;
	task.method = method;
	task.params = params;
	task.result = result; // JsonObject 是引用语义：这是同一个对象，稍后由 run() 补上 ok / data
	task.page = page;
	task.frame = frame;
	pending.push_back(std::move(task));

	// 排到下一次消息循环再弹：当场弹就是"回调套回调"，Runtime 会判重入。
	// 不把 Task* 塞进消息里传 —— 窗口可能在任务执行前就没了，指针会变悬挂的
	if (!App::runLater([] { Dialog::run(); }))
	{
		pending.pop_back(); // 没排进去：把刚放进去的这条撤了，别让它占着队列
		Json::setError(result, L"dialog.showFailed", L"无法显示对话框：调度失败");
		return false;
	}
	return true; // 响应稍后由 run() 发回：调用方不要再发一次
}

void Dialog::run()
{
	if (pending.empty()) return; // 页面已销毁，请求被 removeByPage 清掉了
	auto task = std::move(pending.front());
	pending.pop_front();

	auto owner = task.page->getHwnd();
	if (task.method == L"openPath") openPath(task.params, owner, task.result);
	else if (task.method == L"savePath") savePath(task.params, owner, task.result);
	else if (task.method == L"msgBox") msgBox(task.params, owner, task.result);
	else if (task.method == L"itemInFolder") itemInFolder(task.params, task.result);

	// 弹完才回：此刻已回到消息循环里，走的是 UI 线程上的 WebView2
	task.page->send(task.result, task.frame.Get());
}

void Dialog::removeByPage(Page* target)
{
	std::erase_if(pending, [target](const Task& task) { return task.page == target; });
}

void Dialog::openPath(const JsonObject& params, HWND owner, JsonObject& result)
{
	ComPtr<IFileOpenDialog> dlg;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg))) || !dlg)
	{
		Json::setError(result, L"dialog.showFailed", L"创建文件对话框失败");
		return;
	}

	// type=dir 时只选文件夹；选文件时要 FOS_FORCEFILESYSTEM ——
	// 否则用户可能选到"此电脑"这类虚拟目录，拿回来的是 GUID 而不是真实路径
	auto isDir = Json::getString(params, L"type", L"file") == L"dir";
	FILEOPENDIALOGOPTIONS options = isDir ? (FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM) : FOS_FORCEFILESYSTEM;
	if (Json::getBool(params, L"multiSelection", false)) options |= FOS_ALLOWMULTISELECT;
	if (Json::getBool(params, L"showHiddenFile", false)) options |= FOS_FORCESHOWHIDDEN;

	DWORD current{ 0 }; // 先取再合，别把默认选项（比如 FOS_FILEMUSTEXIST）冲掉
	if (SUCCEEDED(dlg->GetOptions(&current))) dlg->SetOptions(current | options);
	if (!applyCommonOptions(dlg.Get(), params)) { Json::setError(result, L"dialog.error", L"设置对话框选项失败"); return; }

	// 过滤器只在选文件时有效：选文件夹对话框不接受文件类型
	auto filters = readFilters(params);
	if (!isDir && !filters.empty())
	{
		// COMDLG_FILTERSPEC 里存的是指针，所以容器得活到 SetFileTypes 返回之后
		std::vector<COMDLG_FILTERSPEC> specs;
		specs.reserve(filters.size());
		for (auto& f : filters) specs.push_back({ f.first.c_str(), f.second.c_str() });
		dlg->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
		if (auto ext = extensionOf(filters.front().second); !ext.empty())
			dlg->SetDefaultExtension(ext.c_str()); // 用户只输入名字时自动补后缀
	}
	applyDefaultPath(dlg.Get(), Json::getString(params, L"defaultPath", L""), false);

	// 模态：Show 内部会跑嵌套消息循环，窗口不会卡死
	auto hr = dlg->Show(owner);
	if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) { finish(result, true, JsonArray{}); return; }
	if (FAILED(hr)) { Json::setError(result, L"显示文件对话框失败"); return; }

	JsonArray paths;
	if ((options & FOS_ALLOWMULTISELECT) == FOS_ALLOWMULTISELECT)
	{
		ComPtr<IShellItemArray> results;
		if (FAILED(dlg->GetResults(&results)) || !results) { Json::setError(result, L"dialog.error", L"读取所选文件失败"); return; }
		DWORD count{ 0 };
		if (FAILED(results->GetCount(&count))) { Json::setError(result, L"dialog.error", L"读取所选文件失败"); return; }
		for (DWORD i = 0; i < count; ++i)
		{
			ComPtr<IShellItem> item;
			if (SUCCEEDED(results->GetItemAt(i, &item)) && item)
				paths.Append(JsonValue::CreateStringValue(pathOf(item.Get())));
		}
	}
	else
	{
		ComPtr<IShellItem> item;
		if (FAILED(dlg->GetResult(&item)) || !item) { Json::setError(result, L"dialog.error", L"读取所选文件失败"); return; }
		paths.Append(JsonValue::CreateStringValue(pathOf(item.Get())));
	}
	finish(result, false, paths);
}

void Dialog::savePath(const JsonObject& params, HWND owner, JsonObject& result)
{
	ComPtr<IFileSaveDialog> dlg;
	if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg))) || !dlg)
	{
		Json::setError(result, L"dialog.showFailed", L"创建保存对话框失败");
		return;
	}

	// 存文件框的两个固定要求：
	// - 去掉 FOS_FILEMUSTEXIST：要存的通常是还没建的新文件
	// - 加上 FOS_PATHMUSTEXIST / FOS_OVERWRITEPROMPT：路径必须存在，覆盖前要问一句
	auto options = FOS_FORCEFILESYSTEM;
	DWORD current{ 0 };
	if (SUCCEEDED(dlg->GetOptions(&current))) dlg->SetOptions((current | options) & ~FOS_FILEMUSTEXIST);
	if (!applyCommonOptions(dlg.Get(), params)) { Json::setError(result, L"dialog.error", L"设置对话框选项失败"); return; }

	auto filters = readFilters(params);
	if (!filters.empty())
	{
		std::vector<COMDLG_FILTERSPEC> specs;
		specs.reserve(filters.size());
		for (auto& f : filters) specs.push_back({ f.first.c_str(), f.second.c_str() });
		dlg->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
		if (auto ext = extensionOf(filters.front().second); !ext.empty())
			dlg->SetDefaultExtension(ext.c_str());
	}
	applyDefaultPath(dlg.Get(), Json::getString(params, L"defaultPath", L""), true);

	auto hr = dlg->Show(owner);
	if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) { finish(result, true, JsonValue::CreateStringValue(L"")); return; }
	if (FAILED(hr)) { Json::setError(result, L"显示保存对话框失败"); return; }

	ComPtr<IShellItem> item;
	if (FAILED(dlg->GetResult(&item)) || !item) { Json::setError(result, L"dialog.error", L"读取保存路径失败"); return; }
	finish(result, false, JsonValue::CreateStringValue(pathOf(item.Get())));
}

void Dialog::msgBox(const JsonObject& params, HWND owner, JsonObject& result)
{
	// 按钮组（低 4 位）和图标（高 4 位）是两组独立的位，各取一个值拼起来
	auto btn = Json::getString(params, L"btn", L"ok");
	UINT type = MB_OK;
	if (btn == L"okCancel") type = MB_OKCANCEL;
	else if (btn == L"yesNo") type = MB_YESNO;
	else if (btn == L"yesNoCancel") type = MB_YESNOCANCEL;
	else if (btn == L"retryCancel") type = MB_RETRYCANCEL;
	else if (btn == L"abortRetryIgnore") type = MB_ABORTRETRYIGNORE;

	auto iconName = Json::getString(params, L"icon", L"info");
	UINT icon = MB_ICONINFORMATION; // 默认 info
	if (iconName == L"warn") icon = MB_ICONWARNING;
	else if (iconName == L"err") icon = MB_ICONERROR;
	else if (iconName == L"question") icon = MB_ICONQUESTION;
	else if (iconName == L"stop") icon = MB_ICONSTOP;

	auto title = Json::getString(params, L"title", L"");
	auto msg = Json::getString(params, L"msg", L"");

	auto ret = MessageBox(owner, msg.c_str(), title.c_str(), type | icon);
	// 下面这些 ID 值互不重叠，所以按返回值映射到统一的小写字符串
	std::wstring answer;
	switch (ret)
	{
	case IDOK: answer = L"ok"; break;
	case IDCANCEL: answer = L"cancel"; break;
	case IDYES: answer = L"yes"; break;
	case IDNO: answer = L"no"; break;
	case IDABORT: answer = L"abort"; break;
	case IDRETRY: answer = L"retry"; break;
	case IDIGNORE: answer = L"ignore"; break;
	case 0: Json::setError(result, L"dialog.showFailed", L"显示消息框失败，GetLastError=" + std::to_wstring(GetLastError())); return;
	default: Json::setError(result, L"dialog.error", L"消息框返回了未知结果：" + std::to_wstring(ret)); return;
	}
	Json::setOk(result);
	result.SetNamedValue(L"data", JsonValue::CreateStringValue(answer));
}

void Dialog::itemInFolder(const JsonObject& params, JsonObject& result)
{
	auto path = Json::getString(params, L"path", L"");
	if (path.empty()) { Json::setError(result, L"dialog.invalidParam", L"缺少 path 参数"); return; }

	// explorer /select,"<文件>"：打开资源管理器并选中该文件。
	// 路径带空格也要能用，所以整体加引号
	auto command = L"/select,\"" + path + L"\"";
	auto ret = reinterpret_cast<INT_PTR>(ShellExecute(nullptr, L"open", L"explorer.exe", command.c_str(), nullptr, SW_SHOW));
	if (ret <= 32) { Json::setError(result, L"dialog.error", L"打开资源管理器失败，错误码=" + std::to_wstring(ret)); return; }
	Json::setOk(result);
}

namespace {
	// inputBox 是同步模态的，同一时刻只可能有一个，所以状态放在文件级变量里就够。
	// 不绕 GWLP_USERDATA：WM_CREATE 阶段它还设不上，反倒多一处时序坑
	constexpr wchar_t PROMPT_CLASS[] = L"WebWinPromptBox";
	HWND promptEdit{ nullptr };
	bool promptOk{ false };
	bool promptDone{ false };
	std::wstring promptText;

	LRESULT CALLBACK promptProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		switch (msg)
		{
		case WM_COMMAND:
			if (LOWORD(wParam) == IDOK)
			{
				wchar_t buf[4096]{};
				GetWindowText(promptEdit, buf, 4096);
				promptText = buf;
				promptOk = true;
				promptDone = true;
			}
			else if (LOWORD(wParam) == IDCANCEL)
			{
				promptOk = false;
				promptDone = true;
			}
			return 0;
		case WM_CLOSE: // 标题栏的叉等同于取消
			promptOk = false;
			promptDone = true;
			return 0;
		}
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}
}

bool Dialog::inputBox(HWND owner, const std::wstring& title, const std::wstring& msg,
	const std::wstring& defaultText, std::wstring& out)
{
	// 窗口类只注册一次
	static const bool registered = [] {
		WNDCLASSW wc{};
		wc.lpfnWndProc = promptProc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = PROMPT_CLASS;
		wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		return RegisterClassW(&wc) != 0;
	}();
	if (!registered) return false;

	constexpr int W = 420, H = 150; // 客户区尺寸
	RECT rc{ 0, 0, W, H };
	AdjustWindowRect(&rc, WS_CAPTION | WS_SYSMENU | WS_POPUP, FALSE);
	const int wndW = rc.right - rc.left;
	const int wndH = rc.bottom - rc.top;

	// 摆到属主窗口正中间；没有属主就摆工作区中间
	POINT at{ 0, 0 };
	RECT area{};
	if (owner && IsWindow(owner)) GetWindowRect(owner, &area);
	else SystemParametersInfo(SPI_GETWORKAREA, 0, &area, 0);
	at.x = area.left + (area.right - area.left - wndW) / 2;
	at.y = area.top + (area.bottom - area.top - wndH) / 2;

	HWND hwnd = CreateWindowW(PROMPT_CLASS, title.c_str(), WS_CAPTION | WS_SYSMENU | WS_POPUP,
		at.x, at.y, wndW, wndH, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
	if (!hwnd) return false;

	auto font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
	auto add = [hwnd, font](LPCWSTR cls, LPCWSTR text, DWORD style,
		int x, int y, int w, int h, HMENU id) {
			HWND c = CreateWindowW(cls, text, style | WS_VISIBLE | WS_CHILD,
				x, y, w, h, hwnd, id, GetModuleHandleW(nullptr), nullptr);
			SendMessage(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
			return c;
		};

	add(L"Static", msg.c_str(), 0, 12, 14, W - 24, 40, nullptr);
	// ES_AUTOHSCROLL：内容超长时横向滚动而不是折行 —— prompt 只该有一行
	promptEdit = add(L"Edit", defaultText.c_str(), WS_BORDER | ES_AUTOHSCROLL,
		12, 62, W - 24, 24, nullptr);
	add(L"Button", L"确定", BS_DEFPUSHBUTTON, W - 180, H - 40, 80, 26, reinterpret_cast<HMENU>(IDOK));
	add(L"Button", L"取消", 0, W - 92, H - 40, 80, 26, reinterpret_cast<HMENU>(IDCANCEL));

	promptOk = false;
	promptDone = false;
	promptText.clear();

	// 模态：禁用属主 → 跑自己的消息泵 → 恢复。和 MessageBox 内部做的是同一件事，
	// 区别只在于我们能在这中间拿到输入框的内容
	if (owner && IsWindow(owner)) EnableWindow(owner, FALSE);
	ShowWindow(hwnd, SW_SHOW);
	SetFocus(promptEdit);

	MSG m{};
	while (!promptDone && GetMessageW(&m, nullptr, 0, 0) > 0)
	{
		// IsDialogMessage 让 Tab 切焦点、Enter 触发默认按钮、Esc 触发取消
		if (!IsDialogMessage(hwnd, &m))
		{
			TranslateMessage(&m);
			DispatchMessage(&m);
		}
	}

	DestroyWindow(hwnd);
	if (owner && IsWindow(owner))
	{
		EnableWindow(owner, TRUE);
		SetForegroundWindow(owner); // 把焦点还回去，否则属主窗口会退到别处后面
	}

	if (!promptOk) return false;
	out = promptText;
	return true;
}
