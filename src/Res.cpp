#include "Res.h"

#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>
#include <winver.h>

#include "App.h"
#include "Async.h"
#include "Json.h"
#include "Util.h"

// 说明见 Res.h 头部：全异步，写资源要重写整个 PE 文件，不能占着 UI 线程
namespace
{
	// ---------------------------------------------------------------
	// 资源类型
	// ---------------------------------------------------------------

	/**
	 * 资源类型的两种表达：标准类型用整数 id，自定义类型用字符串名。
	 *
	 * 成员**刻意不带默认初始化器** —— 带了就不是聚合类型，
	 * 下面对应的 ResType{...} 初始化就编译不过。
	 */
	struct ResType
	{
		std::wstring name; // isId 为 false 时有效（自定义类型名）
		WORD id;          // isId 为 true 时有效
		bool isId;

		/** 标准类型：传整数 id */
		static ResType byId(WORD v) { return ResType{ L"", v, true }; }
		/** 自定义类型：传字符串类型名 */
		static ResType byName(const std::wstring& v) { return ResType{ v, 0, false }; }
	};

	/** 一个标准资源类型的名字与 id */
	struct KnownType
	{
		LPCWSTR name;
		WORD id;
	};

	/**
	 * 常见标准资源类型名 → id。
	 *
	 * 这里的数字是 PE 资源表里的标准类型号。**不用 RT_* 宏**：那些宏是
	 * `MAKEINTRESOURCE(id)`，类型是 LPWSTR 而不是整数，存不进 WORD。
	 * 转成 lpType 参数的事统一交给下面的 lpType()。
	 *
	 * 故意只列一部分：没列到的按自定义类型名传字符串（PE 资源表本来就允许），
	 * 这样"自定义类型 + 对应资源编辑器"的工作流也能走。
	 */
	const KnownType KNOWN_TYPES[] = {
		{ L"CURSOR", 1 }, { L"BITMAP", 2 }, { L"ICON", 3 }, { L"MENU", 4 },
		{ L"DIALOG", 5 }, { L"STRING", 6 }, { L"FONTDIR", 7 }, { L"FONT", 8 },
		{ L"ACCELERATOR", 9 }, { L"RCDATA", 10 }, { L"MESSAGETABLE", 11 },
		{ L"GROUP_CURSOR", 12 }, { L"GROUP_ICON", 14 }, { L"VERSION", 16 },
		{ L"PLUGPLAY", 19 }, { L"VXD", 20 }, { L"ANICURSOR", 21 }, { L"ANIICON", 22 },
		{ L"HTML", 23 }, { L"MANIFEST", 24 },
	};

	/** 默认资源类型：RCDATA（与 Res.rc 里嵌 SDK 的方式一致） */
	constexpr WORD DEFAULT_TYPE_ID = 10;

	/** 空 → 默认 RCDATA */
	ResType resolveType(const std::wstring& raw)
	{
		if (raw.empty()) return ResType::byId(DEFAULT_TYPE_ID);

		std::wstring upper = raw;
		for (auto& c : upper) c = static_cast<wchar_t>(::towupper(c));
		for (const auto& known : KNOWN_TYPES)
			if (upper == known.name) return ResType::byId(known.id);

		return ResType::byName(raw);
	}

	/** ResType → UpdateResource / FindResource / EnumResourceNames 的 lpType 参数 */
	LPCWSTR lpType(const ResType& t)
	{
		return t.isId ? reinterpret_cast<LPCWSTR>(MAKEINTRESOURCE(t.id)) : t.name.c_str();
	}

	/** 标准类型 id → 名字（反向查 KNOWN_TYPES）。查不到就打数字 */
	std::wstring knownTypeName(WORD id)
	{
		for (const auto& known : KNOWN_TYPES)
			if (known.id == id) return known.name;
		return std::to_wstring(id);
	}

	/**
	 * 资源类型 / 名字 → 可读文本。
	 * 这两个参数传进来的可能是 MAKEINTRESOURCE 造的整数指针，也可能真是字符串指针，
	 * 靠"高位是否非零"区分 —— 这是 Win32 判定资源 id 的标准做法。
	 */
	std::wstring resIdToText(LPCWSTR id)
	{
		ULONG_PTR v = reinterpret_cast<ULONG_PTR>(id);
		if (v >> 16) return id; // 真字符串
		return std::to_wstring(static_cast<WORD>(v));
	}

	// ---------------------------------------------------------------
	// 路径 / 文件
	// ---------------------------------------------------------------

	/** 相对路径 → 绝对路径，基准是 package.json 所在目录（同 Fs 域） */
	std::filesystem::path resolvePath(const std::wstring& p)
	{
		auto path = std::filesystem::path{ p };
		if (!path.has_root_name() && !path.has_root_directory()) return App::getPackageBaseDir() / path;
		return path;
	}

	/** 自身 exe 路径。超长时循环放大缓冲 —— MAX_PATH 之外的路径是存在的 */
	std::filesystem::path selfExe()
	{
		std::wstring buf(MAX_PATH, L'\0');
		for (;;)
		{
			DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
			if (n == 0) return {};
			if (n < buf.size()) { buf.resize(n); break; }
			buf.resize(buf.size() * 2);
		}
		return std::filesystem::path(buf);
	}

	/** 两个路径是否指同一个文件（不区分大小写 + 规范化，绕开 .. 和 8.3 短名） */
	bool sameFile(const std::filesystem::path& a, const std::filesystem::path& b)
	{
		std::error_code ec;
		auto ca = std::filesystem::weakly_canonical(a, ec); if (ec) ca = a;
		auto cb = std::filesystem::weakly_canonical(b, ec); if (ec) cb = b;
		return _wcsicmp(ca.wstring().c_str(), cb.wstring().c_str()) == 0;
	}

	/**
	 * 校验目标 exe。forWrite=true 时额外拦下"改正在运行的自己"这件事。
	 * 不通过时 error 非空。
	 */
	bool checkExe(const std::wstring& raw, std::filesystem::path& out, bool forWrite, std::wstring& error)
	{
		if (raw.empty()) { error = L"exePath 不能为空"; return false; }
		out = resolvePath(raw);

		std::error_code ec;
		if (!std::filesystem::is_regular_file(out, ec))
		{
			error = L"目标 exe 不存在或不是文件：" + out.wstring();
			return false;
		}
		if (forWrite && sameFile(out, selfExe()))
		{
			error = L"不能修改正在运行的这个 exe 自身：Windows 以映像方式映射它，"
			        L"BeginUpdateResource 打开写入会被拒（ERROR_SHARING_VIOLATION）。"
			        L"先复制一份到别的路径再改。目标：" + out.wstring();
			return false;
		}
		return true;
	}

	/** 资源名合法性：非空，且 PE 资源名最长 255 个字符 */
	bool checkResName(const std::wstring& name, std::wstring& error)
	{
		if (name.empty()) { error = L"资源名不能为空"; return false; }
		if (name.size() > 255) { error = L"资源名超过 255 个字符（PE 资源表的上限）"; return false; }
		return true;
	}

	/**
	 * 以**纯数据**方式打开 exe：只当文件映射，不执行 DllMain、不锁成映像。
	 * LOAD_LIBRARY_AS_IMAGE_RESOURCE 让 FindResource / EnumResource* 能正常工作。
	 * 读别的 exe 的资源必须走这条 —— 直接 FindResource(nullptr, ...) 只认自己。
	 */
	HMODULE openDataFile(const std::filesystem::path& exe, std::wstring& error)
	{
		HMODULE mod = LoadLibraryExW(exe.c_str(), nullptr,
			LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
		if (!mod) error = L"打不开目标 exe：" + Util::winErr(GetLastError());
		return mod;
	}

	/** 关掉并置空。必须先关掉再 BeginUpdateResource，否则写不进去 */
	void closeDataFile(HMODULE& mod)
	{
		if (mod) { FreeLibrary(mod); mod = nullptr; }
	}

	std::string readAllBytes(const std::filesystem::path& path, std::wstring& error)
	{
		std::ifstream f(path, std::ios::binary);
		if (!f) { error = L"读不到文件（不存在或没权限）"; return {}; }
		return std::string{ std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
	}

	bool writeAllBytes(const std::filesystem::path& path, const std::string& bytes, std::wstring& error)
	{
		// 不自动建父目录（同 Fs 域：Node 的 writeFile 也不建）
		auto dir = path.parent_path();
		std::error_code ec;
		if (!dir.empty() && !std::filesystem::exists(dir, ec))
		{
			error = L"所在目录不存在（不会自动创建）：" + dir.wstring();
			return false;
		}
		std::ofstream f(path, std::ios::binary | std::ios::trunc);
		if (!f) { error = L"打不开文件写入（没权限或已被别的程序占用）"; return false; }
		if (!bytes.empty()) f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		f.flush();
		if (!f) { error = L"写入失败（磁盘满了？）"; return false; }
		return true;
	}

	// ---------------------------------------------------------------
	// 资源读写
	// ---------------------------------------------------------------

	/**
	 * 打开 exe 的资源更新事务，写一条，然后提交。
	 * data 为空指针、size 为 0 时表示**删除**该资源 —— 这是 Win32 官方的删除方式。
	 *
	 * 中途任何一步失败都必须 EndUpdateResource(TRUE) 丢弃，否则磁盘上那个
	 * PE 会停在半改的状态。
	 */
	bool writeResource(const std::filesystem::path& exe, const std::wstring& name,
		const ResType& type, const char* data, size_t size, std::wstring& error)
	{
		HANDLE hUpdate = BeginUpdateResourceW(exe.c_str(), FALSE);
		if (!hUpdate)
		{
			const DWORD code = GetLastError();
			error = L"BeginUpdateResource 失败：" + Util::winErr(code);
			if (code == ERROR_SHARING_VIOLATION) error += L"（目标正被占用 —— 它是不是正在运行？）";
			return false;
		}
		if (!UpdateResourceW(hUpdate, lpType(type), name.c_str(),
			MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), const_cast<char*>(data), static_cast<DWORD>(size)))
		{
			error = L"UpdateResource 失败：" + Util::winErr(GetLastError());
			EndUpdateResourceW(hUpdate, TRUE); // TRUE = 丢弃本次全部改动
			return false;
		}
		if (!EndUpdateResourceW(hUpdate, FALSE))
		{
			error = L"EndUpdateResource 失败（磁盘满了或文件被占用）：" + Util::winErr(GetLastError());
			return false;
		}
		return true;
	}

	/** 从已打开的 exe 里读一个资源的全部字节。error 非空表示失败 */
	std::string readResourceBytes(HMODULE mod, const std::wstring& name,
		const ResType& type, std::wstring& error)
	{
		HRSRC hRes = FindResourceW(mod, name.c_str(), lpType(type));
		if (!hRes) { error = L"找不到资源（名字或资源类型不对，用 list 看看实际有哪些）"; return {}; }
		HGLOBAL hData = LoadResource(mod, hRes);
		if (!hData) { error = L"LoadResource 失败：" + Util::winErr(GetLastError()); return {}; }

		const auto* p = static_cast<const char*>(LockResource(hData));
		const DWORD size = SizeofResource(mod, hRes);
		if (!p && size) { error = L"LockResource 失败"; return {}; }
		return std::string(p, p + size);
	}

	// ---------------------------------------------------------------
	// 版本信息
	// ---------------------------------------------------------------

	std::wstring fmtVersion(WORD ms, WORD ls)
	{
		return std::to_wstring(HIWORD(ms)) + L"." + std::to_wstring(LOWORD(ms)) + L"."
			+ std::to_wstring(HIWORD(ls)) + L"." + std::to_wstring(LOWORD(ls));
	}

	/** StringFileInfo 里值得读的键。缺哪个就是哪个没有，不算错 */
	const LPCWSTR VERSION_KEYS[] = {
		L"CompanyName", L"FileDescription", L"FileVersion", L"InternalName",
		L"LegalCopyright", L"OriginalFilename", L"ProductName", L"ProductVersion",
		L"Comments", L"LegalTrademarks", L"PrivateBuild", L"SpecialBuild",
	};

	/**
	 * 读 exe 的版本信息，往 data 里放：
	 *   fileVersion / productVersion       —— "1.2.3.4" 文本
	 *   fileVersionRaw / productVersionRaw —— [major, minor, build, revision]
	 *   strings                            —— StringFileInfo 那批字符串键（没有就不出这个字段）
	 *
	 * data 是 JsonObject（agile 类型），后台线程建好、UI 线程取用没问题。
	 */
	bool readVersion(const std::filesystem::path& exe, JsonObject& data, std::wstring& error)
	{
		DWORD handle = 0;
		DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &handle);
		if (size == 0)
		{
			error = L"读不到版本信息（文件里没有版本资源？）：" + Util::winErr(GetLastError());
			return false;
		}
		std::vector<char> buf(size);
		if (!GetFileVersionInfoW(exe.c_str(), handle, size, buf.data()))
		{
			error = L"GetFileVersionInfo 失败：" + Util::winErr(GetLastError());
			return false;
		}

		// 固定信息在根节点 "\" 下，四个 16 位数打包在两个 DWORD 的高低字里
		VS_FIXEDFILEINFO* fixed = nullptr;
		UINT fixedLen = 0;
		if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<LPVOID*>(&fixed), &fixedLen)
			|| !fixed || fixedLen < sizeof(VS_FIXEDFILEINFO))
		{
			error = L"版本信息里没有 VS_FIXEDFILEINFO";
			return false;
		}

		auto putVersion = [&data](const std::wstring& key, DWORD ms, DWORD ls)
		{
			// 注意 CreateStringValue 只吃宽串（hstring），窄的 std::string 喂不进去
			data.SetNamedValue(key.c_str(), JsonValue::CreateStringValue(
				fmtVersion(static_cast<WORD>(ms), static_cast<WORD>(ls))));
			JsonArray arr;
			arr.Append(JsonValue::CreateNumberValue(HIWORD(ms)));
			arr.Append(JsonValue::CreateNumberValue(LOWORD(ms)));
			arr.Append(JsonValue::CreateNumberValue(HIWORD(ls)));
			arr.Append(JsonValue::CreateNumberValue(LOWORD(ls)));
			data.SetNamedValue((key + L"Raw").c_str(), arr);
		};
		putVersion(L"fileVersion", fixed->dwFileVersionMS, fixed->dwFileVersionLS);
		putVersion(L"productVersion", fixed->dwProductVersionMS, fixed->dwProductVersionLS);

		// 字符串表的语言块不能写死（各家 exe 用的语言 / 代码页不一样），
		// 先问 Translation 拿到实际那组，再拼路径去取
		LPVOID trans = nullptr;
		UINT transLen = 0;
		if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", &trans, &transLen)
			|| transLen < sizeof(WORD) * 2)
			return true; // 只有固定信息，也是合法结果

		const auto* t = static_cast<WORD*>(trans);
		wchar_t block[64];
		swprintf_s(block, L"\\StringFileInfo\\%04x%04x", t[0], t[1]);

		JsonObject strings;
		for (auto key : VERSION_KEYS)
		{
			wchar_t sub[128];
			swprintf_s(sub, L"%s\\%s", block, key);
			LPVOID value = nullptr;
			UINT valueLen = 0;
			if (!VerQueryValueW(buf.data(), sub, &value, &valueLen) || !value || valueLen == 0) continue;
			// valueLen 含结尾那个 null，不能算进文本
			strings.SetNamedValue(key, JsonValue::CreateStringValue(
				std::wstring(static_cast<LPCWSTR>(value), valueLen - 1)));
		}
		if (strings.Size() > 0) data.SetNamedValue(L"strings", strings);
		return true;
	}

	// ---------------------------------------------------------------
	// 枚举资源
	// ---------------------------------------------------------------

	/** 一条资源：类型名、名字、字节数。跨线程只传这种纯数据，winrt 对象留到 UI 线程再建 */
	using ResItem = std::tuple<std::wstring, std::wstring, int>;

	struct NameCtx
	{
		std::vector<ResItem>* out;
		LPCWSTR type;         // 原样传回 FindResource
		std::wstring typeName;
	};

	// 两个枚举回调：调用约定要**写字面量 __stdcall**，不能用 WINAPI / CALLBACK 宏。
	// ENUMRESNAMEPROCW 的原型是在 windows.h 阶段展开的（那时宏还是 __stdcall），
	// 而本文件在 Res.h 里引入了 winrt 头之后，WINAPI / CALLBACK 已经被改成 __cdecl
	// —— 写成宏就是 __cdecl，跟 SDK 里那个 __stdcall 原型对不上。
	// 返回值是 int 而不是 BOOL（BOOL 那套也是从 Win16 传下来的习惯）。
	int __stdcall enumNameProc(HMODULE mod, LPCWSTR, LPCWSTR name, LPARAM param)
	{
		auto* ctx = reinterpret_cast<NameCtx*>(param);
		int size = 0;
		if (HRSRC hRes = FindResourceW(mod, name, ctx->type))
			size = static_cast<int>(SizeofResource(mod, hRes));
		ctx->out->emplace_back(ctx->typeName, resIdToText(name), size);
		return TRUE;
	}

		struct TypeCtx
		{
			std::vector<ResItem>* out;
		};

		int __stdcall enumTypeProc(HMODULE mod, LPCWSTR type, LPARAM param)
			{
		auto* ctx = reinterpret_cast<TypeCtx*>(param);
		NameCtx nc;
		nc.out = ctx->out;
		nc.type = type;
		nc.typeName = resIdToText(type);
		// 显式转换到原型类型：SDK 的 ENUMRESNAMEPROCW 第 4 个参数是 LONG_PTR，
		// 和 LPARAM 在 x64 下是同一类型但走的不是同一条 typedef 链，编译器就是判不兼容。
			// x64 只有一种调用约定，转换是无害的
			EnumResourceNamesW(mod, type, reinterpret_cast<ENUMRESNAMEPROCW>(enumNameProc),
				reinterpret_cast<LPARAM>(&nc));
			return TRUE;
			}
}

bool Res::handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	if (method == L"addFile") return addFile(params, page, result, frame);
	if (method == L"addText") return addText(params, page, result, frame);
	if (method == L"get") return get(params, page, result, frame);
	if (method == L"saveToFile") return saveToFile(params, page, result, frame);
	if (method == L"remove") return remove(params, page, result, frame);
	if (method == L"list") return list(params, page, result, frame);
	if (method == L"getInfo") return getInfo(params, page, result, frame);
	return false; // 交给调用方回 unknown method
}

// ===============================================================
// 写
// ===============================================================

bool Res::addFile(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto exeRaw = Json::getString(params, L"exePath", L"");
	auto nameRaw = Json::getString(params, L"name", L"");
	auto fileRaw = Json::getString(params, L"filePath", L"");
	auto typeRaw = Json::getString(params, L"type", L"");

	std::wstring error;
	std::filesystem::path exe;
	if (!checkExe(exeRaw, exe, true, error)) { Json::setError(result, error); return false; }
	if (fileRaw.empty()) { Json::setError(result, L"filePath 不能为空"); return false; }

	auto src = resolvePath(fileRaw);
	std::error_code ec;
	if (!std::filesystem::is_regular_file(src, ec))
	{
		Json::setError(result, L"源文件不存在或不是文件：" + src.wstring());
		return false;
	}
	// 资源名不传就用文件名，跟在资源管理器里往 exe 丢文件的习惯一致
	if (nameRaw.empty()) nameRaw = src.filename().wstring();
	if (!checkResName(nameRaw, error)) { Json::setError(result, error); return false; }

	const std::wstring name = nameRaw;
	const ResType type = resolveType(typeRaw);
	auto started = Async::run(page, result, frame, [exe, src, name, type] {
		std::wstring err;
		auto bytes = readAllBytes(src, err);
		if (!err.empty()) return AsyncResult::fail(err + L"：" + src.wstring());
		if (!writeResource(exe, name, type, bytes.data(), bytes.size(), err))
			return AsyncResult::fail(err + L"（资源 " + name + L"）");
		return AsyncResult::ok(nullptr);
	});
	if (!started) { Json::setError(result, L"创建资源写入任务失败"); return false; }
	return true;
}

bool Res::addText(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto exeRaw = Json::getString(params, L"exePath", L"");
	auto nameRaw = Json::getString(params, L"name", L"");
	auto text = Json::getString(params, L"text", L"");
	auto typeRaw = Json::getString(params, L"type", L"");
	auto encoding = Json::getString(params, L"encoding", L"utf8");

	std::wstring error;
	std::filesystem::path exe;
	if (!checkExe(exeRaw, exe, true, error)) { Json::setError(result, error); return false; }
	if (!checkResName(nameRaw, error)) { Json::setError(result, error); return false; }

	std::string bytes;
	if (encoding == L"base64") bytes = Util::fromBase64(Util::wideToUtf8(text));
	else bytes = Util::wideToUtf8(text);

	const std::wstring name = nameRaw;
	const ResType type = resolveType(typeRaw);
	auto started = Async::run(page, result, frame, [exe, name, type, bytes = std::move(bytes)] {
		std::wstring err;
		if (!writeResource(exe, name, type, bytes.data(), bytes.size(), err))
			return AsyncResult::fail(err + L"（资源 " + name + L"）");
		return AsyncResult::ok(nullptr);
	});
	if (!started) { Json::setError(result, L"创建资源写入任务失败"); return false; }
	return true;
}

bool Res::remove(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto exeRaw = Json::getString(params, L"exePath", L"");
	auto nameRaw = Json::getString(params, L"name", L"");
	auto typeRaw = Json::getString(params, L"type", L"");

	std::wstring error;
	std::filesystem::path exe;
	if (!checkExe(exeRaw, exe, true, error)) { Json::setError(result, error); return false; }
	if (!checkResName(nameRaw, error)) { Json::setError(result, error); return false; }

	const std::wstring name = nameRaw;
	const ResType type = resolveType(typeRaw);
	auto started = Async::run(page, result, frame, [exe, name, type] {
		std::wstring err;
		// data 空 + size 0 是 Win32 官方的"删除这个资源"写法
		if (!writeResource(exe, name, type, nullptr, 0, err))
			return AsyncResult::fail(err + L"（资源 " + name + L"；名字或类型不对的话先用 list 看看）");
		return AsyncResult::ok(nullptr);
	});
	if (!started) { Json::setError(result, L"创建资源删除任务失败"); return false; }
	return true;
}

bool Res::saveToFile(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto exeRaw = Json::getString(params, L"exePath", L"");
	auto nameRaw = Json::getString(params, L"name", L"");
	auto typeRaw = Json::getString(params, L"type", L"");
	auto destRaw = Json::getString(params, L"destPath", L"");

	std::wstring error;
	std::filesystem::path exe;
	if (!checkExe(exeRaw, exe, false, error)) { Json::setError(result, error); return false; }
	if (!checkResName(nameRaw, error)) { Json::setError(result, error); return false; }
	if (destRaw.empty()) { Json::setError(result, L"destPath 不能为空"); return false; }

	const std::wstring name = nameRaw;
	const ResType type = resolveType(typeRaw);
	const auto dest = resolvePath(destRaw);
	auto started = Async::run(page, result, frame, [exe, name, type, dest] {
		std::wstring err;
		HMODULE mod = openDataFile(exe, err);
		if (!mod) return AsyncResult::fail(err);
		auto bytes = readResourceBytes(mod, name, type, err);
		closeDataFile(mod);
		if (!err.empty()) return AsyncResult::fail(err + L"：" + name);
		// 原样落盘：不转码、不做任何加工，要的就是那串字节
		if (!writeAllBytes(dest, bytes, err))
			return AsyncResult::fail(err + L"：" + dest.wstring());
		return AsyncResult::ok(nullptr);
	});
	if (!started) { Json::setError(result, L"创建资源导出任务失败"); return false; }
	return true;
}

// ===============================================================
// 读
// ===============================================================

bool Res::get(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto exeRaw = Json::getString(params, L"exePath", L"");
	auto nameRaw = Json::getString(params, L"name", L"");
	auto typeRaw = Json::getString(params, L"type", L"");
	auto encoding = Json::getString(params, L"encoding", L"utf8");

	// 读不改，目标就是自己也无所谓 —— Res.rc 里嵌的 webwin.js 就是这么读的
	std::wstring error;
	std::filesystem::path exe;
	if (!checkExe(exeRaw, exe, false, error)) { Json::setError(result, error); return false; }
	if (!checkResName(nameRaw, error)) { Json::setError(result, error); return false; }

	const std::wstring name = nameRaw;
	const ResType type = resolveType(typeRaw);
	auto started = Async::run(page, result, frame, [exe, name, type, encoding] {
		std::wstring err;
		HMODULE mod = openDataFile(exe, err);
		if (!mod) return AsyncResult::fail(err);
		auto bytes = readResourceBytes(mod, name, type, err);
		closeDataFile(mod);
		if (!err.empty()) return AsyncResult::fail(err + L"：" + name);

		const auto size = bytes.size();
		// binary：字节走共享内存，页面拿 ArrayBuffer —— 省掉 base64 那 1/3 膨胀
		if (encoding == L"binary")
		{
			return AsyncResult::binary(
				std::vector<uint8_t>(bytes.begin(), bytes.end()),
				[size] {
					JsonObject data;
					data.SetNamedValue(L"size", JsonValue::CreateNumberValue(static_cast<double>(size)));
					return data;
				});
		}
		// base64：二进制内容无损，代价是字符串体积涨 1/3
		if (encoding == L"base64")
			return AsyncResult::ok([text = Util::toBase64(bytes)] {
				return JsonValue::CreateStringValue(Util::utf8ToWide(text));
			});
		// 默认 utf8：内容不是合法 UTF-8 时（GBK 之类）JS 那边会出乱码，
		// 要原样字节就用 base64 / binary
		return AsyncResult::ok([text = Util::utf8ToWide(bytes)] {
			return JsonValue::CreateStringValue(text);
		});
	});
	if (!started) { Json::setError(result, L"创建资源读取任务失败"); return false; }
	return true;
}

bool Res::list(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto exeRaw = Json::getString(params, L"exePath", L"");
	auto typeRaw = Json::getString(params, L"type", L"");

	std::wstring error;
	std::filesystem::path exe;
	if (!checkExe(exeRaw, exe, false, error)) { Json::setError(result, error); return false; }

	// 不传 type 就列全部类型；传了（标准名或自定义名都行）就只列那一类
	const bool onlyThisType = !typeRaw.empty();
	const ResType type = resolveType(typeRaw);
	auto started = Async::run(page, result, frame, [exe, type, onlyThisType] {
		std::wstring err;
		HMODULE mod = openDataFile(exe, err);
		if (!mod) return AsyncResult::fail(err);

		// 后台线程只往纯 C++ 容器里塞，winrt 的 JsonObject 留到 UI 线程再建
		std::vector<ResItem> found;
		if (onlyThisType)
		{
			NameCtx nc;
			nc.out = &found;
			nc.type = lpType(type);
			// 标准类型枚举出来是整数指针，转回名字；自定义类型名本身就是字符串
			nc.typeName = type.isId ? knownTypeName(type.id) : type.name;
			EnumResourceNamesW(mod, nc.type, reinterpret_cast<ENUMRESNAMEPROCW>(enumNameProc),
				reinterpret_cast<LPARAM>(&nc));
		}
		else
		{
			TypeCtx tc{ &found };
			EnumResourceTypesW(mod, reinterpret_cast<ENUMRESTYPEPROCW>(enumTypeProc),
				reinterpret_cast<LPARAM>(&tc));
		}
		closeDataFile(mod);

		return AsyncResult::ok([items = std::move(found)] {
			JsonArray arr;
			for (const auto& [typeName, name, size] : items)
			{
				JsonObject o;
				o.SetNamedValue(L"type", JsonValue::CreateStringValue(typeName));
				o.SetNamedValue(L"name", JsonValue::CreateStringValue(name));
				o.SetNamedValue(L"size", JsonValue::CreateNumberValue(size));
				arr.Append(o);
			}
			JsonObject data;
			data.SetNamedValue(L"items", arr);
			data.SetNamedValue(L"count", JsonValue::CreateNumberValue(static_cast<double>(items.size())));
			return data;
		});
	});
	if (!started) { Json::setError(result, L"创建资源枚举任务失败"); return false; }
	return true;
}

bool Res::getInfo(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto exeRaw = Json::getString(params, L"exePath", L"");

	std::wstring error;
	std::filesystem::path exe;
	if (!checkExe(exeRaw, exe, false, error)) { Json::setError(result, error); return false; }

	// 版本信息只读一个文件、很快，但线程模型跟 Fs 域保持一致，一律走后台线程
	auto started = Async::run(page, result, frame, [exe] {
		std::wstring err;
		JsonObject data;
		if (!readVersion(exe, data, err)) return AsyncResult::fail(err);
		return AsyncResult::ok([data] { return data; });
	});
	if (!started) { Json::setError(result, L"创建版本信息读取任务失败"); return false; }
	return true;
}