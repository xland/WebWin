#pragma once
#include <string>
#include <wrl.h>
#include <WebView2.h>
#include <winrt/Windows.Data.Json.h>

#include "Page.h"

using namespace Microsoft::WRL;
using namespace winrt::Windows::Data::Json;

struct sqlite3; // 不在头文件里拉进 sqlite3.h：只有本域的实现需要它

/**
 * db 域：SQLite 数据库（引擎是内嵌的 src/sqlite/sqlite3.c）。
 *
 * 一个 Db 对象 = 一个 sqlite3 连接。连接是 C++ 侧的资源，过不了消息通道，
 * 所以 JS 侧实际拿到的是它的 id —— 但**开发者不感知**：Db.ts 把它包进实例，
 * 对外只有 `Db.open()` 和实例方法（跟 Win / Menu 一样的形状）。
 *
 * 库文件默认落在 `App::getDataPath()/db` 下。dataPath 同时是 WebView2 的
 * profile 目录（见 Env.cpp），所以必须再垫一层 db/，别跟浏览器数据混在一起。
 *
 * 类型映射（SQLite → JSON）：INTEGER/FLOAT → number，TEXT → string，
 * BLOB → base64 字符串，NULL → null。两个已知取舍：
 * - INTEGER 是 64 位的，转成 JS number（double）超过 2^53 会丢精度
 * - SQLite 没有日期类型，不做自动转换，交给调用方自己定存法
 */
class Db
{
public:
	// 处理 type=db 域的消息：按 method 分发到本域的具体动作
	static void handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame = nullptr);
	// 页面 / iframe 没了：关掉它开的连接，否则 sqlite3* 一直占着文件锁
	static void removeByPage(Page* page);
	static void removeByFrame(Page* page, ICoreWebView2Frame2* frame);
	~Db();
private:
	Db(Page* page, ICoreWebView2Frame2* frame);
	/** 打开（文件不存在则创建）库文件；失败时 err 里是给人看的说明 */
	bool open(const std::wstring& name, std::wstring& err);
	/** 解析库文件路径：空 / 只有文件名 → dataPath/db 下；带路径的按开发者给的原样用 */
	static std::wstring resolvePath(const std::wstring& name);
	/** 开库后立刻加固：防御模式 + 禁用 ATTACH + 忙等待 + WAL */
	static void harden(sqlite3* db);
private:
	int id{ 0 };
	sqlite3* db{ nullptr };
	Page* page{ nullptr };
	ComPtr<ICoreWebView2Frame2> frame; // 空 = 顶级文档
};
