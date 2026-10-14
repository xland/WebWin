#pragma once

#include <Windows.h>
#include <filesystem>
#include <wrl.h>
#include <WebView2.h>

using namespace Microsoft::WRL;

class App
{
public:
	~App();
	static void init();
	static App* get();
	static std::filesystem::path getDataPath();
	static ICoreWebView2Environment* getWebViewEnv();
private:
	App();
	static void initDispatcherQueueCtrl();
	void checkWebViewVersion();
	bool checkRegKey(const HKEY& key, const std::wstring& subKey);
	void initDataPath();
	void initWebViewEnv();
	HRESULT onEnvReady(HRESULT result, ICoreWebView2Environment* env);
private:
	std::filesystem::path dataPath;
	ComPtr<ICoreWebView2Environment> webViewEnv;
};

