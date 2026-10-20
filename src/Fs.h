#pragma once
#include <string>
// wrl.h 必须在 winrt 头之前（照 Tray.h / Http.h 的顺序）
#include <wrl.h>
#include <Windows.h>
#include <WebView2.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include "Page.h"

using namespace Microsoft::WRL;
using namespace winrt::Windows::Data::Json;

/**
 * fs 域：**文件**读写（对标 Node 的 fsPromises，全部异步）。
 *
 * 按 arch/07-web.md 的分划，**本域只管文件** —— 目录的创建 / 遍历 / 删除
 * 另归一处（arch/01-api.md 第 12 项仍是待办）。所以这里没有 mkdir / readdir / rmdir。
 *
 * **为什么全异步**：框架里没有同步 API。而且文件操作看着快，实际可能很慢 ——
 * 网络盘（UNC）、被杀毒软件盯上的写入、几百 MB 的读取，都能卡住几秒。
 * 跑在 UI 线程上窗口就冻住了，所以一律交给 Async 丢到后台线程。
 *
 * **路径基准**：相对路径一律以 package.json 所在目录（`App::getPackageBaseDir()`）
 * 为底，跟 JS 侧 `path.resolve()` 一致。不能用"进程当前目录"当基准 ——
 * GUI 进程本来就没有有意义的 cwd，而且 Windows 的文件对话框还会悄悄改掉它，
 * 同一个相对路径在弹过一次对话框后可能指向别处。
 */
class Fs
{
public:
	/**
	 * 处理 type=fs 域的消息。返回 true 表示响应由 Async 稍后发回、
	 * 调用方别再发一次；未知方法或当场就错的返回 false
	 */
	static bool handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame = nullptr);

private:
	/** 读文件。encoding='binary' 时字节走共享内存，页面拿到 ArrayBuffer。
	 *  params.chunkSize>0 时走分块流式：配合 JS 侧 callback 逐块下行（见 Fs.ts） */
	static bool readFile(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame);
	/** 读文件的一段：从 start 偏移读 size 字节，返回单块（encoding 同 readFile） */
	static bool readFileChunk(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame);
	/** 写文件（覆盖）。encoding='base64' 时把 data 当 base64 解成字节再写 */
	static bool writeFile(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame);
	/** 追加写；文件不存在时创建 */
	static bool appendFile(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame);
	/** 删文件 */
	static bool unlink(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame);
	/** 复制文件；目标已存在时覆盖（Node 的默认行为） */
	static bool copyFile(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame);
	/** 移动 / 改名；目标已存在时覆盖 */
	static bool rename(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame);
	/** 取文件信息（大小、类型、时间戳） */
	static bool stat(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame);
	/** 检查能不能访问；mode 见 JS 侧 Fs.constants */
	static bool access(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame);
};
