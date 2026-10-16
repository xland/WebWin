#pragma once

#include <array>
#include <string>
#include <filesystem>
#include <Windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <winrt/Windows.Data.Json.h>

#include "Page.h"

using namespace winrt::Windows::Data::Json;

class App
{
public:
	~App();
	static void init();
	static App* get();
	// 处理 type=app 域的消息：按 method 分发到本域的具体动作
	static void handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame = nullptr);
	static std::filesystem::path getDataPath();
	// 启动命令行 `--path=...` 指明的 package.json 所在目录，
	// 必须在 init() 之前调用；不调用则走默认（exe 同目录）
	static void setPackagePath(std::filesystem::path path);
	// 实际生效的 package.json 基准目录（命令行为主，否则 exe 同目录）；
	// 入口 HTML 相对路径、webview 资源目录都按它解析
	static std::filesystem::path getPackageBaseDir();
	void readPackageJson();
	// 按读到的配置创建窗口：必须先 readPackageJson()，且数据目录已就绪
	void createWindow();
	const std::wstring& getId() const;
	const std::wstring& getName() const;
	const std::array<int, 3>& getVersion() const;
	bool getQuitWhenNoWindow() const;
private:
	App();
	static void initDispatcherQueueCtrl();
	void initDataPath();
private:
	std::filesystem::path dataPath;
	std::wstring id{ L"webwin" }; // 未配置时的默认应用 ID
	std::wstring name;
	std::array<int, 3> version{ 1, 1, 1 };
	bool quitWhenNoWindow{ true };
};

