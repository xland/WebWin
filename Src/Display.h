#pragma once
#include <string>
#include <Windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <winrt/Windows.Data.Json.h>

#include "Page.h"

using namespace Microsoft::WRL;
using namespace winrt::Windows::Data::Json;

/**
 * display 域：显示设备（显示器）的信息与截图。
 *
 * 与窗口无关（单例能力），所以事件订阅用的窗口 id 是 Win::NO_WINDOW ——
 * 和 os 域一样，事件是全局的（插拔显示器跟窗口没关系）。
 *
 * **坐标口径**：进程是 PerMonitorHighDPIAware，本域所有矩形、坐标、截图尺寸
 * 一律是**物理像素**（不是 CSS 像素）。150% 缩放的 4K 屏，bounds.width 是 3840。
 *
 * 各方法把结果放进 result 的 data 字段，信封见 Json::setOk / setError
 *（协议见 arch/02-architecture.md）。
 */
class Display
{
public:
	// 处理 type=display 域的消息：按 method 分发到本域的具体动作。
	// frame 是发消息的文档（iframe 或空=顶级文档），只有订阅事件时用到。
	// 返回 true 表示**响应已由本域自行发回**（getImage 走共享内存），
	// 调用方不要再发一次 JSON 信封
	static bool handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame = nullptr);
private:
	/** 所有显示器 */
	static void getAll(JsonObject& result);
	/** 主显示器 */
	static void getPrimary(JsonObject& result);
	/** 包含该点的显示器；点落在多屏之间的空隙时取最近的 */
	static void getByPos(const JsonObject& params, JsonObject& result);
	/** 与该矩形重叠面积最大的显示器；完全不重叠时取中心点最近的 */
	static void getByRect(const JsonObject& params, JsonObject& result);
	/** 取屏幕上某点的颜色（#RRGGBB）；不给坐标就取鼠标所在处 */
	static void getColor(const JsonObject& params, JsonObject& result);
	/** 截屏幕的一块区域，PNG 经共享内存发回。返回 true 表示已自行发回 */
	static bool getImage(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame);
	// 订阅 / 取消订阅事件。目前只支持 display-added / display-removed
	static void on(Page* page, const JsonObject& params, JsonObject& result, ICoreWebView2Frame2* frame);
	static void off(Page* page, const JsonObject& params, JsonObject& result, ICoreWebView2Frame2* frame);
};
