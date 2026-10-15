#pragma once
#include <string>
#include <vector>
#include <winrt/Windows.Data.Json.h>

using namespace winrt::Windows::Data::Json;

class Json
{
public:
	static bool getBool(const JsonObject& obj, const wchar_t* key, bool def);
	static int getInt(const JsonObject& obj, const wchar_t* key, int def);
	static std::wstring getString(const JsonObject& obj, const wchar_t* key, const std::wstring& def);
	static std::vector<int> getIntArray(const JsonObject& obj, const wchar_t* key);
	static JsonObject getObject(const JsonObject& obj, const wchar_t* key);
	static JsonArray getArray(const JsonObject& obj, const wchar_t* key);
};
