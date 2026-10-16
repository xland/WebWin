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

## WebView2 版本约定

**最低版本：1.0.4258.31**（暂定）。这是相当新的版本，因此：

- 可以、也**应当**优先使用较新的 API（例如带 `WithRequestSourceKinds` 后缀的新接口）
- **不使用实验性（Experimental）API** —— 它们可能随版本变更
- 遇到"老接口已废弃"的情况，一律换到新接口

> 注意：`Env::checkWebViewVersion()` 检查的是**运行时浏览器版本**，
> 与这里的 SDK 版本是两个口径，别混：

| 口径 | 值 | 出处 |
| --- | --- | --- |
| SDK 版本 | `1.0.4258.31` | 本节的约定 |
| 运行时浏览器版本 | `155.0.4283.45` | `Env::checkRegKey()` 的 `minVersion` |

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

### 已遇到的个案：`restore` 后 `:hover` 残留

**现象**：点击自定义标题栏的 restore 按钮后窗口还原，鼠标已不在按钮上，
但按钮的 `:hover` 效果仍在。

**成因**：`:hover` 由浏览器进程的命中测试维护。窗口几何通过 Win32
（`ShowWindow` / `SetWindowPos`）改变时鼠标没动，没有鼠标消息送进 WebView，
浏览器不会重算命中，`:hover` 就留在原元素上。

**已定处置**：窗口几何变化后，由 C++ 通知浏览器"鼠标已离开" ——
`Win::resetHoverState()` 做两件事：

1. `ICoreWebView2Controller::NotifyParentWindowPositionChanged()`（官方 API）
2. 按 Chromium 窗口类名 `Chrome_WidgetWin_0` / `Chrome_RenderWidgetHostHWND`
   找到 WebView 的子窗口，投递 `WM_MOUSELEAVE`

触发点（**每条路径只清一次**，不重复劳动）：

| 场景 | 谁来清 | 时机 |
| --- | --- | --- |
| `minimize` / `hide` | `handleApi` 对应分支 | 状态改变**之前** |
| `show` / `move` | `handleApi` 对应分支 | 状态改变**之后** |
| `maximize` / `restore` / `resize` | `WM_SIZE`（同步触发） | 状态改变之后 |
| 原生最小化（标题栏 / 系统菜单 / 任务栏） | `WM_SYSCOMMAND` 的 `SC_MINIMIZE` | 窗口隐藏**之前** |
| 拖边缘 / 双击标题栏 / Win+方向键 | `WM_SIZE` | 状态改变之后 |
| `close` / `destroy` | 不清（窗口没了） | — |

两个"为什么"：

- **`minimize` / `hide` 要提前**：窗口一旦隐藏，浏览器很可能不再处理鼠标消息，
  那时再发 `WM_MOUSELEAVE` 是白发
- **`WM_SIZE` 要跳过 `SIZE_MINIMIZED`**：最小化在 `WM_SYSCOMMAND` /
  `handleApi` 里已经清过，且那两次时机更早、更可靠

**JS 侧改样式无效（已实测排除）**：给元素设 `pointer-events: none`、
或 `display` 切走再切回，都不能清掉 `:hover` —— 命中测试在浏览器进程，
渲染进程的样式改不动它。这条别再试了。

**风险**：窗口类名是 Chromium 内部实现，WebView2 未公开。
所以实现成"尽力而为"：找不到就静默跳过，退化为未修复时的行为，不会更糟。
Debug 下找不到会 `OutputDebugString` 提示。

### iframe 与 C++ 的通信（额外兼容逻辑）

**问题**：`CoreWebView2.WebMessageReceived` **只接收顶级文档**的消息 ——
iframe 里调 `chrome.webview.postMessage`，C++ 收不到。
`CoreWebView2.PostWebMessageAsJson` 同理，只发给顶级文档。

**处置**：以"文档"为粒度收发消息（一个文档 = 页面 + 页面里的某个 iframe）：

| 方向 | 顶级文档 | iframe |
| --- | --- | --- |
| 页面 → C++ | `CoreWebView2::WebMessageReceived` | `ICoreWebView2Frame2::add_WebMessageReceived` |
| C++ → 页面 | `CoreWebView2::PostWebMessageAsJson` | `ICoreWebView2Frame2::PostWebMessageAsJson` |

实现要点：

- `ICoreWebView2_4::add_FrameCreated` 发现 iframe，逐个订阅它的消息与销毁事件
- 结果与事件都发回**发消息 / 订阅的那个 frame**（`Page::postJson`）
- 订阅表按"文档"记录（`Win::EventSub` = 页面 + frame）；
  frame 销毁时由 `Win::unsubscribeFrame` 摘掉它的订阅，避免往失效 frame 推事件
- SDK 注入用的 `AddScriptToExecuteOnDocumentCreated`
  **对所有顶级文档和子框架导航生效**，所以 iframe 里同样有 `win` 对象，无需额外处理

**顺带换掉一个废弃接口**：`AddWebResourceRequestedFilter` 已废弃，
且只对主框架和同源 iframe 生效；已改为
`AddWebResourceRequestedFilterWithRequestSourceKinds`（`ICoreWebView2_22`）。

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
