# 配置说明

WebWin 的所有配置写在 `package.json` 里，集中在顶层对象的 `"webwin"` 子对象下。
`webwin` 之外的字段框架一律忽略，你可以照常写 npm 需要的 `name`、`dependencies` 等。

`package.json` 应放在 **exe 所在目录**（分发时随 exe 一起给出）。

## 完整示例

```json
{
  "name": "my-app",
  "version": "1.0.0",
  "webwin": {
    "id": "myapp",
    "name": "我的应用",
    "version": [1, 1, 1],
    "quitWhenNoWindow": true,
    "window": {
      "resizable": true,
      "size": { "w": 1000, "h": 800 },
      "miniSize": { "w": 250, "h": 200 },
      "maxSize": { "w": 2000, "h": 1600 },
      "pos": "centerScreen",
      "visible": true,
      "frame": true,
      "shadow": true,
      "minimizable": true,
      "maximizable": true,
      "alwaysOnTop": false,
      "taskbar": true,
      "borderRadius": true,
      "animation": true,
      "transparent": false,
      "title": "我的应用",
      "webview": {
        "path": ".",
        "allowWindowOpen": true
      }
    }
  }
}
```

> 所有字段都是可选的，不写就用默认值。

## 应用级字段

| 字段               | 类型   | 默认值    | 说明                              |
| ------------------ | ------ | --------- | --------------------------------- |
| `id`               | 字符串 | `webwin`  | 应用 ID，**必须是英文**，约束见下 |
| `name`             | 字符串 | —         | 应用名称，**可以是中文**          |
| `version`          | 数组   | `[1,1,1]` | 版本号，**必须是三个数字的数组**  |
| `quitWhenNoWindow` | 布尔   | `true`    | 所有窗口都关闭时，退出应用程序    |

### `id` 的约束

`id` 有两个用途，因此受两套规则约束：

1. **页面来源的虚拟主机名**：页面地址是 `https://{id}/index.html`
2. **Windows 的任务栏身份**：系统用它区分任务栏分组、跳转列表与通知归属

约束：

- **必须是英文**
- **不超过 128 个字符**
- **不能包含空格**
- 推荐点分格式：`CompanyName.ProductName.SubProduct.VersionInformation`
- 不配置（或写成空串）时使用默认值 `webwin`

> 多个应用共用同一份 `WebWin.exe`，系统靠 `id` 区分它们。
> 如果都用默认值 `webwin`，这些应用在任务栏上会被归并为同一个图标。

## 窗口字段 `window.*`

| 字段           | 类型        | 默认值           | 说明                                       |
| -------------- | ----------- | ---------------- | ------------------------------------------ |
| `resizable`    | 布尔        | `true`           | 是否允许调整窗口大小                       |
| `size`         | 枚举 / 对象 | `{w:1000,h:800}` | 初始尺寸，取值见下                         |
| `miniSize`     | 对象        | `{w:250,h:200}`  | 最小尺寸                                   |
| `maxSize`      | 对象        | 无限制           | 最大尺寸                                   |
| `pos`          | 枚举 / 对象 | `"centerScreen"` | 初始位置，取值见下                         |
| `visible`      | 布尔        | `true`           | 创建后立即显示                             |
| `frame`        | 布尔        | `true`           | 是否有边框（标题栏 + 边框）                |
| `shadow`       | 布尔        | `true`           | 是否有阴影；`frame` 为 `true` 时无效       |
| `minimizable`  | 布尔        | `true`           | 是否允许最小化                             |
| `maximizable`  | 布尔        | `true`           | 是否允许最大化                             |
| `alwaysOnTop`  | 布尔        | `false`          | 是否置顶                                   |
| `taskbar`      | 布尔        | `true`           | 是否在任务栏中显示                         |
| `borderRadius` | 布尔        | `true`           | 是否使用系统圆角                           |
| `animation`    | 布尔        | `true`           | 是否启用最小化 / 最大化的过渡动画          |
| `transparent`  | 布尔        | `false`          | 窗口背景是否透明；`frame` 为 `true` 时无效 |
| `title`        | 字符串      | 空               | 窗口标题                                   |

### `size` 取值

| 值               | 行为                             |
| ---------------- | -------------------------------- |
| `{w:1000,h:800}` | 按指定大小显示（默认）           |
| `"maximize"`     | 最大化显示，**不覆盖任务栏**     |
| `"fullScreen"`   | 全屏显示，**覆盖任务栏**         |
| `"fullDesktop"`  | 覆盖所有屏幕全屏显示，覆盖任务栏 |

### `pos` 取值

| 值               | 行为               |
| ---------------- | ------------------ |
| `"centerScreen"` | 屏幕正中央（默认） |
| `{x:100,y:100}`  | 指定位置           |

### 关于 `borderRadius`

圆角由系统的窗口圆角策略决定，`false` 时强制为方角。
该设置**从 Windows 11 起生效**，低版本系统会忽略它。

### 关于 `shadow`

只有 `frame: false`（无边框）时阴影才需要框架处理——无边框窗口没有非客户区，
框架会借助 DWM 让系统绘制外阴影。`frame: true` 时阴影由系统自带，此配置无效。

## 页面字段 `window.webview.*`

| 字段              | 类型   | 默认值  | 说明                                                |
| ----------------- | ------ | ------- | --------------------------------------------------- |
| `path`            | 字符串 | `.`     | 页面来源：一个**目录**或一个 **URL 地址**           |
| `allowWindowOpen` | 布尔   | `true`  | 是否允许页面中打开新窗口                            |
| `allowRemoteSdk`  | 布尔   | `false` | 是否允许把 SDK 注入到**非应用自身来源**的页面，见下 |

`path` 的两种写法：

- **目录**：该目录下必须有 `index.html`。相对路径以 **exe 所在目录**为基准。
  框架会把它映射为虚拟域名，实际加载的是：

  ```
  https://{id}/index.html
  ```

- **URL**：以 `http://` 或 `https://` 开头，直接加载该地址。

```json
"webview": { "path": "dist" }                  // exe 同目录下的 dist 文件夹
"webview": { "path": "D:\\app\\dist" }         // 绝对路径
"webview": { "path": "https://example.com" }   // 远程地址
```

## 关于 `allowRemoteSdk`

框架会把 `WebWin` SDK 注入到窗口中**每一个**页面文档，包括页面后来跳转到的站点、
以及第三方 iframe。这些页面拿到 SDK 就等于拿到了文件、进程等系统能力，风险不小。

因此默认行为是：**只把 SDK 装到应用自身的页面**（即 `path` 指定的来源），
其他来源的文档一律不装。把 `allowRemoteSdk` 设为 `true` 则取消这个限制，
不做任何来源判断地注入。

```json
"webview": { "path": ".", "allowRemoteSdk": false }
```

## JS SDK

框架在页面文档创建时（早于页面自身脚本）注入 SDK。页面里可以直接使用两个全局对象：

- **`win`** —— 当前窗口
- **`Win`** —— 窗口类型，用来创建新窗口

### 当前窗口

```js
win.maximize();
win.minimize();
win.restore();
win.show();
win.hide();
await win.resize(1024, 768);
await win.move(100, 100);
await win.close();

// 订阅当前窗口的事件，返回值用于取消订阅
const off = win.on("resize", (payload) => {
  console.log(payload.width, payload.height);
});
off(); // 取消订阅
```

### 新建窗口

新窗口**只能**用 `Win.create()` 创建（不支持 `new Win(...)`）：

```js
const win2 = await Win.create({ title: "设置", size: { w: 600, h: 400 } });

win2.maximize(); // 操作的是新窗口
win2.on("resize", (p) => console.log("win2 大小变了", p)); // 新窗口的事件会送到这里
await win2.close();
```

配置沿用 `webwin.window` 那一套字段（见上文窗口字段表），
`path` 未指定时新窗口会显示框架内置页面。

窗口关闭后再调用它的方法，会拿到一个错误。

开发期类型提示：把 SDK 附带的 `webwin.d.ts` 放进你的前端工程即可
（它不随 exe 分发，仅供开发期使用）。

> 目前已实现**窗口**这一能力域。
> 其余能力（`Fs` / `Clipboard` / `Dialog` / `Os` / `Screen` 等）会陆续开放。

## 配置不可用时

出现下面任一情况，框架会显示内置的默认页面（提示配置不可用），而不是你的页面：

- exe 同目录下没有 `package.json`
- 没有读到 `webwin` 配置对象
- 没有读到 `window` 配置对象
- 没有读到 `webview` 配置对象
- `path` 指向的目录里没有 `index.html`

## 尚未生效的字段

下面这些字段**已经能配置**（框架读得到），但行为还没实现，当前不会起作用：

| 字段              | 状态                                                         |
| ----------------- | ------------------------------------------------------------ |
| `transparent`     | 未实现。透明窗口与当前的 WebView2 集成方式存在冲突，仍在评估 |
| `allowWindowOpen` | 未实现。目前不限制页面中 `window.open`                       |
| `visible: false`  | 窗口不会自动显示，但配套的手动显示能力还没提供               |

另外，`title` 当前只作为窗口初始标题，**网页里的 `<title>` 变化不会同步到窗口标题**。

## 字段清单仍在扩充

上面列出的字段**尚未定稿**，后续还会追加（例如页面能力开关：是否允许脚本执行、
是否允许弹 `alert`、是否与后端通信、是否启用右键菜单等）。
新增字段都会保持"可选、有默认值"的风格，不会破坏现有配置。
