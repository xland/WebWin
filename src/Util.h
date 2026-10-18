#pragma once
#include <filesystem>
#include <string>

class Util
{
public:
	static std::filesystem::path getExeDir();
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
};
