#include <Windows.h>
#include <filesystem>
#include <string>
#include "App.h"
#include "Util.h"

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPTSTR lpCmdLine, _In_ int nCmdShow)
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return -1;

    // 解析命令行：--path=... 指明 package.json 所在目录。
    // CommandLineToArgvW 会按引号规则正确切分，避免手写解析踩到带空格的路径
    auto args = Util::getCommandLineArgs();
    const std::wstring prefix = L"--path=";
    for (size_t i = 1; i < args.size(); ++i)
    {
        std::wstring arg{ args[i] };
        if (arg.rfind(prefix, 0) == 0)
        {
            App::setPackagePath(std::filesystem::path{ arg.substr(prefix.size()) });
            break; // 首个 --path 生效
        }
    }

    App::init();
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    CoUninitialize();
    // WM_QUIT 的 wParam 就是退出码：app.exit(code) 靠它把退出码带出进程
    return static_cast<int>(msg.wParam);
}
