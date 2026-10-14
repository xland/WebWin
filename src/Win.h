#pragma once
#include <wrl.h>
#include <string>
#include <Windows.h>
#include <WebView2.h>
#include <winrt/Windows.Data.Json.h>

using namespace Microsoft::WRL;
using namespace winrt::Windows::Data::Json;
class Win
{
	friend class Page;
public:
	Win(const JsonObject& config);
	~Win();
private:
	void createNativeWindow();
	std::wstring& getClsName();
	static LRESULT winProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	HRESULT onCtrlReady(HRESULT result, ICoreWebView2Controller* ctrl);
private:
	HWND hwnd;
	ComPtr<ICoreWebView2Controller> ctrl;
	//std::unique_ptr<Page> page;
};

