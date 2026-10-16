#include "Json.h" //一定要放在前面

#include <winrt/Windows.Foundation.Collections.h>

bool Json::hasKey(const JsonObject& obj, const wchar_t* key)
{
	if (!obj) return false; // 消息里没有 params 字段时 getObject 给的是空对象
	return obj.HasKey(key);
}

bool Json::getBool(const JsonObject& obj, const wchar_t* key, bool def)
{
	if (!obj || !obj.HasKey(key)) return def;
	auto val = obj.GetNamedValue(key);
	if (val.ValueType() != JsonValueType::Boolean) return def;
	return val.GetBoolean();
}

int Json::getInt(const JsonObject& obj, const wchar_t* key, int def)
{
	if (!obj || !obj.HasKey(key)) return def;
	auto val = obj.GetNamedValue(key);
	if (val.ValueType() != JsonValueType::Number) return def;
	return static_cast<int>(val.GetNumber());
}

std::wstring Json::getString(const JsonObject& obj, const wchar_t* key, const std::wstring& def)
{
	if (!obj || !obj.HasKey(key)) return def;
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
	if (!obj || !obj.HasKey(key)) return nullptr;
	auto val = obj.GetNamedValue(key);
	if (val.ValueType() != JsonValueType::Object) return nullptr;
	return val.GetObject();
}

JsonArray Json::getArray(const JsonObject& obj, const wchar_t* key)
{
	if (!obj || !obj.HasKey(key)) return nullptr;
	auto val = obj.GetNamedValue(key);
	if (val.ValueType() != JsonValueType::Array) return nullptr;
	return val.GetArray();
}

void Json::setOk(JsonObject& result)
{
	result.SetNamedValue(L"ok", JsonValue::CreateBooleanValue(true));
}

void Json::setError(JsonObject& result, const std::wstring& message)
{
	// error 暂用字符串承载；等错误码表（T011）定下来再改成 { code, message }
	result.SetNamedValue(L"ok", JsonValue::CreateBooleanValue(false));
	result.SetNamedValue(L"error", JsonValue::CreateStringValue(message));
}
