#pragma once
#include <string>
// wrl.h 必须在 winrt 头之前（照 Fs.h / Win.h 的顺序）
#include <wrl.h>
#include <Windows.h>
#include <WebView2.h>
#include <winrt/Windows.Data.Json.h>

#include "Page.h"

using namespace Microsoft::WRL;
using namespace winrt::Windows::Data::Json;

/**
 * res 域：**exe 里的资源**读写，以及 exe 文件的版本信息。
 *
 * 底层就是 Win32 那套：写用 BeginUpdateResource / UpdateResource / EndUpdateResource，
 * 读用 LoadLibraryEx(LOAD_LIBRARY_AS_DATAFILE) + FindResource / EnumResourceNames，
 * 版本信息用 GetFileVersionInfo + VerQueryValue。
 *
 * **全部异步**：资源可能几 MB（我们自己的 exe 就内嵌了整个 SDK），读写都要碰磁盘，
 * 而写资源要**重写整个 PE 文件**，跑在 UI 线程上窗口会僵住。
 *
 * **改不了正在运行的那个 exe**：Windows 以映像方式映射运行中的可执行文件，
 * 那个文件句柄只声明了 FILE_SHARE_READ | FILE_SHARE_DELETE，
 * BeginUpdateResource 内部的 GENERIC_WRITE 打开必然撞 ERROR_SHARING_VIOLATION。
 * 所以 addFile / addText / remove 的 exePath 必须指向别的文件（安装包、待分发的目标程序），
 * 或者先复制一份出来再改。碰到目标是自身时这里会**提前**给出明确错误，
 * 而不是让调用方去猜一个天书般的 sharing violation。
 *
 * **资源类型**：默认 RT_RCDATA，可传字符串类型名（'ICON' / 'HTML' / 'MANIFEST' …）。
 * 不认识的名字不当错误 —— PE 资源表允许任意字符串类型名，原样传下去即可。
 *
 * **相对路径**以 package.json 所在目录为底，跟 Fs 域和 JS 的 path.resolve() 一致。
 */
class Res
{
public:
	/**
	 * 处理 type=res 域的消息。返回 true 表示响应由 Async 稍后发回、
	 * 调用方别再发一次；未知方法或当场就错的返回 false
	 */
	static bool handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame = nullptr);

private:
	/** 把一个文件整个塞进 exe。资源名不传时默认取文件名 */
	static bool addFile(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame);
	/** 把一段文本塞进 exe（encoding='base64' 时按 base64 解码） */
	static bool addText(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame);
	/** 读资源内容。encoding = utf8 | base64 | binary（binary 走共享内存） */
	static bool get(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame);
	/** 把资源**原样**写成文件（不转码、不做任何加工） */
	static bool saveToFile(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame);
	/** 删掉一个资源 */
	static bool remove(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame);
	/** 列出 exe 里的资源（类型 + 名字 + 字节数）。传 type 则只列该类型 */
	static bool list(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame);
	/**
	 * 读 exe 的版本信息块（`VS_VERSIONINFO`）：固定版本号 + `StringFileInfo` 字符串表。
	 * 方法叫 getInfo 不叫 getVersion，是因为返回的远不止版本号 ——
	 * CompanyName / FileDescription / ProductName 这些文案也在里面
	 */
	static bool getInfo(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame);
};