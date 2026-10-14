# 技术选型与依赖

## 已定选型

| 领域 | 选型 | 说明 |
| --- | --- | --- |
| 窗口 / 系统能力 | Win32 API | 项目只面向 Windows，直接使用 |
| 渲染 / 宿主 | Microsoft Edge WebView2 | 承载前端页面 |
| WebView2 集成方式 | **子窗口（Child HWND）** | **不使用 UI Composition 集成** |
| 前后端通信 | WebView2 `postMessage` / `addEventListener` | 见 `02-architecture.md` |
| **JSON 序列化 / 反序列化** | **WinRT `Windows.Data.Json`**（`winrt/Windows.Data.Json.h`） | **不引入任何外部 JSON 解析库** |
| 语言标准 | C++20 | `/std:c++20`、`/utf-8` |

## WebView2 集成方式（重点决策）

**以子窗口（Child HWND）方式集成 WebView2，不使用 UI Composition 技术集成。**

即：使用基于 HWND 的 `ICoreWebView2Controller`，
**不使用** `ICoreWebView2CompositionController` / `Windows.UI.Composition` 那一套 Visual 宿主方案。

实现要点：

- 创建时传入父窗口 `HWND`（`CreateCoreWebView2Controller(hwndParent, ...)`）
- 父窗口尺寸变化（`WM_SIZE` / `WM_WINDOWPOSCHANGED`）时，
  调用 `ICoreWebView2Controller::put_Bounds` 同步 WebView 区域
- 键盘输入、焦点、DPI、无障碍等由 WebView2 自身在子窗口模式下处理，
  Composition 模式才需要宿主自己转发输入事件

取舍：

| | 子窗口模式（选定） | Composition 模式（不采用） |
| --- | --- | --- |
| 实现复杂度 | 低 | 高（需自建 Visual 树、转发输入） |
| 额外依赖 | 无 | 需 `Windows.UI.Composition` / WinRT 合成栈 |
| 与"小体积 / 高效率" | 一致 | 多一层合成开销 |
| 灵活性 | 受限（见下） | 可做变换、透明、与原生内容自由合成 |

已知限制（**Airspace 问题**）：子窗口是一个独立的 HWND，
它会裁剪同级窗口区域 —— **原生控件无法叠在 WebView 内容之上**，
也难以做透明、裁剪、任意变换等视觉效果。

**处置策略（负责人已定）**：不预设方案、不提前改架构，
**遇到具体问题时临时想办法处理**。
仅在个案确实无法绕过、且必须切换到 Composition 模式时，
才按 `00-overview.md` 的冲突裁决规则上报抉择。

## JSON 方案（重点决策）

**使用 Windows SDK 自带的 `winrt/Windows.Data.Json.h` 完成 JSON 的序列化与反序列化，
不引入外部 JSON 解析库。**

理由：

- 系统自带，零第三方依赖，符合"不引重型依赖 / 小体积"原则（见 `00-overview.md`）
- 消息通道上的 JSON 体量小、结构固定，无需重型解析能力

集成要点（实现阶段按此处理）：

- C++/WinRT 头文件随 Windows SDK 提供（`Include\<sdkver>\winrt\`），
  命名空间为 `winrt::Windows::Data::Json`
- 主要类型：`JsonObject` / `JsonArray` / `JsonValue` / `IJsonValue`
- 使用 WinRT 前需初始化公寓：`winrt::init_apartment(winrt::apartment_type::single_threaded)`
  —— 单线程公寓（STA），与 WebView2 要求 UI 线程为 STA 一致，二者可共用同一模型
- 桌面程序调用 WinRT 需链接 `WindowsApp.lib`

## 性能关注（待实测）

WinRT 对象模型经 COM ABI，每次取值/构造都带引用计数与跨 ABI 调用开销，
通常高于纯头文件的解析库（如 RapidJSON）。

- 消息通道属于**每次 API 调用必经路径**，JSON 处理开销会直接叠加到调用延迟上。
- 已登记为待办事项 **T001**：在通信层成型后做一次序列化/反序列化开销实测。
- 若实测确认成为瓶颈，按 `00-overview.md` 的冲突裁决规则上报抉择，不自行替换方案。
- 详见 `99-todo.md`。

## 明确不引入

- 任何第三方 JSON 解析库（nlohmann/json、RapidJSON、JsonCpp 等）
- 跨平台抽象层 / 框架
