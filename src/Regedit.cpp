#include <cstring>
#include <string>
#include <vector>

// JsonArray::Append 是 IVector 的方法，要显式带上集合头文件（同 Os.cpp / Process.cpp）
#include <winrt/Windows.Foundation.Collections.h>

#include "Regedit.h"
#include "Json.h"
#include "Util.h"

namespace
{
	/** 根键别名表：简写与全名指向同一个 HKEY，两条都认是为了少一个"写错就报错"的坑 */
	struct RootEntry
	{
		const wchar_t* name;
		HKEY key;
	};
	// 不能写 constexpr：HKEY 那几个常量带了指针强转，不是常量表达式
	const RootEntry ROOTS[] = {
		{ L"HKCR", HKEY_CLASSES_ROOT },     { L"HKEY_CLASSES_ROOT", HKEY_CLASSES_ROOT },
		{ L"HKCU", HKEY_CURRENT_USER },     { L"HKEY_CURRENT_USER", HKEY_CURRENT_USER },
		{ L"HKLM", HKEY_LOCAL_MACHINE },    { L"HKEY_LOCAL_MACHINE", HKEY_LOCAL_MACHINE },
		{ L"HKU", HKEY_USERS },              { L"HKEY_USERS", HKEY_USERS },
		{ L"HKCC", HKEY_CURRENT_CONFIG },   { L"HKEY_CURRENT_CONFIG", HKEY_CURRENT_CONFIG },
	};

	/** 根键名 → HKEY；不分大小写。不认识返回 false */
	bool rootOf(const std::wstring& name, HKEY& root)
	{
		auto upper = name;
		for (auto& ch : upper) ch = static_cast<wchar_t>(towupper(ch));
		for (auto& item : ROOTS) if (upper == item.name) { root = item.key; return true; }
		return false;
	}

	/**
	 * 把 `'HKCU\\Software\\MyApp'` 拆成根键 + 子路径。
	 *
	 * 只按**第一个**反斜杠切：根键名里不可能有反斜杠，而子路径里很常见
	 *（所以不能用"最后一个"或者按反斜杠全切开再拼）
	 */
	bool splitPath(const std::wstring& full, HKEY& root, std::wstring& sub)
	{
		auto pos = full.find(L'\\');
		if (!rootOf(pos == std::wstring::npos ? full : full.substr(0, pos), root)) return false;
		sub = pos == std::wstring::npos ? std::wstring{} : full.substr(pos + 1);
		return true;
	}

	/** 一段 POD 拷成字节流：注册表的值一律是字节 */
	template <typename T>
	std::vector<BYTE> pack(T value)
	{
		auto bytes = std::vector<BYTE>(sizeof(T));
		std::memcpy(bytes.data(), &value, sizeof(T));
		return bytes;
	}

	/** "键不存在" / "值不存在"都算"没有"，不当故障 —— 跟 os.credRead 一个口径 */
	bool isNotFound(unsigned long code)
	{
		return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
	}

	/** 本域只认这几种值类型；其余（REG_LINK / REG_RESOURCE_LIST 等）一律不支持 */
	const std::wstring& typeName(DWORD type)
	{
		static const std::wstring unsupported{};
		if (type == REG_SZ) { static const std::wstring name{ L"REG_SZ" }; return name; }
		if (type == REG_EXPAND_SZ) { static const std::wstring name{ L"REG_EXPAND_SZ" }; return name; }
		if (type == REG_MULTI_SZ) { static const std::wstring name{ L"REG_MULTI_SZ" }; return name; }
		if (type == REG_DWORD) { static const std::wstring name{ L"REG_DWORD" }; return name; }
		if (type == REG_QWORD) { static const std::wstring name{ L"REG_QWORD" }; return name; }
		if (type == REG_BINARY) { static const std::wstring name{ L"REG_BINARY" }; return name; }
		return unsupported;
	}

	/**
	 * 字节流 → `data` 字段。
	 *
	 * 注意注册表里的字符串**不保证**以 0 结尾（很多程序写的时候就不带），
	 * 所以一律按字节数截，不靠结尾的 0 判断长度
	 */
	bool valueToJson(DWORD type, const BYTE* data, DWORD size, JsonObject& out)
	{
		auto name = typeName(type);
		if (name.empty()) return false; // 暂不支持的类型，调用方负责报错
		out.SetNamedValue(L"type", JsonValue::CreateStringValue(name));

		if (type == REG_SZ || type == REG_EXPAND_SZ)
		{
			std::wstring text(reinterpret_cast<const wchar_t*>(data), size / sizeof(wchar_t));
			while (!text.empty() && text.back() == L'\0') text.pop_back(); // 去掉末尾多余的 0
			out.SetNamedValue(L"data", JsonValue::CreateStringValue(text));
			return true;
		}
		if (type == REG_MULTI_SZ)
		{
			// 一串以 0 结尾的字符串，整体再补一个 0 收尾
			JsonArray list;
			auto text = reinterpret_cast<const wchar_t*>(data);
			auto count = size / sizeof(wchar_t);
			for (auto i = size_t{ 0 }; i < count;)
			{
				auto start = i;
				while (i < count && text[i] != L'\0') ++i;
				if (i > start) list.Append(JsonValue::CreateStringValue(std::wstring(text + start, i - start)));
				++i; // 跳过这个 0
			}
			out.SetNamedValue(L"data", list);
			return true;
		}
		if (type == REG_DWORD && size >= sizeof(DWORD))
		{
			DWORD number = 0;
			std::memcpy(&number, data, sizeof(DWORD));
			out.SetNamedValue(L"data", JsonValue::CreateNumberValue(static_cast<double>(number)));
			return true;
		}
		if (type == REG_QWORD && size >= sizeof(unsigned long long))
		{
			// JS 的 number 是 double：超过 2^53 会丢精度（跟 SQLite 的 INTEGER 一个毛病）
			unsigned long long number = 0;
			std::memcpy(&number, data, sizeof(number));
			out.SetNamedValue(L"data", JsonValue::CreateNumberValue(static_cast<double>(number)));
			return true;
		}
		if (type == REG_BINARY)
		{
			// JSON 带不了原始字节，所以编成 base64（体积涨 1/3，跟 fs 的 base64 一路）。
			// toBase64 出来的是窄字符串，JSON 那边要宽的，转一道
			auto text = Util::toBase64(std::string(reinterpret_cast<const char*>(data), size));
			out.SetNamedValue(L"data", JsonValue::CreateStringValue(Util::utf8ToWide(text)));
			return true;
		}
		return false; // 理论上到不了：typeName 认的这里都处理了
	}

	/** 按 value 的 JSON 类型挑注册表类型；挑不出来返回 false */
	bool pickType(const IJsonValue& value, DWORD& type, std::vector<BYTE>& bytes)
	{
		if (value.ValueType() == JsonValueType::String)
		{
			auto text = value.GetString();
			type = REG_SZ;
			// 结尾的 0 也要写进去：读值的程序（含注册表编辑器）都指望字符串以 0 结尾。
			// 注意：指针已转成 BYTE*，加减一律按**字节**走，所以长度要乘 sizeof(wchar_t)
			//（不乘的话每个字符只拷半个，字符串会被截掉一半）
			auto begin = reinterpret_cast<const BYTE*>(text.data());
			bytes.assign(begin, begin + (text.size() + 1) * sizeof(wchar_t));
			return true;
		}
		if (value.ValueType() == JsonValueType::Number)
		{
			auto number = static_cast<unsigned long long>(value.GetNumber()); // 负数按补码落位
			// 32 位放得下就写 REG_DWORD：注册表里 DWORD 与 QWORD 是两个类型，
			// 一律写 QWORD 会让读这个值的老程序认不出来
			if (number <= 0xFFFFFFFFull)
			{
				type = REG_DWORD;
				bytes = pack(static_cast<DWORD>(number));
			}
			else
			{
				type = REG_QWORD;
				bytes = pack(number);
			}
			return true;
		}
		return false;
	}

	/** 打开一个键（只读）。code 是出参：调用方据此区分"没有"和"真出错" */
	bool openKey(HKEY root, const std::wstring& sub, HKEY& opened, unsigned long& code)
	{
		code = static_cast<unsigned long>(RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ, &opened));
		return code == ERROR_SUCCESS;
	}

	void setNull(JsonObject& result)
	{
		Json::setOk(result);
		result.SetNamedValue(L"data", JsonValue::CreateNullValue());
	}
}

void Regedit::handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	if (method == L"get") get(params, result);
	else if (method == L"set") set(params, result);
	else if (method == L"deleteKey") deleteKey(params, result);
	else if (method == L"listKeys") listKeys(params, result);
	else if (method == L"listValues") listValues(params, result);
	else Json::setError(result, L"unknown method: " + method);
}

void Regedit::get(const JsonObject& params, JsonObject& result)
{
	HKEY root = nullptr;
	std::wstring sub;
	if (!splitPath(Json::getString(params, L"key", L""), root, sub))
	{ Json::setError(result, L"无法识别的根键"); return; }
	// name 为空（或不传）= 读这个键的默认值，也就是注册表编辑器里那个"(默认)"
	auto name = Json::getString(params, L"name", L"");

	HKEY opened = nullptr;
	auto code = unsigned long{ 0 };
	if (!openKey(root, sub, opened, code))
	{
		if (isNotFound(code)) { setNull(result); return; }
		Json::setError(result, L"打开注册表键失败：" + Util::winErr(code));
		return;
	}

	// 查两遍：第一遍只问"要多大缓冲"，第二遍才取数据 ——
	// 注册表的值长度不可控（REG_BINARY 尤其），没法一次猜准
	auto type = DWORD{ 0 };
	auto size = DWORD{ 0 };
	code = static_cast<unsigned long>(RegQueryValueExW(opened,
		name.empty() ? nullptr : name.c_str(), nullptr, &type, nullptr, &size));
	if (code != ERROR_SUCCESS)
	{
		RegCloseKey(opened);
		if (isNotFound(code)) { setNull(result); return; }
		Json::setError(result, L"读注册表值失败：" + Util::winErr(code));
		return;
	}

	auto buffer = std::vector<BYTE>(size ? size : 1); // 空值也要给个缓冲，免得传 nullptr 进去
	code = static_cast<unsigned long>(RegQueryValueExW(opened,
		name.empty() ? nullptr : name.c_str(), nullptr, &type, buffer.data(), &size));
	RegCloseKey(opened);
	if (code != ERROR_SUCCESS) { Json::setError(result, L"读注册表值失败：" + Util::winErr(code)); return; }

	JsonObject data;
	if (!valueToJson(type, buffer.data(), size, data))
	{ Json::setError(result, L"暂不支持的注册表类型：" + std::to_wstring(type)); return; }

	Json::setOk(result);
	result.SetNamedValue(L"data", data);
}

void Regedit::set(const JsonObject& params, JsonObject& result)
{
	HKEY root = nullptr;
	std::wstring sub;
	if (!splitPath(Json::getString(params, L"key", L""), root, sub))
	{ Json::setError(result, L"无法识别的根键"); return; }
	auto name = Json::getString(params, L"name", L"");
	// 空串留给"读默认值"用（RegSetValueEx 里 nullptr 才是默认值），写的时候分不清，干脆禁掉
	if (name.empty()) { Json::setError(result, L"missing name"); return; }

	auto value = params.TryLookup(L"value");
	if (!value) { Json::setError(result, L"missing value"); return; }

	auto type = DWORD{ 0 };
	auto bytes = std::vector<BYTE>{};
	if (!pickType(value, type, bytes)) { Json::setError(result, L"value 只支持字符串与数字"); return; }

	HKEY opened = nullptr;
	// RegCreateKeyEx 自己会把缺的父键一层层建出来，所以"键不存在时自动创建"不用手写递归
	auto code = static_cast<unsigned long>(RegCreateKeyExW(root, sub.c_str(), 0, nullptr,
		0, KEY_WRITE, nullptr, &opened, nullptr));
	if (code != ERROR_SUCCESS)
	{
		Json::setError(result, L"打开 / 创建注册表键失败：" + Util::winErr(code));
		return;
	}

	code = static_cast<unsigned long>(RegSetValueExW(opened, name.c_str(), 0,
		type, bytes.data(), static_cast<DWORD>(bytes.size())));
	RegCloseKey(opened);
	if (code != ERROR_SUCCESS) { Json::setError(result, L"写注册表值失败：" + Util::winErr(code)); return; }

	Json::setOk(result);
}

void Regedit::deleteKey(const JsonObject& params, JsonObject& result)
{
	HKEY root = nullptr;
	std::wstring sub;
	if (!splitPath(Json::getString(params, L"key", L""), root, sub))
	{ Json::setError(result, L"无法识别的根键"); return; }
	// 不给删根键：真删了就是整台机器的注册表，不是某个应用的数据
	if (sub.empty()) { Json::setError(result, L"不许删根键"); return; }

	// RegDeleteTree 递归删：子键和值一起走
	auto code = static_cast<unsigned long>(RegDeleteTreeW(root, sub.c_str()));
	if (code != ERROR_SUCCESS)
	{
		// 键本来就不在，也算"删完了"
		if (isNotFound(code)) { Json::setOk(result); return; }
		Json::setError(result, L"删注册表键失败：" + Util::winErr(code));
		return;
	}

	Json::setOk(result);
}

void Regedit::listKeys(const JsonObject& params, JsonObject& result)
{
	HKEY root = nullptr;
	std::wstring sub;
	if (!splitPath(Json::getString(params, L"key", L""), root, sub))
	{ Json::setError(result, L"无法识别的根键"); return; }

	HKEY opened = nullptr;
	auto code = unsigned long{ 0 };
	if (!openKey(root, sub, opened, code))
	{
		if (isNotFound(code)) { Json::setOk(result); result.SetNamedValue(L"data", JsonArray{}); return; }
		Json::setError(result, L"打开注册表键失败：" + Util::winErr(code));
		return;
	}

	// 问一次就够：子键个数 + 名字最长多少（这个长度不含结尾的 0）
	DWORD count = 0, maxLen = 0;
	code = static_cast<unsigned long>(RegQueryInfoKeyW(opened, nullptr, nullptr, nullptr,
		&count, &maxLen, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
	if (code != ERROR_SUCCESS)
	{
		RegCloseKey(opened);
		Json::setError(result, L"查询注册表键信息失败：" + Util::winErr(code));
		return;
	}

	JsonArray list;
	std::vector<wchar_t> buffer(maxLen + 1);
	for (DWORD i = 0; i < count; ++i)
	{
		auto len = static_cast<DWORD>(buffer.size());
		code = static_cast<unsigned long>(RegEnumKeyExW(opened, i, buffer.data(), &len,
			nullptr, nullptr, nullptr, nullptr));
		// 枚举期间键被别的程序删了就跳过：拿到的本来就是"某一瞬间"的快照
		if (code != ERROR_SUCCESS) continue;
		list.Append(JsonValue::CreateStringValue(std::wstring(buffer.data(), len)));
	}
	RegCloseKey(opened);

	Json::setOk(result);
	result.SetNamedValue(L"data", list);
}

void Regedit::listValues(const JsonObject& params, JsonObject& result)
{
	HKEY root = nullptr;
	std::wstring sub;
	if (!splitPath(Json::getString(params, L"key", L""), root, sub))
	{ Json::setError(result, L"无法识别的根键"); return; }

	HKEY opened = nullptr;
	auto code = unsigned long{ 0 };
	if (!openKey(root, sub, opened, code))
	{
		if (isNotFound(code)) { Json::setOk(result); result.SetNamedValue(L"data", JsonArray{}); return; }
		Json::setError(result, L"打开注册表键失败：" + Util::winErr(code));
		return;
	}

	DWORD count = 0, maxLen = 0;
	code = static_cast<unsigned long>(RegQueryInfoKeyW(opened, nullptr, nullptr, nullptr,
		nullptr, nullptr, nullptr, &count, &maxLen, nullptr, nullptr, nullptr));
	if (code != ERROR_SUCCESS)
	{
		RegCloseKey(opened);
		Json::setError(result, L"查询注册表键信息失败：" + Util::winErr(code));
		return;
	}

	JsonArray list;
	std::vector<wchar_t> buffer(maxLen + 1);
	for (DWORD i = 0; i < count; ++i)
	{
		auto len = static_cast<DWORD>(buffer.size());
		code = static_cast<unsigned long>(RegEnumValueW(opened, i, buffer.data(), &len,
			nullptr, nullptr, nullptr, nullptr));
		if (code != ERROR_SUCCESS) continue;
		// 默认值（注册表编辑器里那个"(默认)"）的名字是空串 —— 也列出来
		list.Append(JsonValue::CreateStringValue(std::wstring(buffer.data(), len)));
	}
	RegCloseKey(opened);

	Json::setOk(result);
	result.SetNamedValue(L"data", list);
}
