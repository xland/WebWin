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
