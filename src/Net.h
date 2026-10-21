#pragma once
#include <string>
#include <Windows.h>
#include <winrt/Windows.Data.Json.h>

#include "Page.h"

using namespace winrt::Windows::Data::Json;

/**
 * net 域：网络相关的信息与状态 + 命名管道通信。
 *
 * 与窗口无关（单例能力），所以对外给静态方法 / 连接实例 —— 用法见 SDK 里的 `Net`。
 *
 * - `getAddress`：取网卡地址（v1 就有）
 * - `createNamedPipeServer` / `createNamedPipeClient`：命名管道（见下）
 *
 * 命名管道与**将来的 socket 通信在逻辑上隔离**：两者共用本文件的 connId 路由 +
 * 事件下行（net 域隐藏事件），但具体的传输实现各自独立成块，互不直接耦合。
 * 当前只有 pipe 一块；将来加 socket 时新增一块、复用同一套 connId 与事件，pipe 代码不动。
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

	/** 页面销毁时关闭它创建的所有连接，避免读线程空转 / 句柄泄漏 */
	static void removeByPage(Page* page);
private:
	/** 取本机网卡的信息（名字、MAC、IP 地址） */
	static void getAddress(JsonObject& result);

	// ---- 命名管道（与将来的 socket 逻辑隔离：这里只管 pipe）----
	static void createNamedPipeServer(const JsonObject& params, Page* page, JsonObject& result);
	static void createNamedPipeClient(const JsonObject& params, Page* page, JsonObject& result);
	static void sendData(const JsonObject& params, JsonObject& result);
	static void closeConn(const JsonObject& params, JsonObject& result);
};
