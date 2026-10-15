#pragma once

#include <array>
#include <string>
#include <filesystem>
#include <Windows.h>
#include <wrl.h>
#include <WebView2.h>

class App
{
public:
	~App();
	static void init();
	static App* get();
	static std::filesystem::path getDataPath();
	void readPackageJson();
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

