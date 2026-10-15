#include "Json.h"

#include <winrt/Windows.Foundation.Collections.h>

bool Json::getBool(const JsonObject& obj, const wchar_t* key, bool def)
{
	if (!obj.HasKey(key)) return def;
	auto val = obj.GetNamedValue(key);
	if (val.ValueType() != JsonValueType::Boolean) return def;
	return val.GetBoolean();
}

int Json::getInt(const JsonObject& obj, const wchar_t* key, int def)
{
	if (!obj.HasKey(key)) return def;
	auto val = obj.GetNamedValue(key);
	if (val.ValueType() != JsonValueType::Number) return def;
	return static_cast<int>(val.GetNumber());
}

std::wstring Json::getString(const JsonObject& obj, const wchar_t* key, const std::wstring& def)
{
	if (!obj.HasKey(key)) return def;
	auto val = obj.GetNamedValue(key);
	if (val.ValueType() != JsonValueType::String) return def;
	return { val.GetString().c_str() };
}

std::vector<int> Json::getIntArray(const JsonObject& obj, const wchar_t* key)
{
	std::vector<int> result;
	auto arr = getArray(obj, key);
	if (!arr) return result;
	for (uint32_t i = 0; i < arr.Size(); ++i)
	{
		auto val = arr.GetAt(i);
		if (val.ValueType() == JsonValueType::Number) result.push_back(static_cast<int>(val.GetNumber()));
	}
	return result;
}

JsonObject Json::getObject(const JsonObject& obj, const wchar_t* key)
{
	if (!obj.HasKey(key)) return nullptr;
	auto val = obj.GetNamedValue(key);
	if (val.ValueType() != JsonValueType::Object) return nullptr;
	return val.GetObject();
}

JsonArray Json::getArray(const JsonObject& obj, const wchar_t* key)
{
	if (!obj.HasKey(key)) return nullptr;
	auto val = obj.GetNamedValue(key);
	if (val.ValueType() != JsonValueType::Array) return nullptr;
	return val.GetArray();
}
