#include <string>
#include <Windows.h>

#include "Util.h"

std::filesystem::path Util::getExeDir()
{
	std::wstring exePath(MAX_PATH, L'\0');
	auto len = GetModuleFileName(nullptr, exePath.data(), static_cast<DWORD>(exePath.size()));
	exePath.resize(len);
	return std::filesystem::path(exePath).parent_path();
}

std::wstring Util::utf8ToWide(const std::string& utf8)
{
	if (utf8.empty()) return {};
	auto size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	std::wstring wide(size, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
	return wide;
}
