#pragma once
#include <filesystem>
#include <string>

class Util
{
public:
	static std::filesystem::path getExeDir();
	static std::wstring utf8ToWide(const std::string& utf8);
	static std::string wideToUtf8(const std::wstring& wide);
};
