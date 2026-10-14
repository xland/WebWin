#pragma once

#include <Windows.h>
#include <filesystem>
#include <wrl.h>
#include <WebView2.h>

class App
{
public:
	~App();
	static void init();
	static App* get();
	static std::filesystem::path getDataPath();
private:
	App();
	static void initDispatcherQueueCtrl();
	void initDataPath();
private:
	std::filesystem::path dataPath;
};

