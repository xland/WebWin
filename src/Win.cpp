#include "Win.h"
#include "Env.h"

Win::Win(const JsonObject& config)
{
}

Win::~Win()
{
}

void Win::createNativeWindow()
{
	auto clsName = getClsName();
    hwnd = CreateWindowEx(WS_EX_APPWINDOW, clsName.data(), L"", WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_POPUP, 100, 100, 1200, 800, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    auto wvEnv = Env::getEnv();
    auto ctrlReadyCB = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(this, &Win::onCtrlReady);
    wvEnv->CreateCoreWebView2Controller(hwnd, ctrlReadyCB.Get());
}

std::wstring& Win::getClsName()
{
    static std::wstring clsName = [] {
        WNDCLASSEXW wcex;
        wcex.cbSize = sizeof(WNDCLASSEX);
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = &Win::winProc;
        wcex.cbClsExtra = 0;
        wcex.cbWndExtra = 0;
        wcex.hInstance = GetModuleHandle(nullptr);
        wcex.hIcon = LoadIcon(wcex.hInstance, (LPCTSTR)IDI_WINLOGO);
        wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wcex.lpszMenuName = nullptr;
        wcex.lpszClassName = L"WebWin";
        wcex.hIconSm = LoadIcon(wcex.hInstance, (LPCTSTR)IDI_WINLOGO);
        RegisterClassExW(&wcex);
        return wcex.lpszClassName;
        }();
    return clsName;
}

LRESULT Win::winProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto winObj = reinterpret_cast<Win*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
	if (winObj == nullptr) return DefWindowProc(hwnd, msg, wParam, lParam);

    return DefWindowProc(hwnd, msg, wParam, lParam);
}
HRESULT Win::onCtrlReady(HRESULT result, ICoreWebView2Controller* ctrl)
{
    this->ctrl = ctrl;
    ComPtr<ICoreWebView2> webview;
    ctrl->get_CoreWebView2(&webview);
    RECT bounds;
    GetClientRect(hwnd, &bounds);
    ctrl->put_Bounds(bounds);
    //page = std::make_unique<Page>(this, webview);
    return S_OK;
}