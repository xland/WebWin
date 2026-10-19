#pragma once
#include <string>
#include <Windows.h>
#include <winrt/Windows.Data.Json.h>

#include "Page.h"

using namespace winrt::Windows::Data::Json;

/**
 * os 域：系统级的信息与状态。
 *
 * 与窗口无关（单例能力），所以事件订阅用的窗口 id 是 Win::NO_WINDOW ——
 * win 域的事件绑在某个窗口上，os 域的事件是全局的（锁屏就是全局的）。
 *
 * 各方法把结果放进 result 的 data 字段，信封见 Json::setOk / setError
 *（协议见 arch/02-architecture.md）。
 */
class Os
{
public:
	// 处理 type=os 域的消息：按 method 分发到本域的具体动作。
	// frame 是发消息的文档（iframe 或空=顶级文档），只有订阅事件时用到
	static void handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame = nullptr);
private:
	static void getVersion(JsonObject& result);
	static void getCpuId(JsonObject& result);
	static void getDiskId(JsonObject& result);
	/** 取语言；scope 区分要的是"当前用户"的还是"系统"的 */
	static void getLang(const JsonObject& params, JsonObject& result);
	static void getColor(JsonObject& result);
	static void getIpAddr(JsonObject& result);
	// 取系统常用目录（home / appData / temp / desktop / ...）的绝对路径
	static void getPath(const JsonObject& params, JsonObject& result);
	static void preventSleep(JsonObject& result);
	static void stopPreventSleep(JsonObject& result);
	// 系统凭据管理器（Windows 凭据）：写 / 读 / 删 / 列 key
	static void credWrite(const JsonObject& params, JsonObject& result);
	static void credRead(const JsonObject& params, JsonObject& result);
	static void credDelete(const JsonObject& params, JsonObject& result);
	static void credList(const JsonObject& params, JsonObject& result);
	// 订阅 / 取消订阅事件。目前只支持 osLock（锁屏 / 解锁）
	static void on(Page* page, const JsonObject& params, JsonObject& result, ICoreWebView2Frame2* frame);
	static void off(Page* page, const JsonObject& params, JsonObject& result, ICoreWebView2Frame2* frame);
};
