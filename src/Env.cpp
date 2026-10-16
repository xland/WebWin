#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include "Env.h"
#include "App.h"
#include "Win.h"

namespace {
	std::unique_ptr<Env> env;
}

Env::Env()
{

}

Env::~Env()
{
}

void Env::init()
{
	if (!env)
	{
        Env* rawPtr = new Env();
        env = std::unique_ptr<Env>(rawPtr);
        rawPtr->checkWebViewVersion();
        rawPtr->initWebViewEnv();
	}
}

Env* Env::get()
{
	return env.get();
}

void Env::checkWebViewVersion()
{
    std::wstring regSubKey = L"\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
    bool hasRuntime = checkRegKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node" + regSubKey);
    if (hasRuntime) return;
    hasRuntime = checkRegKey(HKEY_CURRENT_USER, L"Software" + regSubKey);
    if (!hasRuntime) {
        auto hr = MessageBox(nullptr, L"需要安装 WebView2 运行时才能继续使用。\n\n是否立即跳转到下载页面？", L"系统提示", MB_YESNO | MB_ICONQUESTION);
        if (hr == IDYES) {
            std::wstring downloadUrl{ L"https://go.microsoft.com/fwlink/p/?LinkId=2124703" };
            ShellExecute(nullptr, L"open", downloadUrl.data(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ExitProcess(-1);
    }
}

bool Env::checkRegKey(const HKEY& key, const std::wstring& subKey)
{

    DWORD valueSize = 0;
    auto rc = RegGetValue(key, subKey.c_str(), L"pv", RRF_RT_REG_SZ, nullptr, nullptr, &valueSize);
    if (rc != ERROR_SUCCESS && rc != ERROR_MORE_DATA) return false;
    if (valueSize <= sizeof(wchar_t)) return false;
    std::wstring valueBuf;
    valueBuf.resize(valueSize / sizeof(wchar_t));
    rc = RegGetValueW(key, subKey.c_str(), L"pv", RRF_RT_REG_SZ, nullptr, valueBuf.data(), &valueSize);
    if (rc != ERROR_SUCCESS) return false;
    valueSize /= sizeof(wchar_t);
    if (valueSize > 0) valueBuf.resize(valueSize - 1); //去掉终止符
    if (valueBuf.empty()) return false;
    std::wstringstream ss(valueBuf.data());
    std::vector<int> versions;
    std::wstring segment;
    while (std::getline(ss, segment, L'.'))
    {
        if (!segment.empty())
        {
            versions.push_back(std::stoi(segment)); // 宽字符转整数，结尾处有非数字字符也没有问题
        }
    }
    // 注意：这是**运行时浏览器版本**（对应 SDK 1.0.4258.31），不是 SDK 版本号。
    // 两个口径别混：注册表 pv 里读出来的是前者
    std::vector<int> minVersion = { 155, 0, 4283, 45 };
    if (versions < minVersion) return false;
    return true;
}
ICoreWebView2Environment* Env::getEnv()
{
	if (!env) return nullptr;
    return env->webViewEnv.Get();
}

void Env::initWebViewEnv()
{
    auto envReadyCB = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(this, &Env::onEnvReady);
    CreateCoreWebView2EnvironmentWithOptions(nullptr,App::getDataPath().c_str(), nullptr, envReadyCB.Get());
}

HRESULT Env::onEnvReady(HRESULT result, ICoreWebView2Environment* env)
{
    if (FAILED(result)) {
        MessageBox(nullptr, L"WebView2环境初始化失败", L"系统提示", MB_OK);
        ExitProcess(-1);
    }
    webViewEnv = env;
    // 配置与窗口已在 App::init 中就绪，环境一好就把控制器补上
    if (auto win = Win::getFirst()) win->createCtrl();
    return S_OK;
}