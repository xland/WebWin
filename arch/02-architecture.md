# 架构设计：分层、通信模型与 JS API 层

## 设计目标

1. **绝大多数场景下，开发者只写 HTML / CSS / JS 就能完成一个完整的桌面应用。**
   C++ 侧由框架提供，开发者通常不需要写宿主代码。
2. **框架提供"完成一个完整桌面应用"所需的全部 API。**
   即：窗口、菜单、对话框、文件系统、系统托盘、剪贴板、通知、进程、配置存储等，
   凡是桌面应用该有的能力，都由框架以 API 形式暴露给 JS。
3. 开发者通过 JS 与框架通信；框架完成操作后，再通知开发者的 JS 代码。

> 推论：C++ 在这个项目里的角色是**框架实现语言**，不是应用开发语言。
> 框架的 API 覆盖面决定了产品能力边界，这是后续需求拆解的主线。

## 分层

```
┌─────────────────────────────────────────┐
│  开发者应用：HTML / CSS / JS             │
├─────────────────────────────────────────┤
│  WebWin JS SDK（框架注入到页面中）        │  ← 封装层，开发者只面对这一层
│  window.WebWin.invoke(...) / .on(...)    │
├─────────────────────────────────────────┤
│  WebView2 消息通道                       │
│  postMessage  ⇄  addEventListener        │
├─────────────────────────────────────────┤
│  C++ 框架核心（原生实现）                 │
│  API 分发 · 窗口/系统能力 · 事件推送      │
├─────────────────────────────────────────┤
│  Win32 / WebView2 / Windows Runtime      │
└─────────────────────────────────────────┘
```

## 通信模型（双向）

底层直接基于 WebView2 的标准消息机制：

| 方向 | 机制 | 用途 |
| --- | --- | --- |
| JS → 框架 | `window.chrome.webview.postMessage(...)` | 调用框架 API |
| 框架 → JS | `window.chrome.webview.addEventListener("message", ...)` | 返回结果 / 推送事件通知 |

**开发者不直接接触 `window.chrome.webview`**，框架用 JS 把它封装掉。

> **iframe 需要额外处理**：上面这两个 API 只收发**顶级文档**的消息，
> iframe 要走 frame 级接口（收发都是 `ICoreWebView2Frame2`）。
> 框架已按"文档"粒度处理：顶级文档和每个 iframe 各自独立收发、独立订阅事件。
> 详见 `03-tech-stack.md` 的"iframe 与 C++ 的通信"。

## JS SDK（框架注入）

框架提供一套 JS 封装，并**注入到开发者页面中**，供开发者访问框架 API。

- 封装对象：把 `chrome.webview.postMessage` / `addEventListener` 包成语义化接口。
- 注入时机：页面加载阶段注入（对应 WebView2 的
  `AddScriptToExecuteOnDocumentCreated`），保证开发者脚本执行时 SDK 已就绪。
- 开发者侧形态（示意，具体命名待定）：

```js
// 调用 API
const path = await WebWin.dialog.openFile({ filters: [...] });

// 订阅事件
WebWin.on('window.resize', (e) => { ... });
```

## 已定设计决策

| 议题 | 决策 |
| --- | --- |
| API 调用语义 | **统一为异步 Promise** |
| 请求 / 响应关联 | **引入 request id** |
| 消息通道 | **API 响应与框架事件共用同一条 `message` 通道** |
| JS SDK 交付方式 | **内置于 `exe` 中，随 exe 分发**；另附一个 `.d.ts` 供开发期类型提示（**不单独发 npm 包**） |
| SDK 注入范围 | SDK 注入到**每一个**文档（含远程页面 / iframe）；但能否真正调用本地能力由 **C++ 侧按消息来源（origin）校验**决定。`webview.allowRemoteSdk` 配置可信域名（支持 `*` 通配符）后，匹配域名的远程页面也能调本地能力 |

> 备注：单通道 + request id 同时也符合项目的"效率优先"原则 ——
> 少建一条通道，少一层分发开销，消息体积也更小。

## 消息协议（通道层已落地）

通道层已按本协议实现：`src/web/webwin.ts` 编译后以 RCDATA 资源内嵌（`src/web/dist/webwin.js`），
由 `Page::injectSdk()` 用 `AddScriptToExecuteOnDocumentCreated` 注入。
通信能力是 `Ipc` 基类：通道方法（`call` / `invoke` / `request`）`protected`，
不暴露给开发者；事件 API（`on` / `off`）公开，各能力域继承它。
已落地的能力域：

| 域 | 全局对象 | 与窗口的关系 |
| --- | --- | --- |
| `win` | `win`（当前窗口）、`Win`（窗口类型） | 绑定窗口，事件按窗口分发 |
| `os` | `os`（全局单例） | **与窗口无关**，事件全局订阅 |

其余能力随 `01-api.md` 逐项开放。

上行消息可携带 `winId`：它是"操作哪个窗口"的路由凭据，
C++ 侧据此把消息落到指定窗口（见 `arch/07-web.md`）。
不带 `winId` 表示"与窗口无关"（`os` 这类单例能力），C++ 侧取到 `Win::NO_WINDOW`（-1）；
它订阅的事件也**不下发 `winId` 字段** —— 页面侧订阅时用的是 `null`，下发了就对不上键。

**C++ 侧分两层分发**：`Page` 只按 `type` 把消息路由到对应能力域的处理器，
再由该处理器按 `method` 处理具体业务 —— 避免所有能力的方法都堆在 `Page` 里。

> `error` 目前是**字符串**，协议里的 `{ code, message }` 等错误码表定下来后再改。

共用一条通道意味着下行消息需要靠信封字段区分"响应"与"事件"。

上行（JS → 框架，调用 API）：

```js
// type = 能力域，method = 该域下的具体动作
{ id: 7, type: "dialog", method: "openFile", params: { filters: [...] }, winId: 1 }
```

下行（框架 → JS）：

```js
// API 响应：带 id，与发起调用对应
{ id: 7, ok: true,  data: "C:\\Users\\..." }
{ id: 7, ok: false, error: { code: "...", message: "..." } }

// 框架事件推送：无 id，靠 name 区分
{ name: "window.resize", payload: { width: 1280, height: 720 } }
```

JS SDK 侧据此把带 `id` 的消息 resolve/reject 对应的 Promise，
把带 `name` 的消息派发给事件订阅者。

## 剩余待定

- **全局对象命名**：暂记 `WebWin`，最终命名待定。
- **错误结构**：`error` 的具体字段与错误码表。
- **权限 / 安全**：是否对 API 做白名单或页面来源限制。
- ~~**类型提示**：前端工程如何获得 `.d.ts`~~ → **已定**：SDK 附 `.d.ts`，
  且因它不进入最终分发物，不占用"小体积"预算（见 `00-overview.md` 体积口径）。
