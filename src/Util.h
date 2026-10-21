#pragma once
#include <filesystem>
#include <string>
#include <vector>

class Util
{
public:
	static std::filesystem::path getExeDir();
	/** 完整 exe 路径（含文件名）；超长路径也能正确拿到（循环放大缓冲） */
	static std::filesystem::path getExePath();
	static std::wstring utf8ToWide(const std::string& utf8);
	static std::string wideToUtf8(const std::wstring& wide);
	/** base64 编码（不带换行）；用于把二进制内容塞进 JSON 字符串 */
	static std::string toBase64(const std::string& bytes);
	/** base64 解码；输入不合法时返回空串 */
	static std::string fromBase64(const std::string& text);
	/**
	 * 系统错误码转成能看的消息，末尾一定带上数字。
	 *
	 * 带数字是因为很多错误码不在系统消息表里（WinHTTP 那批尤其如此），
	 * FormatMessage 常常查不出来，而那个数字才是能拿去查的东西
	 */
	static std::wstring winErr(unsigned long code);
	/**
	 * 读 exe 内嵌的 RCDATA 资源（当前进程），返回原始字节。
	 * webwin.js / default.html 这类文本资源直接当 string 用即可（见 Res.rc）
	 */
	static std::string loadRcdata(const wchar_t* name);
	/** 按 MSVCRT 规则给命令行参数加引号（文件名 / 含空格 / 含引号的一律加引号） */
	static std::wstring quoteArg(const std::wstring& text);
	/** 解析当前进程的命令行，返回参数数组（含 exe 路径作首元素）；内部负责 LocalFree */
	static std::vector<std::wstring> getCommandLineArgs();
};
