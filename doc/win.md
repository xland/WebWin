# 窗口 API（`win` 域）

页面里直接可用的全局对象：

- **`win`** — 当前窗口
- **`Win`** — 窗口类型，用来创建新窗口

新建窗口的配置字段见 [config.md](config.md) 中 `webwin.window.*` 一节。

## `win`（当前窗口）

### 几何与显隐

| 方法 | 行为 |
| --- | --- |
| `win.maximize()` | 最大化 |
| `win.minimize()` | 最小化 |
| `win.restore()` | 从最大化 / 最小化还原 |
| `win.resize(width, height)` | 改尺寸 |
| `win.move(x, y)` | 移动，**屏幕坐标** |
| `win.show()` / `win.hide()` | 显示 / 隐藏 |
| `win.isMaximized()` | 是否处于最大化 |

### 关闭

| 方法 | 行为 |
| --- | --- |
| `win.close()` | 发 `WM_CLOSE`，调用方可以拦截 |
| `win.destroy()` | 直接 `DestroyWindow`，不给拦截机会 |

窗口关掉之后再调它的任何方法，都会拿到 C++ 返回的错误字符串。

### 事件

```ts
const off = win.on('resize', p => console.log(p.width, p.height));
const off2 = win.on('move',   p => console.log(p.x, p.y));
// 取消订阅
off();
```

| 事件 | 触发时机 | 载荷 |
| --- | --- | --- |
| `resize` | 窗口尺寸变化 | `{ width, height }` |
| `move` | 窗口位置变化 | `{ x, y }` |

**`on` 的返回值就是取消订阅的函数**，存起来，想停的时候调它。

> 切换页面时务必取消订阅。外壳（`index.html`）切换左侧菜单时会调本页面的
> `unsubscribeAll()`，在那里统一撤掉。

### 无边框窗口：自定义边框与标题栏

无边框（`frame: false`）窗口没有系统边框和标题栏，移动与缩放得自己做。
底下两套东西走的是同一条路：`mousedown` 时让 C++ 给自己的窗口发一条 `WM_NCLBUTTONDOWN`，
由 `DefWindowProc` 进**系统原生**的循环 —— 所以贴边分屏、最大化 / 最小化动画这些原生行为都还在。

> 为什么不能像普通窗口那样在 `WM_NCHITTEST` 里返回 `HTCAPTION`：
> webview 是以子窗口嵌进来的，它铺满整个客户区，父窗口在自己的地盘上收不到这个消息。

#### 边框：`setResizeEdges()`

```ts
await win.setResizeEdges();                    // 8 个全建：4 条边 + 4 个角
await win.setResizeEdges(['left', 'right']);   // 只建指定的
await win.removeResizeEdges();                 // 拆掉
```

- 把手是 `position: fixed`、`z-index` 拉满的透明 div：边 6px、角 8px，边角重叠处归角
- **每次调用先清掉上一批**，不会叠加
- 窗口最大化时自动隐藏（系统本来也不让改大小）
- 无边框**且**可缩放（`frame: false` + `resizable: true`）的窗口，
  **顶级页面**在 DOM ready 后会自动全建一份
- 不想让它自动铺：在页面自己的脚本里（DOM ready 之前）调一次 `win.removeResizeEdges()`
- iframe 不会铺（它没资格去改顶层窗口的大小）

#### 标题栏：`data-caption`

给元素加上 `data-caption`，它就成了标题栏：

```html
<div data-caption>应用名</div>

<div data-caption>
  应用名
  <button data-caption="no-drag">×</button>  <!-- 按钮自己收点击，不触发拖动 -->
</div>
```

- 拖动 = 移动窗口；最大化时**按下不动不会还原**，真的拖起来了才还原并跟手
  （这一整套是系统原生行为，SDK 不插手，所以位置和手感跟系统标题栏一致）
- 双击 = 最大化 / 还原
- 右键 = 弹出系统为窗口准备的那个菜单（还原 / 移动 / 大小 / 最小化 / 最大化 / 关闭）
- 内部标了 `data-caption="no-drag"` 的子树不参与拖动 —— 留给按钮、输入框这类控件
- 用事件委托实现：页面后来动态插入的标题栏也生效，不用重新调用任何东西
- 只在**顶级页面**里接（iframe 不参与）

#### 最大化占的是工作区，不是整屏

无边框窗口是 `WS_POPUP`，没有 `WS_CAPTION`，`DefWindowProc` 给 `WM_GETMINMAXINFO`
的默认最大化尺寸按**整块屏幕**算，还会再外扩一圈边框 —— 直接表现就是盖住任务栏、
且窗口顶部被顶到屏幕外（标题栏少一截）。

所以 `WM_GETMINMAXINFO` 里对无边框窗口把 `ptMaxSize` / `ptMaxPosition`
钉成窗口所在显示器的**工作区**（`MONITORINFO::rcWork`）。配合上面
`WM_NCCALCSIZE` 那条（客户区 = 窗口矩形），最大化后客户区正好等于工作区，不压任务栏。

#### 想自己做把手 / 标题栏

上面两个都是基于这两个方法，开发者可以直接用：

| 方法 | 行为 |
| --- | --- |
| `win.startDrag()` | 进入系统原生的移动循环 |
| `win.startResize(edge)` | 进入系统原生的缩放循环，`edge` 取 `top` / `bottom` / `left` / `right` / `topLeft` / `topRight` / `bottomLeft` / `bottomRight` |
| `win.showSystemMenu()` | 弹出系统菜单 |

### 右键菜单

`win.setContextMenu(menu)` 挂菜单；`win.setContextMenu(null)` 解绑。详见 [menu.md](menu.md)。

### 其他

- `win.winId`：当前窗口在 SDK 里的标识（C++ 侧用于把消息路由回这个窗口）

## `Win`（窗口类型）

新窗口**只能**用 `Win.create()` 创建（不支持 `new Win(...)`）：

```ts
const win2 = await Win.create({
  title: '设置',
  size:  { w: 600, h: 400 },
  webview: { path: 'win.html' } // 新窗口加载哪个页面
});
```

返回 `Promise<Win>`，resolve 时窗口 id 已就绪。

**`win2` 是创建它的那个页面里的对象**，不是新窗口自己的。
新窗口的 `resize` / `move` 事件会**送到 `win2`**，而不是新窗口的 `win`。

## 错误处理

C++ 返回的错误是字符串，不是 `Error` 对象：

```ts
try {
  await win.maximize();
} catch (e) {
  // e 可能是字符串，比如 "window not found: 3"
  const msg = e && e.message ? e.message : e;
  console.error('win.maximize() 失败：', msg);
}
```
