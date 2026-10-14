#pragma once
#include <wrl.h>
#include <string>
#include <Windows.h>
#include <WebView2.h>
using namespace Microsoft::WRL;

class Env
{
public:
	~Env();
	static void init();
	static Env* get();
	static ICoreWebView2Environment* getEnv();
private:
	Env();
	void checkWebViewVersion();
	bool checkRegKey(const HKEY& key, const std::wstring& subKey);
	void initWebViewEnv();
	HRESULT onEnvReady(HRESULT result, ICoreWebView2Environment* env);
private:
	ComPtr<ICoreWebView2Environment> webViewEnv;
};

