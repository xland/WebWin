#pragma once
#include <string>
#include <vector>
#include <winrt/Windows.Data.Json.h>

using namespace winrt::Windows::Data::Json;

/**
 * Json 取值 / 构造的公共工具。
 *
 * 取值一律**容忍空对象**：obj 为空（比如消息里没有 params 字段）时返回默认值，
 * 调用方不必先判空。
 */
class Json
{
public:
	static bool getBool(const JsonObject& obj, const wchar_t* key, bool def);
	static int getInt(const JsonObject& obj, const wchar_t* key, int def);
	static std::wstring getString(const JsonObject& obj, const wchar_t* key, const std::wstring& def);
	static std::vector<int> getIntArray(const JsonObject& obj, const wchar_t* key);
	static JsonObject getObject(const JsonObject& obj, const wchar_t* key);
	static JsonArray getArray(const JsonObject& obj, const wchar_t* key);

	// 下行响应的信封（协议见 arch/02-architecture.md）：{ id, ok, data | error }
	static void setOk(JsonObject& result);
	static void setError(JsonObject& result, const std::wstring& message);
};
