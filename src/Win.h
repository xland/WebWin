#pragma once
#include <wrl.h>
#include <string>
#include <Windows.h>
#include <WebView2.h>
#include <winrt/Windows.Data.Json.h>

using namespace Microsoft::WRL;
using namespace winrt::Windows::Data::Json;
class Win{
	friend class Page;
public:
	enum class SizeMode { Normal, Maximize, FullScreen, FullDesktop };
	enum class PosMode { CenterScreen, Absolute };
	static Win* Create(const JsonObject& config);
	~Win();
private:
	Win(const JsonObject& config);
	void readConfig(const JsonObject& config);
	void createNativeWindow();
	std::wstring& getClsName();
	static LRESULT winProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	HRESULT onCtrlReady(HRESULT result, ICoreWebView2Controller* ctrl);
private:
	HWND hwnd;
	ComPtr<ICoreWebView2Controller> ctrl;
	//std::unique_ptr<Page> page;

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
	bool transparent{ false };
	std::wstring title;          // 空表示使用网页中的 title
	std::wstring webviewPath{ L"." };
	bool allowWindowOpen{ true };
};

