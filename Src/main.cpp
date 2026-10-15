#include <Windows.h>
#include <shellapi.h>
#include <filesystem>
#include <string>
#include "App.h"

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPTSTR lpCmdLine, _In_ int nCmdShow)
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return -1;

    // 解析命令行：--path=... 指明 package.json 所在目录。
    // CommandLineToArgvW 会按引号规则正确切分，避免手写解析踩到带空格的路径
    int argc = 0;
    if (auto argv = CommandLineToArgvW(GetCommandLineW(), &argc))
    {
        const std::wstring prefix = L"--path=";
        for (int i = 1; i < argc; ++i)
        {
            std::wstring arg{ argv[i] };
            if (arg.rfind(prefix, 0) == 0)
            {
                App::setPackagePath(std::filesystem::path{ arg.substr(prefix.size()) });
                break; // 首个 --path 生效
            }
        }
        LocalFree(argv);
    }

    App::init();
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    CoUninitialize();
    return 0;
}
