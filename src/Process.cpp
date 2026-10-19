#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <filesystem>

// JsonArray::Append 是 IVector 的方法，要显式带上集合头文件（同 Os.cpp / Json.cpp）
#include <winrt/Windows.Foundation.Collections.h>

// _beginthreadex：泵线程要长期阻塞在 ReadFile / WaitForSingleObject 上，
// 得自己拿着句柄好"等它收尾"，所以用这一套而不是 std::thread
// （后者 detach 之后就拿不到句柄了）
#include <process.h>

#include "Process.h"
#include "App.h"
#include "Json.h"
#include "Util.h"
#include "Win.h"

namespace
{
	// 本域支持的事件。它们挂在**某个子进程**上（下行消息里带 proc 字段），
	// 不像 os / display 那套是全局事件 —— 同时开着多个子进程时各收各的
	constexpr wchar_t EVENT_STDOUT[] = L"stdout";
	constexpr wchar_t EVENT_STDERR[] = L"stderr";
	constexpr wchar_t EVENT_EXIT[] = L"exit";

	/** 泵线程一次读多少字节 */
	constexpr DWORD CHUNK = 8192;
	/** 等两条管道收尾的上限：孙进程攥着管道时 EOF 不会来，不能无限等 */
	constexpr DWORD PUMP_WAIT_MS = 3000;

	/** 一个已启动的子进程 */
	struct Child
	{
		int id{ 0 };                 // 框架内部分配：下行事件靠它认实例
		unsigned long pid{ 0 };      // 系统进程 id
		HANDLE process{ nullptr };
		HANDLE thread{ nullptr };    // 主线程句柄（等退出用）
		HANDLE outRead{ nullptr };   // 读 stdout 的一端
		HANDLE errRead{ nullptr };   // 读 stderr 的一端
		HANDLE outPump{ nullptr };   // 泵线程句柄：退出前等它把最后一段推完
		HANDLE errPump{ nullptr };
		Page* page{ nullptr };       // 事件推回给谁；为空 = 页面已销毁
		ComPtr<ICoreWebView2Frame2> frame;
		bool detached{ false };
	};

	/** 泵线程的入参；线程函数里 delete */
	struct PumpArg
	{
		int id;
		const wchar_t* name; // 字符串常量，生命周期是整个进程
		HANDLE pipe;
	};

	/** 等退出线程的入参；线程函数里 delete，句柄也由它关 */
	struct WaitArg
	{
		int id;
		HANDLE process;
		HANDLE thread;
		HANDLE outPump;
		HANDLE errPump;
	};

	std::mutex mtx;
	int lastId{ 0 };
	std::map<int, Child> children;

	/**
	 * 按 MSVCRT 解析 argv 的规则给一段文本加引号：
	 * 没有空格 / 制表符 / 引号且不为空就原样返回；否则加引号，并转义里面的引号与反斜杠
	 * —— 反斜杠只在"紧跟一个引号"时才加倍，末尾的反斜杠也要加倍，
	 * 不加的话它会把后面那个收尾引号一起转义掉
	 *
	 * 文件名一律加引号：Program Files 这类带空格的路径不引会被拆成两段
	 */
	std::wstring quote(const std::wstring& text)
	{
		if (!text.empty() && text.find_first_of(L" \t\"") == std::wstring::npos) return text;

		std::wstring out{ L"\"" };
		for (size_t i = 0; i < text.size(); ++i)
		{
			auto slashes = size_t{ 0 };
			while (i < text.size() && text[i] == L'\\') { ++i; ++slashes; }
			if (i == text.size())
			{
				out.append(slashes * 2, L'\\'); // 末尾的反斜杠：后面要跟引号，加倍
				break;
			}
			if (text[i] == L'"') out.append(slashes * 2 + 1, L'\\');
			else out.append(slashes, L'\\');
			out.push_back(text[i]);
		}
		out.push_back(L'"');
		return out;
	}

	/** 末尾"可能被切断的 UTF-8 序列"的字节数（0..3）：留着跟下一段拼起来再解 */
	size_t trailingPartial(const std::string& text)
	{
		for (size_t i = 1; i <= 3 && i <= text.size(); ++i)
		{
			auto byte = static_cast<unsigned char>(text[text.size() - i]);
			if (byte < 0x80) return 0;  // 单字节：后面不可能跟续字节
			if (byte >= 0xC0)            // 起始字节：看它声明的长度够不够
			{
				auto need = byte >= 0xF0 ? 4 : (byte >= 0xE0 ? 3 : 2);
				return need > i ? i : 0;
			}
			// 续字节（0x80..0xBF）：继续往前找起始字节
		}
		return 0;
	}

	/**
	 * 一段输出 → 宽字符串。
	 *
	 * 先按 UTF-8 解；解不动就退回系统 ANSI 代码页（中文 Windows 上是 GBK）——
	 * cmd.exe 这类老程序用的是控制台代码页，不是 UTF-8，全按 UTF-8 解会出一堆替换字符
	 */
	std::wstring decode(const std::string& bytes)
	{
		if (bytes.empty()) return {};
		auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
		if (size > 0)
		{
			std::wstring wide(size, L'\0');
			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
				bytes.data(), static_cast<int>(bytes.size()), wide.data(), size);
			return wide;
		}
		size = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
		std::wstring wide(size, L'\0');
		MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(), size);
		return wide;
	}

	/**
	 * 回到 UI 线程，把一条事件推给"创建这个子进程的那个文档"。
	 *
	 * 两个约束叠加的结果：管道泵跑在后台线程上，而 PostWebMessageAsJson 只能在
	 * UI 线程上调；"页面还在不在"也只有回到 UI 线程查才安全（页面销毁同样发生在
	 * UI 线程，两者互斥）。JSON 也放到那边才构造 —— 不把 winrt 对象带过线程
	 *
	 * @param fill 填 payload 的回调，在 UI 线程上被调一次
	 * @param last 这是该进程的最后一个事件：发完就把它从注册表里摘掉，
	 *             之后再到的输出（比如孙进程还在写）会被丢弃
	 */
	void emit(int id, const wchar_t* name, std::function<void(JsonObject&)> fill, bool last = false)
	{
		App::runLater([id, name, fill = std::move(fill), last] {
			Child child;
			auto alive = false;
			{
				std::lock_guard<std::mutex> lock(mtx);
				auto it = children.find(id);
				if (it != children.end())
				{
					alive = it->second.page != nullptr;
					child = it->second; // 拷一份：持住 frame 的引用，再放锁
					// 最后一个事件发完就摘掉条目，之后再到的数据一律丢弃。
					// 即便页面已经没了也照样摘 —— 不然条目会永久留在表里
					if (last) children.erase(it);
				}
			}
			if (!alive) return; // 页面已被销毁：没人会收到

			JsonObject payload;
			fill(payload);
			JsonObject msg;
			msg.SetNamedValue(L"proc", JsonValue::CreateNumberValue(id)); // Process 域的钩子认这个字段
			msg.SetNamedValue(L"name", JsonValue::CreateStringValue(name));
			msg.SetNamedValue(L"payload", payload);
			child.page->send(msg, child.frame.Get()); // 送回发起 spawn 的那个文档
		});
	}

	/**
	 * 管道泵：一条管道一条线程，阻塞在 ReadFile 上，读到一段就回 UI 线程推事件。
	 *
	 * 为什么不在 UI 线程上读：子进程什么时候输出、有没有输出完全不可控
	 *（有的是常驻进程），阻塞读会把窗口冻住。
	 * 为什么不用 WinRT 线程池：池子上的任务应当"短平快"，长期占着一条会拖慢
	 * 别人的活（http / fs 那些是一次性的，正合适；这里是"一直等"，不合适）
	 */
	unsigned __stdcall pumpProc(void* raw)
	{
		auto arg = static_cast<PumpArg*>(raw);
		auto id = arg->id;
		auto name = arg->name;
		auto pipe = arg->pipe;
		delete arg;

		std::vector<char> buffer(CHUNK);
		std::string pending; // 上一次剩下的、可能是半个字的尾巴
		while (true)
		{
			DWORD read = 0;
			auto ok = ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr);
			if (!ok || read == 0) break; // 出错 / 子进程关了它那一端：管道到头
			pending.append(buffer.data(), read);
			auto keep = trailingPartial(pending);
			auto full = pending.substr(0, pending.size() - keep);
			pending.erase(0, pending.size() - keep);
			if (!full.empty()) emit(id, name, [text = decode(full)](JsonObject& payload) {
				payload.SetNamedValue(L"text", JsonValue::CreateStringValue(text));
			});
		}
		// 子进程可能正好断在半个字上退出了：剩下的照样解一次，
		// 宁可出个替换字符，也别把最后几个字节悄悄丢掉
		if (!pending.empty()) emit(id, name, [text = decode(pending)](JsonObject& payload) {
			payload.SetNamedValue(L"text", JsonValue::CreateStringValue(text));
		});

		CloseHandle(pipe);
		return 0;
	}

	/**
	 * 等子进程退出：拿到退出码后推 exit 事件，再收掉全部句柄。
	 *
	 * 先等两条泵线程收尾再发 exit —— 子进程一退出它那端的管道就关了，
	 * 所以这两条线程多半已经结束；排队（DispatcherQueue 是 FIFO）保证
	 * 页面那边"最后一段输出"一定排在 exit 之前到达
	 */
	unsigned __stdcall waitProc(void* raw)
	{
		auto arg = static_cast<WaitArg*>(raw);
		auto id = arg->id;
		auto process = arg->process;
		auto thread = arg->thread;
		HANDLE pumps[2] = { arg->outPump, arg->errPump };
		delete arg;

		WaitForSingleObject(process, INFINITE);
		unsigned long code = 0;
		GetExitCodeProcess(process, &code);

		// 加上限是因为孙进程可能继承了管道句柄（那样 EOF 永远不来）——
		// 不能因为"还有个孙进程攥着管道"就让调用方一直收不到退出码
		HANDLE waiting[2] = {};
		DWORD count = 0;
		for (auto handle : pumps) if (handle) waiting[count++] = handle;
		if (count) WaitForMultipleObjects(count, waiting, TRUE, PUMP_WAIT_MS);

		emit(id, EVENT_EXIT, [code](JsonObject& payload) {
			payload.SetNamedValue(L"code", JsonValue::CreateNumberValue(static_cast<double>(code)));
		}, true);

		for (auto handle : pumps) if (handle) CloseHandle(handle);
		CloseHandle(thread);
		CloseHandle(process);
		return 0;
	}

	/** 起一条泵线程；失败就把管道关掉，免得没人读、子进程卡在写上面 */
	HANDLE startPump(int id, const wchar_t* name, HANDLE pipe)
	{
		auto arg = new PumpArg{ id, name, pipe };
		auto handle = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, pumpProc, arg, 0, nullptr));
		if (!handle)
		{
			delete arg;
			CloseHandle(pipe);
		}
		return handle;
	}

	/** 起一条等退出的线程 */
	void startWaiter(int id)
	{
		Child child;
		{
			std::lock_guard<std::mutex> lock(mtx);
			auto it = children.find(id);
			if (it == children.end()) return; // 已被清理（页面销毁）：不必再等
			child = it->second;
		}
		auto arg = new WaitArg{ id, child.process, child.thread, child.outPump, child.errPump };
		auto handle = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, waitProc, arg, 0, nullptr));
		if (!handle) delete arg; // 开不了线程：句柄只能留到进程结束（极端情况，不管）
	}
}

void Process::handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	// v1 只有一个方法：静态方法 spawn。事件不在这里订阅 ——
	// C++ 建进程时就替发起方订好了，见 spawn 里的说明
	if (method == L"spawn") spawn(params, page, result, frame);
	else Json::setError(result, L"unknown method: " + method);
}

void Process::spawn(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto file = Json::getString(params, L"file", L"");
	if (file.empty()) { Json::setError(result, L"file 不能为空"); return; }

	auto options = Json::getObject(params, L"options");
	auto hide = options ? Json::getBool(options, L"hide", true) : true;
	auto detached = options ? Json::getBool(options, L"detached", false) : false;

	// 命令行 = 文件名 + 参数，逐个按规则加引号
	std::wstring cmd = quote(file);
	auto args = Json::getArray(params, L"args");
	if (args) for (auto item : args)
	{
		if (item.ValueType() != JsonValueType::String) continue; // 非字符串参数跳过
		cmd.push_back(L' ');
		cmd += quote(item.GetString().c_str());
	}

	// 子进程的工作目录。相对路径以应用基准目录为底 —— 框架里没有 cwd 概念
	//（全库不调 GetCurrentDirectory），跟 fs 域一个口径
	auto cwd = std::filesystem::path{ options ? Json::getString(options, L"cwd", L"") : L"" };
	if (cwd.empty()) cwd = App::getPackageBaseDir();
	else if (cwd.is_relative()) cwd = App::getPackageBaseDir() / cwd;

	SECURITY_ATTRIBUTES secAttr{};
	secAttr.nLength = sizeof(secAttr);
	secAttr.bInheritHandle = TRUE; // 子进程的 std 句柄必须可继承

	HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
	// 子进程的 stdin 给 NUL：GUI 进程自己没有控制台，不特别指定的话子进程拿到的是
	// 无效句柄，有些控制台程序会直接报错。给个空设备最省心
	auto nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		&secAttr, OPEN_EXISTING, 0, nullptr);
	auto pipesOk = CreatePipe(&outRead, &outWrite, &secAttr, 0)
		&& CreatePipe(&errRead, &errWrite, &secAttr, 0);
	if (nul == INVALID_HANDLE_VALUE || !pipesOk)
	{
		if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
		if (outRead) CloseHandle(outRead);
		if (outWrite) CloseHandle(outWrite);
		if (errRead) CloseHandle(errRead);
		if (errWrite) CloseHandle(errWrite);
		Json::setError(result, L"创建管道失败：" + Util::winErr(GetLastError()));
		return;
	}
	// 只有子进程那一端该被继承：读端留在父进程里。
	// 否则子进程握着读端不放，管道永远等不到"写端全关"那一刻（收不到 EOF）
	SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = nul;
	startup.hStdOutput = outWrite;
	startup.hStdError = errWrite;

	PROCESS_INFORMATION procInfo{};
	auto flags = DWORD{ 0 };
	// GUI 应用拉起命令行工具时不该闪黑框 —— 要让用户看见就显式传 hide: false
	if (hide) flags |= CREATE_NO_WINDOW;

	std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end()); // CreateProcess 要求可写缓冲
	cmdBuf.push_back(L'\0');
	auto started = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, flags, nullptr,
		cwd.c_str(), &startup, &procInfo);

	// 写端归子进程：父进程留着只会让"写端全关"永远不成立，同样收不到 EOF
	CloseHandle(outWrite);
	CloseHandle(errWrite);
	CloseHandle(nul);

	if (!started)
	{
		CloseHandle(outRead);
		CloseHandle(errRead);
		Json::setError(result, L"启动失败：" + Util::winErr(GetLastError()) + L"：" + file);
		return;
	}

	Child child;
	child.pid = procInfo.dwProcessId;
	child.process = procInfo.hProcess;
	child.thread = procInfo.hThread;
	child.outRead = outRead;
	child.errRead = errRead;
	child.page = page;
	child.frame = frame;
	child.detached = detached;
	int id = 0;
	{
		std::lock_guard<std::mutex> lock(mtx);
		child.id = id = ++lastId;
		children.emplace(id, std::move(child));
	}

	// 顺序要紧：先把两条管道泵起来，再起等退出的线程。
	// 子进程可能一启动就输出，晚一步开头几行就没了
	HANDLE outPump = startPump(id, EVENT_STDOUT, outRead);
	HANDLE errPump = startPump(id, EVENT_STDERR, errRead);
	{
		std::lock_guard<std::mutex> lock(mtx);
		auto it = children.find(id);
		if (it != children.end())
		{
			it->second.outPump = outPump;
			it->second.errPump = errPump;
		}
	}
	startWaiter(id);

	JsonObject data;
	data.SetNamedValue(L"id", JsonValue::CreateNumberValue(id));
	data.SetNamedValue(L"pid", JsonValue::CreateNumberValue(static_cast<double>(procInfo.dwProcessId)));
	Json::setOk(result);
	result.SetNamedValue(L"data", data);
}

void Process::removeByPage(Page* target)
{
	std::lock_guard<std::mutex> lock(mtx);
	for (auto& kv : children)
	{
		auto& child = kv.second;
		if (child.page != target) continue;
		// 页面没了，它启动的子进程一起结束；detached 的是应用自己要求留下的，不杀
		if (!child.detached) TerminateProcess(child.process, 1);
		child.page = nullptr;  // 断开目标：之后的事件不再推
		child.frame = nullptr;
	}
}

void Process::removeByFrame(Page* target, ICoreWebView2Frame2* targetFrame)
{
	std::lock_guard<std::mutex> lock(mtx);
	for (auto& kv : children)
	{
		auto& child = kv.second;
		if (child.page != target || child.frame.Get() != targetFrame) continue;
		if (!child.detached) TerminateProcess(child.process, 1);
		child.page = nullptr;
		child.frame = nullptr;
	}
}
