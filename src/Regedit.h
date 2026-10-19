#pragma once
#include <string>
#include <Windows.h>
#include <winrt/Windows.Data.Json.h>

#include "Page.h"

using namespace winrt::Windows::Data::Json;

/**
 * regedit 域：读写 Windows 注册表。
 *
 * 与窗口无关（单例能力）。键路径写成一整条，跟 reg.exe 一个写法：
 * `'HKCU\\Software\\MyApp'` —— 根键那节的简写与全名都认，不分大小写。
 *
 * v1 只给五个基础方法（get / set / deleteKey / listKeys / listValues），
 * 以后要加"键是否存在"、"重命名"、"权限"之类，照现在的路子往里加即可，
 * 消息协议和通道都不用动。
 *
 * 各方法把结果放进 result 的 data 字段，信封见 Json::setOk / setError
 *（协议见 arch/02-architecture.md）。
 */
class Regedit
{
public:
	// 处理 type=regedit 域的消息：按 method 分发到本域的具体动作。
	// frame 是发消息的文档（iframe 或空=顶级文档），本域不用它，只是接口统一
	static void handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame = nullptr);
private:
	/** 读一个值 → { type, data }；键或值不存在 → null */
	static void get(const JsonObject& params, JsonObject& result);
	/** 写一个值；键不存在时逐层创建。value 是字符串 → REG_SZ，数字 → REG_DWORD / REG_QWORD */
	static void set(const JsonObject& params, JsonObject& result);
	/** 删掉整个键（含子键与值） */
	static void deleteKey(const JsonObject& params, JsonObject& result);
	/** 列出该键下的子键名 */
	static void listKeys(const JsonObject& params, JsonObject& result);
	/** 列出该键下的值名 */
	static void listValues(const JsonObject& params, JsonObject& result);
};
