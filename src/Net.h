#pragma once
#include <string>
#include <Windows.h>
#include <winrt/Windows.Data.Json.h>

#include "Page.h"

using namespace winrt::Windows::Data::Json;

/**
 * net 域：网络相关的信息与状态。
 *
 * 与窗口无关（单例能力），所以对外只给静态方法 —— 用法见 SDK 里的 `Net`。
 * v1 只给一个"取网卡地址"，以后要加"联网状态变化"、"本机端口占用"之类，
 * 照现在的路子往里加即可，消息协议和通道都不用动。
 *
 * 各方法把结果放进 result 的 data 字段，信封见 Json::setOk / setError
 *（协议见 arch/02-architecture.md）。
 */
class Net
{
public:
	// 处理 type=net 域的消息：按 method 分发到本域的具体动作。
	// frame 是发消息的文档（iframe 或空=顶级文档），本域不用它，只是接口统一
	static void handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame = nullptr);
private:
	/** 取本机网卡的信息（名字、MAC、IP 地址） */
	static void getAddress(JsonObject& result);
};
