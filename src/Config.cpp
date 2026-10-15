#include <memory>
#include <fstream>
#include <string>
#include <Windows.h>
#include "Config.h"
#include "Win.h"
#include "Json.h"

using namespace winrt::Windows::Data::Json;

namespace {
	std::unique_ptr<Config> config;

	std::wstring getPackageJsonPath()
	{
		std::wstring exePath(MAX_PATH, L'\0');
		auto len = GetModuleFileName(nullptr, exePath.data(), static_cast<DWORD>(exePath.size()));
		exePath.resize(len);
		return exePath.substr(0, exePath.find_last_of(L'\\')) + L"\\package.json";
	}

	std::wstring utf8ToWide(const std::string& utf8)
	{
		if (utf8.empty()) return {};
		auto size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
		std::wstring wide(size, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
		return wide;
	}
}

Config::Config()
{
	readPackageJson();
}

Config::~Config()
{
}

void Config::init()
{
	if (!config)
	{
		Config* rawPtr = new Config();
		config = std::unique_ptr<Config>(rawPtr);
	}
}

Config* Config::get()
{
	return config.get();
}

const std::wstring& Config::getId() const
{
	return id;
}

const std::wstring& Config::getName() const
{
	return name;
}

const std::array<int, 3>& Config::getVersion() const
{
	return version;
}

bool Config::getQuitWhenNoWindow() const
{
	return quitWhenNoWindow;
}

void Config::readPackageJson()
{
	JsonObject window;
	std::ifstream file(getPackageJsonPath(), std::ios::binary);
	if (file) // package.json 不存在时保持各字段默认值，window 为空对象
	{
		std::string content{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
		JsonObject json = JsonObject::Parse(utf8ToWide(content));

		// 有 webwin 就用 webwin 对象，没有就把 package.json 本身当作 webwin 内容
		JsonObject webwin = Json::getObject(json, L"webwin");
		if (!webwin) webwin = json;

		id = Json::getString(webwin, L"id", id);
		name = Json::getString(webwin, L"name", name);
		quitWhenNoWindow = Json::getBool(webwin, L"quitWhenNoWindow", quitWhenNoWindow);
		auto windowObj = Json::getObject(webwin, L"window");
		if (windowObj) window = windowObj;

		auto vers = Json::getIntArray(webwin, L"version");
		if (vers.size() >= 3) version = { vers[0], vers[1], vers[2] };
	}
	Win::Create(window);
}
