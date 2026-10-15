# `src/web` 目录承载的逻辑

`src/web` 是框架的前端侧源码目录，产物经构建后进入 `dist`，
再由 `Res.rc` 以 RCDATA 资源内嵌进 `exe`。

- 源码：`webwin.ts`（JS SDK）、`default.html`（内置默认页面）等
- 产物：`dist/webwin.js`、`dist/webwin.d.ts`、`dist/default.html`
- 构建：`npm run build:all`（esbuild 打包压缩 js、压缩 html、tsc 生成 d.ts）
- 消费方：`Page` 从资源读出后，js 用 `AddScriptToExecuteOnDocumentCreated` 注入，
  html 在配置不可用时用 `NavigateToString` 加载

> 本文档由负责人逐步阐述、逐步补齐。
> 定稿后再据此调整 `src/web` 的实现（当前实现的 `webwin.ts` 存在待讨论的问题）。

## 一、按能力域拆分源文件

`src/web` 下应有一系列 ts 文件，**每个文件对应一个能力域**，
即在某个领域上暴露给 JS 的能力。

举例（负责人给出的示例，清单会持续扩充）：

| 文件 | 能力域 |
| --- | --- |
| `Win.ts` | 窗口 |
| `Clipboard.ts` | 剪贴板 |
| `Dialog.ts` | 系统对话框 |
| `Fs.ts` | **只管文件** |
| `Os.ts` | 操作系统（进程、版本等） |
| `Screen.ts` | 屏幕信息 |

> 上面这几个**只是少数例子**，以后还会有很多文件，比如 `http`、`https` 等。
> 目录相关的**路径**将来由单独的文件负责（不与 `Fs.ts` 混在一起）。

### 命名原则

**两套命名并存，各管一件事：**

| 场合 | 首字母 | 例子 |
| --- | --- | --- |
| 暴露给开发者的是**对象** | 小写 | `dialog`、`fs`、`clipboard` |
| 暴露给开发者的是**类型** | 大写 | `Menu` |

这与 **Electron 的 API 命名原则一致**。

> 该原则**不影响 ts 文件的命名** —— 文件仍用 PascalCase，
> 因为 ts 文件里**大部分内容都是类型**。

即：文件 `Dialog.ts` 中可以同时导出类型 `Dialog` 和对象 `dialog`，
文件名按类型走大写，开发者拿到的是小写对象。
（`02-architecture.md` 里的示意 `WebWin.dialog.openFile(...)` 与此一致。）

与 `01-api.md` 的对应关系（**推测，待负责人确认**）：

| 文件 | 可能覆盖的 `01-api.md` 能力 |
| --- | --- |
| `Win.ts` | 访问窗口（枚举窗口、向窗口发送消息） |
| `Clipboard.ts` | 剪贴板访问 |
| `Dialog.ts` | 使用系统对话框 |
| `Fs.ts` | 文件读写删除创建（**只管文件**；目录与常用目录待拆分） |
| `Os.ts` | 访问系统进程、启动子进程、访问版本信息、阻止锁屏、多开通知 |
| `Screen.ts` | 访问屏幕信息 |

## 二、窗口：`win` 与 `Win`

在**一个页面中**，JS 脚本可以**直接使用 `win` 对象，它代表当前窗口**：

```js
win.maximize();                    // 当前窗口最大化
win.on('resize', (e) => { ... });  // 监听当前窗口大小变化
```

需要**新窗口**时，用 `Win` 这个类型去构造：

```js
const win2 = new Win(...);         // 创建一个新窗口
```

即同一份能力有两种形态，正好对应上一节的命名原则：

| 名字 | 身份 | 用法 |
| --- | --- | --- |
| `win`（小写） | **当前窗口实例** | 页面里直接可用，代表"这个窗口" |
| `Win`（大写） | **类型 / 构造器** | `new Win(...)` 创建新窗口 |

由此确认**待确认第 4 条**：大小写并存的命名是**统一约定** ——
大写是可 `new` 的类型，小写是开发者直接拿到的实例（或单例对象）。

同时可以看出：**事件是挂在实例上的**（`win.on(...)`），
而不是只有一个全局的 `WebWin.on(...)`。

### 已定

1. **`win` 是全局变量**（不是 `WebWin.win`）。
2. **`new Win(...)` 的参数复用 `package.json` 里 `window` 那套配置**。
3. `clipboard`、`dialog` **肯定是单例对象**；其余能力域是类还是单例，**先不定**。
4. **有应用级全局事件，挂在 `app` 对象上**。

## 三、`app` 对象（应用级）

与 `win` 并列，还有全局单例对象 **`app`**，代表整个应用程序：

```js
app.on('...', (e) => { ... });   // 应用级事件
```

命名同样遵循第一节的原则：`app` 小写是单例对象，
对应的源文件按类型走 PascalCase（`App.ts`，**文件名待确认**）。

- 应用级 / 全局事件**挂在 `app` 上**，实例级事件挂在 `win` 上。
- `win` 与 `app` 都是页面里**直接可用**的全局对象。

### 已定

- `app` 上提供的能力目前有：**退出应用、版本信息、多开通知、数据目录**。
  其余能力**慢慢再补**。
- **不需要**可 `new` 的 `App` 类型（只有单例对象 `app`）。

## 四、通信基类

**JS 与 C++ 的通信能力是一个基类。开发者不会直接操作这个通信类**，
而是通过各个对象或类型**使用这个基类**来与 C++ 打交道。

即：对外只暴露语义化的方法，通信细节藏在基类里。

```js
win.maximize();   // 开发者只看到这个
                  // 其内部经由通信基类把请求发给 C++
```

各能力域（`Win`、`Clipboard`、`Dialog`、`Fs`、`Os`、`Screen` 等）
以及单例对象（`win`、`app`、`clipboard`、`dialog`）
都建立在这个基类之上。

### 已定

1. 基类命名为 **`Ipc`**（源文件 `Ipc.ts`）。
2. 基类**同时承载事件订阅**（`on` / `off`），不只是请求响应。
3. 单例对象（`clipboard`、`dialog`、`app`）**也以该基类为实现基础**。
4. 各能力类与基类之间：**倾向继承**（`extends`），理由见下。

### 继承 vs 组合

负责人倾向继承，评估后**采纳继承**：

- **事件订阅天然复用**：`win.on` / `app.on` / 各能力对象上的事件订阅行为一致，
  继承后无需在每个类里重复挂一层。
- **代码量与产物更小**：方法挂在原型上共享，打包压缩后每个类只多一层声明。
  （SDK 的 js 会内嵌进 `exe`，属最终分发物，见 `00-overview.md` 体积口径。）
- **耦合风险低**：基类职责单一（只管通信），能力类本身很薄，
  基类变动的影响面可控。
- 组合更灵活的场景（一个类需要多条通道 / 不同注入实例）目前不存在。

**代价与缓解**：继承会把 `invoke` / `on` 一并暴露到每个能力对象上，
运行时 `clipboard.invoke(...)` 仍可调（语义泄漏）。
用 TS 的 `protected` 在**类型层面**把通道藏起来，
`.d.ts` 里不导出这些方法，使开发者在编辑器里看不到、也不该用。

示意（**api 命名待定**）：

```ts
class Ipc {
	/** 能力域标识，子类覆盖（win / dialog / clipboard ...） */
	protected type: string = '';

	/** 显式指定域时用：invoke(type, method, params?) */
	protected invoke<T>(type: string, method: string, params?: unknown): Promise<T>;
	/** 调本域的方法：type 取 this.type，方法里不用重复写域名 */
	protected call<T>(method: string, params?: unknown): Promise<T>;

	protected on(event: string, handler: (payload: unknown) => void): () => void;
	protected off(event: string, handler: (payload: unknown) => void): void;
}

class Win extends Ipc {
	private static readonly domain = 'win';
	protected type = Win.domain;

	maximize() { return this.call('maximize'); }
}
```

`call` 是**给各能力域日常使用**的公共方法（封装了第一个参数 `type`），
`invoke` 保留给需要显式指定域的场合（如 `Win.create` 这种静态方法）。

### 已定

1. 通道方法用 TS 的 **`protected`**，且 **`.d.ts` 里不导出**。
2. api 名按 **`域.动作`** 组织，实现时**拆成两个参数**：
   `type`（能力域，如 `win` / `dialog`）+ `method`（动作，如 `maximize`）。
   具体每一条在实现具体类型时逐条设定。
3. **C++ 侧分两层分发**（避免 `Page::onMsgReceived` 里的方法表爆炸）：

   ```
   Page::onMsgReceived   —— 只按 type 路由：type=="win" → Win::handleApi(...)
   Win::handleApi        —— 再按 method 处理具体业务（on/off/create/maximize/...）
   ```

   将来加一个能力域，只在 `Page` 里多一行 type 分支，
   该域自己的方法全部收在它自己的处理函数里。

## 五、`Win` 类型：跨窗口对象与窗口 id

**基本形态**：页面加载成功后，向该页面的 JS 暴露 `win` 对象，
其方法用于操作**当前窗口**。

**跨窗口持有**：A 页面中可以创建新窗口：

```js
let winB = new Win(...);   // A 创建了 B 窗口，A 持有 B 的窗口对象
```

要求：

- B 窗口的事件要能**分发到 `winB` 对象**上（事件不进 A 的 `win`）
- 通过 `winB` 能**操作 B 窗口**的行为

**因此：每个窗口都要有一个不重复的 id** —— 用于把消息路由到正确的窗口对象。

### 难点（分析，待逐项定方案）

1. **事件要跨页面投递**。`winB` 活在 **A 页面**里，而事件由 **B 窗口的 webview** 产生。
   `PostWebMessageAsJson` 是发给某个 webview 的，所以 B 的事件必须先送到 **A 的 webview**，
   再带上窗口 id，由 A 页面的 SDK 路由到 `winB`。
   → C++ 侧需记录"哪个窗口由哪个页面创建"。
2. **`new Win()` 是同步的，建窗口是异步的**。窗口真正建好前拿不到 id，
   构造函数返回的对象处于未就绪状态。需要决定就绪语义（见待确认）。
3. **窗口关闭后的对象处置**。B 关了以后 A 里的 `winB` 变成悬空引用，
   C++ 侧的映射也要回收。
4. **SDK 侧需要一张窗口表**（id → `Win` 实例）用于路由与查找。

> `invoke` 的响应**不受影响**：请求由持有者页面发出、响应回到同一页面，
> 靠 request id 关联即可。只有**事件**需要"跨页面 + 窗口 id"的路由。

### id 的必要性（结论：必需）

即使不考虑跨页面事件，**方法调用本身也需要 id**：
`winB.maximize()` 这条消息是从 **A 页面**发出的，C++ 收到后必须知道目标是 B 而不是 A。
窗口 id 是消息里唯一能指明"操作哪个窗口"的凭据。因此 id 不是优化，是前提。

### 已定

1. 窗口 id **由 C++ 生成**，类型为**自增整数**。
   → C++ 的 `Win` 类补上 id 字段，并提供**按 id 查找**与**创建者页面**的记录
   （后者是跨页面事件路由的前提）。
2. **B 窗口关闭后**，`winB` **不再接收任何事件**；
   此时调用 `winB` 的方法，**只会得到 C++ 返回的报错信息**。
3. **事件推送策略：订阅后推**（见下）。

### 事件推送：订阅后推

采纳订阅后推。理由：`resize` / `move` 这类事件在一次拖拽中会产生几十条消息，
且跨页面投递还要多一跳，一律推送的浪费确实高。
代价是 C++ 侧要维护一张订阅表：

```
(窗口 id, 事件名) → 订阅它的页面(webview) 集合
```

复杂度可控（几张 map，窗口关闭时清理），换来的是不推无用消息。

### 已定（补充）

4. **允许**同一事件被多个页面订阅（A 和 C 可同时监听 B 的 `resize`）。
5. **首批 api**（`win.*` 域）：
   `maximize`、`minimize`、`hide`、`show`、`restore`、`resize`、`move`、`close`、`destroy`。
   其余 api 在实现具体类型时逐条补充。
6. **窗口已关闭后**调用方法，返回**错误字符串**
   （正式错误码表待 T011 定，届时再改为结构化错误）。

## 六、`new Win(...)` 的就绪语义

窗口创建是异步的，而 `new` 是同步的 —— 对象返回时 id 还没拿到。
两种写法：

### A：`new Win(opts)` + `ready: Promise<Win>`

```js
const winB = new Win({ ... });
await winB.ready;      // 之后 id 才可用
winB.maximize();
```

- 对象**立即存在**，可以先订阅事件、再等就绪
- 每个方法内部必须等 id：统一走 `await this.ready` 再发消息，
  （集中在基类/子类的一个私有 `call()` 里，不必每个方法都写）
- **不 `await ready` 也能用**，方法调用会被自动排队

### B：`Win.create(opts): Promise<Win>`

```js
const winB = await Win.create({ ... });   // 拿到就是就绪的
```

- 不存在"半就绪"状态，语义最干净
- `new Win(...)` 的语义则另作他用（见待确认）

### 已定：**只保留 `Win.create(opts): Promise<Win>`**

- **`new Win(...)` 不作为创建方式**，不开放给用户 ——
  构造函数设为 TS 的 `private constructor`，类型层面禁止 `new Win(...)`。
  （JS 运行时仍可调用，但 `.d.ts` 不导出该签名，编辑器里会直接报错。）
- 只有 `Win.create(opts)` 一条入口，返回 `Promise<Win>`，resolve 出来的对象**id 已就绪**。
- **不支持 `new Win(id)`**（传 id 绑定已有窗口）这一形态。
- 创建失败时 `create` **reject**。
- 当前窗口的全局对象 `win` 由 **SDK 内部**构造（不受"禁止 new"影响），
  开发者直接取用。

### 已定

1. 将来会为 `Win` 类型提供**静态方法**：**遍历所有窗口**、**找到指定窗口**。
   （构造函数这条路已封死，绑定已存在窗口只能靠这类静态方法。）
   具体方法名与查找条件**待定**。

## 七、与 `window.open` 的关系

页面用 HTML 的 `window.open(...)` 打开窗口时，走的是 WebView2 的
`NewWindowRequested` 事件（对应配置项 `webview.allowWindowOpen`）。

**能兼容，但要接受一点**：

- `window.open` 是浏览器 API，**同步返回**浏览器自己的 `Window` 对象，
  返回值**无法变成**我们的 `Win` 实例。
- 被打开的页面**同样会注入 SDK**，所以那个页面里可以直接用自己的 `win` 对象。
- 若打开方想拿到对应的 `Win` 实例，需要 C++ 在创建后把**窗口 id 通知给打开方页面**
  （例如推一个事件），页面据此构造出绑定到该 id 的 `Win` 对象。

**已定**：`window.open` 与 `new Win(...)` **复用同一套"创建窗口"的实现**，
只是触发方不同 —— 行为、事件、生命周期完全一致，不会出现两套语义。

### 已定

1. `allowWindowOpen: false` 时 —— **不允许打开窗口**（直接取消）。
2. `window.open` 创建的窗口**同样走订阅后推**的事件机制。

## 八、消息通道的扩展：附加对象与共享内存

除了 JSON 消息，通道层还需要两种特殊载体。二者方向相反。

### 1. JS → C++：附加对象（`postMessageWithAdditionalObjects`）

```js
window.chrome.webview.postMessageWithAdditionalObjects({ api: 'getFilePath' }, file.files);
```

- 第二个参数是**数组**，元素是 DOM 对象（典型是 `File`）
- 解决的是"**JSON 序列化不了的东西怎么传**"：
  最典型的场景是拿到 `<input type="file">` 选中文件的**真实磁盘路径**
- C++ 侧在 `WebMessageReceived` 的事件参数里取附加对象集合，
  元素为 `ICoreWebView2File`（可读 `Path`）
- **要求 `Ipc` 基类兼容**：`invoke` 需要能携带这类"附加对象"参数

### 2. C++ → JS：共享内存（`PostSharedBufferToScript`）

```cpp
ComPtr<ICoreWebView2_17> wv17;
webview.As<ICoreWebView2_17>(&wv17);
wv17->PostSharedBufferToScript(sharedBuffer.Get(),
	COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY, jsonStr.data()); // 通知渲染进程
```

- 大块数据**不再经 JSON 字符串拷贝**，改为共享内存，页面侧收 `sharedbufferreceived` 事件
- 典型用途：读取大文件的内容、传递二进制数据
- 需要管理 buffer 的**生命周期与访问权限**（上例为只读）

### 已定

1. **附加对象并入主通道** `{ id, api, params }`：
   JS 侧仍走 `postMessageWithAdditionalObjects`，消息体还是 `{ id, api, params }`；
   C++ 侧在**具体的 api 处理方法里**按需取附加对象：

   ```cpp
   ComPtr<ICoreWebView2ObjectCollectionView> additionalObjects;
   args2->get_AdditionalObjects(&additionalObjects);
   ```

   即：附加对象**不是独立的消息类型**，只是某条消息"顺带携带"的额外数据。
2. **共享内存走独立通道**：`PostSharedBufferToScript` 发出、
   页面侧由 `window.chrome.webview.addEventListener('sharedbufferreceived', ...)`
   接收，不并入 `{ id, api, params }`。
3. **共享内存的触发条件：由具体的 api 自己决定**（不按数据大小自动切换）。
4. `webview.As<...>()` **失败时（Runtime 太旧）直接报错**，不做静默降级。
5. **WebView2 Runtime 最低版本：暂时不定**。

## 九、业界对照：Tauri / Electron

负责人对"跨页面持有窗口对象"的成本有疑虑，考察两个框架的实际做法。

### Tauri v2：**支持**，且比我们的设计更彻底

- 前端可直接 `new WebviewWindow(label, options)` 创建窗口；
  `WebviewWindow.getByLabel(label)` 拿已有窗口的句柄。
- 句柄上有 `listen` / `once` / `onResized` / `onMoved` / `onFocusChanged` /
  `onCloseRequested` 等**窗口级事件** —— 即 A 页面可以监听 B 窗口的事件。
- 事件按 **label（target）路由**：后端把发往某个 label 的事件，
  投递给注册了该 target 监听器的页面 —— 正是我们设计的"跨页面事件路由"。
- `emitTo(target, event, payload)` 支持定向投递。
- 所有方法返回 `Promise`（一次调用一次 IPC）。
- 监听器**在窗口销毁时自动移除**（对应我们"B 关闭后不再接收事件"）。
- 窗口标识是**字符串 label**，因此可以**按名字查找**（`getByLabel`）。

### Electron：**不支持**

- `BrowserWindow` 只能在主进程创建；渲染进程只能 IPC 请主进程建，拿回一个 id。
- 渲染进程拿不到另一个窗口的对象（`BrowserWindow.fromId` 是主进程 API）。
- `window.open` 返回 `BrowserWindowProxy`，**功能有限**
  （`closed` / `blur` / `close` / `focus` / `print` / `postMessage`），
  **没有事件订阅能力**。
- 想监听另一个窗口的事件，只能在主进程里捕获后转发到渲染进程。

### 结论

**"A 持有 B 的窗口对象、且 B 的事件能分发到 A" —— Tauri 有，Electron 没有。**
我们的设计更接近 Tauri。

### 成本分层（成本不在同一处）

| 能力 | 成本 |
| --- | --- |
| 跨窗口**方法调用**（`winB.maximize()`） | **低**：同一跳 IPC，消息里多一个 id 字段 |
| 跨窗口**事件分发**（B 的 resize → A 的 winB） | **高**：需要订阅表、跨页面投递、生命周期清理 |

复杂度与效率压力几乎全部来自后者。

### 已定（负责人拍板）

**不分阶段，按原设计全量实现** —— 即跨窗口方法调用与跨页面事件分发都要做，
不做"先做一半"的裁剪。

**不引入 label，只用窗口 id**（C++ 自增整数）。
Tauri 那套字符串 label（`getByLabel` 按名查找）不采用。

### 待确认

1. `window.open` 创建的窗口，其 `Win` 对象**如何交回打开方**（事件名、时机）——
   **负责人：以后再定**。

### 上一节遗留的待确认

1. ~~文件与能力是**一对一**还是一对多~~ → **`Fs.ts` 只管文件**，
   目录、常用目录、路径的归属待补（路径由单独文件负责）。
2. `01-api.md` 中的「启动 HTTP 服务」「加解密」（T005）「WebSocket」（T006）等，
   将来是否也各自独立成文件（`Http.ts` / `Crypto.ts` / `WebSocket.ts`）。
   —— 已确认 `http`、`https` 这类会陆续出现，但**未见得一个能力域一个文件**。
3. 是否需要一个聚合入口把这些能力域挂到 `WebWin` 上，还是各自直接挂载。
4. ~~一个文件同时导出"类型"与"对象"时，两者的名字只差首字母大小写
   （如 `Dialog` / `dialog`），是否作为统一约定~~ → **是**（见第二节）。
