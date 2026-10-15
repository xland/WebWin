#pragma once
#include <memory>
#include <wrl.h>
#include <string>
#include <Windows.h>
#include <WebView2.h>
#include <winrt/Windows.Data.Json.h>

#include "Page.h"

using namespace Microsoft::WRL;
using namespace winrt::Windows::Data::Json;
class Win{
	friend class Page;
public:
	enum class SizeMode { Normal, Maximize, FullScreen, FullDesktop };
	enum class PosMode { CenterScreen, Absolute };
	static Win* Create(const JsonObject& config);
	static Win* getFirst();
	void createCtrl();
	~Win();
private:
	Win(const JsonObject& config);
	void readConfig(const JsonObject& config);
	void createNativeWindow();
	void applyDwmAttributes();
	std::wstring& getClsName();
	static LRESULT winProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	HRESULT onCtrlReady(HRESULT result, ICoreWebView2Controller* ctrl);
private:
	HWND hwnd;
	ComPtr<ICoreWebView2Controller> ctrl;
	bool ctrlCreating{ false }; // 控制器是否已发起创建（避免重复创建）
	std::unique_ptr<Page> page;

	bool resizable{ true };
	SizeMode sizeMode{ SizeMode::Normal };
	int width{ 1000 };
	int height{ 800 };
	int minWidth{ 250 };
	int minHeight{ 200 };
	int maxWidth{ 0 };   // 0 表示未设置
	int maxHeight{ 0 };
	PosMode posMode{ PosMode::CenterScreen };
	int posX{ 0 };
	int posY{ 0 };
	bool visible{ true };
	bool frame{ true };
	bool shadow{ true };
	bool minimizable{ true };
	bool maximizable{ true };
	bool alwaysOnTop{ false };
	bool taskbar{ true };
	bool borderRadius{ true };
	bool animation{ true };
	bool transparent{ false };
	std::wstring title;          // 空表示使用网页中的 title
	std::wstring webviewPath{ L"." };
	bool allowWindowOpen{ true };
	bool webviewConfigured{ false }; // 是否读到了 webview 配置对象
};

