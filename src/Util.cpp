#include <string>
#include <Windows.h>
// CryptBinaryToString / CryptStringToBinary。必须在 Windows.h 之后（同 Tray.h 里 shellapi.h 的规矩）
#include <wincrypt.h>
// CommandLineToArgvW：同样必须在 Windows.h 之后
#include <shellapi.h>

#include "Util.h"

std::filesystem::path Util::getExePath()
{
	// 超长路径（> MAX_PATH）存在，缓冲不够时循环放大再试
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

std::filesystem::path Util::getExeDir()
{
	return getExePath().parent_path();
}

std::wstring Util::utf8ToWide(const std::string& utf8)
{
	if (utf8.empty()) return {};
	auto size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	std::wstring wide(size, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
	return wide;
}

std::string Util::wideToUtf8(const std::wstring& wide)
{
	if (wide.empty()) return {};
	auto size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
	std::string utf8(size, '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), size, nullptr, nullptr);
	return utf8;
}

std::string Util::toBase64(const std::string& bytes)
{
	if (bytes.empty()) return {};
	DWORD need = 0;
	const auto flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
	// 先问长度：need 含结尾的 \0，所以下面要把它去掉
	if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(bytes.data()),
		static_cast<DWORD>(bytes.size()), flags, nullptr, &need)) return {};
	std::string out(static_cast<size_t>(need), '\0');
	if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(bytes.data()),
		static_cast<DWORD>(bytes.size()), flags, out.data(), &need)) return {};
	while (!out.empty() && out.back() == '\0') out.pop_back();
	return out;
}

std::string Util::fromBase64(const std::string& text)
{
	if (text.empty()) return {};
	DWORD need = 0;
	if (!CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()),
		CRYPT_STRING_BASE64, nullptr, &need, nullptr, nullptr)) return {};
	std::string out(static_cast<size_t>(need), '\0');
	if (!CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()),
		CRYPT_STRING_BASE64, reinterpret_cast<BYTE*>(out.data()), &need, nullptr, nullptr)) return {};
	out.resize(need);
	return out;
}

std::wstring Util::winErr(unsigned long code)
{
	LPWSTR buf = nullptr;
	auto n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
	std::wstring msg = (n && buf) ? std::wstring{ buf, n } : std::wstring{};
	if (buf) LocalFree(buf);
	while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' ')) msg.pop_back();
	if (msg.empty()) msg = L"未知错误";
	return msg + L"（错误码 " + std::to_wstring(code) + L"）";
	}

	std::string Util::loadRcdata(const wchar_t* name)
	{
	auto hRes = FindResource(nullptr, name, RT_RCDATA);
	if (!hRes) return {};
	auto hData = LoadResource(nullptr, hRes);
	if (!hData) return {};
	auto data = static_cast<const char*>(LockResource(hData));
	auto size = SizeofResource(nullptr, hRes);
	return { data, size };
	}

	std::wstring Util::quoteArg(const std::wstring& text)
	{
	// 按 MSVCRT 解析 argv 的规则加引号：无空格 / 制表符 / 引号且非空则原样返回；
	// 否则加引号，并转义内部的引号与反斜杠（反斜杠只在紧跟引号时才加倍，末尾反斜杠也要加倍）
	if (!text.empty() && text.find_first_of(L" \t\"") == std::wstring::npos) return text;
	std::wstring out{ L"\"" };
	for (size_t i = 0; i < text.size(); ++i)
	{
	auto slashes = size_t{ 0 };
	while (i < text.size() && text[i] == L'\\') { ++i; ++slashes; }
	if (i == text.size()) { out.append(slashes * 2, L'\\'); break; }
	if (text[i] == L'"') out.append(slashes * 2 + 1, L'\\');
	else out.append(slashes, L'\\');
	out.push_back(text[i]);
	}
	out.push_back(L'"');
	return out;
	}

	std::vector<std::wstring> Util::getCommandLineArgs()
	{
	int argc = 0;
	auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv) return {};
	std::vector<std::wstring> args(argv, argv + argc); // 拷贝后立刻释放，避免调用方忘了 LocalFree
	LocalFree(argv);
	return args;
	}
