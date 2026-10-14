# `package.json` 中 `webwin` 字段详解

所有 WebWin 配置集中在 `package.json` 顶层对象的 `"webwin"` 子对象下
（依据 `05-boot-config.md` 的已定决策）。
`webwin` 之外的字段框架一律忽略。

> 字段清单**尚未定稿**，负责人表示还有其他属性后续追加。

骨架示意：

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
      "transparent": false,
      "webview": {
        "path": ".",
        "allowWindowOpen": true
      }
    }
  }
}
```

## 应用级字段

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `id` | 字符串 | — | 应用 ID，**必须是英文** |
| `name` | 字符串 | — | 应用名称，**可以是中文** |
| `version` | 数组 | `[1,1,1]` | 应用版本号，**必须是三个数字的数组** |
| `quitWhenNoWindow` | 布尔 | `true` | 当所有窗口关闭时，退出应用程序 |

> `id` 同时被用作 WebView2 的**虚拟主机名**（见 `window.webview.path`）。

## 窗口字段 `window.*`

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `resizable` | 布尔 | `true` | 是否允许调整窗口大小 |
| `size` | 枚举 / 对象 | `{w:1000,h:800}` | 窗口初始尺寸，取值见下 |
| `miniSize` | 对象 | `{w:250,h:200}` | 窗口最小尺寸 |
| `maxSize` | 对象 | 无（示例 `{w:2000,h:1600}`） | 窗口最大尺寸 |
| `pos` | 枚举 / 对象 | `"centerScreen"` | 窗口初始位置，取值见下 |
| `visible` | 布尔 | `true` | 创建窗口即显示（不等 HTML 加载完）；`false` 时由开发者加载完后手动显示 |
| `frame` | 布尔 | `true` | 是否拥有边框；`true` 时为带标题栏和边框的普通窗口 |
| `shadow` | 布尔 | `true` | 是否有阴影；**`frame` 为 `true` 时此属性无效** |
| `minimizable` | 布尔 | `true` | 是否允许最小化 |
| `maximizable` | 布尔 | `true` | 是否允许最大化 |
| `alwaysOnTop` | 布尔 | `false` | 是否置顶 |
| `taskbar` | 布尔 | `true` | 是否在任务栏中显示 |
| `transparent` | 布尔 | `false` | 窗口背景是否透明；**`frame` 为 `true` 时此属性无效** |
| `title` | 字符串 | **无默认值**（默认使用网页中设置的 title） | 窗口标题。未设置时默认采用网页的 `title`；设置后作为网页加载前的初始标题，**HTML 的 `title` 加载后仍会覆盖此设置** |

### `window.size` 取值

| 值 | 行为 |
| --- | --- |
| `{w:1000,h:800}`（默认） | 按指定大小显示 |
| `"maximize"` | 在当前屏幕最大化显示，**不覆盖任务栏** |
| `"fullScreen"` | 在当前屏幕全屏显示，**覆盖任务栏** |
| `"fullDesktop"` | **覆盖所有屏幕**并全屏显示，覆盖任务栏 |

### `window.pos` 取值

| 值 | 行为 |
| --- | --- |
| `"centerScreen"`（默认） | 窗口出现在**当前屏幕**正中央 |
| `{x:100,y:100}` | 在指定位置显示 |

## `window.webview.*`

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `path` | 字符串 | `.` | 可以是一个**目录**或一个 **URL 地址**。若是目录，该目录下必须有 `index.html`；可以是相对路径或绝对路径 |
| `allowWindowOpen` | 布尔 | `true` | 是否允许在页面中打开新窗口 |

`path` 为目录时，实际会被格式化为：

```
https://{appId}/index.html
```

即：把该目录映射为以**应用 ID 作为虚拟主机名**的来源（对应 WebView2 的
虚拟主机名到文件夹的映射机制）。

## 页面能力开关

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `scriptDialogEnable` | 待补 | 待补 | 是否允许脚本打开 `alert`、`confirm` 等对话框 |
| `scriptEnable` | 待补 | 待补 | 是否允许窗口中的页面执行脚本 |
| `webMessageEnable` | 待补 | 待补 | 是否允许窗口中的页面与 C++ 后端通信 |
| `contextMenuEnable` | 待补 | 待补 | 是否启用窗口上的右键菜单 |

### 右键菜单项默认启用状态

| 菜单项 | 默认 |
| --- | --- |
| `inspectElement` | 启用 |
| `selectAll` | 启用 |
| `pasteAndMatchStyle` | 启用 |
| `paste` | 启用 |
| `copy` | 启用 |
| `cut` | 启用 |
| `other` | 禁用 |
| `webCapture` | 禁用 |
| `saveAs` | 禁用 |
| `reload` | 禁用 |
| `forward` | 禁用 |
| `back` | 禁用 |
| `print` | 禁用 |

## 待确认

1. **四个页面能力开关**（`scriptDialogEnable` / `scriptEnable` / `webMessageEnable` /
   `contextMenuEnable`）未给出类型与默认值，待补（推测为布尔）。
2. **多屏幕下"当前屏幕"的判定**：`size` 的 `maximize` / `fullScreen` 与 `pos` 的
   `centerScreen` 都提到"当前屏幕"——指主显示器、还是鼠标所在显示器，待明确。
3. **`id` / `name` 是否必填**、缺失时如何处置（是否落到内置默认页面，见 `05-boot-config.md`）。
4. **`resizable: false` 与 `miniSize` / `maxSize` 同时配置时**如何取舍，待明确。
5. **`transparent` 与子窗口模式**：透明窗口通常需要分层窗口支持，而本项目以子窗口 HWND
   方式集成 WebView2（见 `03-tech-stack.md` 的 Airspace 限制）。
   该组合能否实现、代价如何，实现到窗口层时需验证。
6. 右键菜单项是否支持按项单独覆盖配置（当前只给了默认启用状态）。
