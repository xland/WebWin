# 托盘图标 API（`tray` 域）

`Tray` 是**类型**（`window.Tray`），不是实例。图标要显式创建：

```ts
const menu = await Menu.create([
	{ label: '显示窗口', onclick: () => win.show() },
	{ label: '退出',     onclick: () => app.exit() },
]);
const tray = await Tray.create({ tooltip: '我的应用', menu });

tray.on('doubleClick', () => win.show());
```

`Tray.create(...)` 是 async：图标是 C++ 侧的 Win32 资源，得等它建好。

一个进程**可以建多个**托盘图标（Windows 本来就允许，这里不做限制）。

## 方法

| 方法 | 返回 | 说明 |
| --- | --- | --- |
| `Tray.create(options?)` | `Promise<Tray>` | 建图标（async） |
| `tray.setToolTip(text)` | `Promise<void>` | 改鼠标悬停时的提示文字 |
| `tray.destroy()` | `Promise<void>` | 移除图标；**忘调会一直留在托盘里** |
| `tray.on(name, fn)` | `() => void` | 订阅事件（继承自 `Ipc`，返回退订函数） |

`create` 的两个参数**都可以不传**：

```ts
interface TrayOptions {
	/** 鼠标悬停时的提示文字；系统最多只有 128 个字符，超了会被截 */
	tooltip?: string;
	/** 右键时弹出的菜单（左键不弹）。不传就只有事件、没有菜单 */
	menu?: Menu;
}
```

## 事件

| 事件 | 触发 |
| --- | --- |
| `click` | 左键单击 |
| `rightClick` | 右键 |
| `doubleClick` | 左键双击 |

**双击不会再连带出一次 `click`。** Windows 的双击消息序列是
`UP → DBLCLK → UP`，所以单击不能当场发 —— C++ 侧把它压后，
等过系统双击窗口（`GetDoubleClickTime()`，默认 500ms）确认没有 `DBLCLK` 才发。

> 用的是**系统自己的**双击间隔而不是写死一个值：用户在控制面板里把阈值调大之后，
> 写死的值会小于系统阈值，那样"慢速双击"会既发 `click` 又发 `doubleClick`。

**有菜单时右键两个都给** —— 既发 `rightClick` 又弹菜单。想只听事件就不传菜单，
想只弹菜单就不订阅，不用在本域里替你做这个取舍。

## 菜单

菜单**只能在 create 时传，之后换不了**（没有 `setMenu`）。

点击菜单项照旧推给**创建那个菜单的页面**（跟 `win.setContextMenu` 一个路由）。
所以页面销毁后菜单也就没了 —— 图标还在，但右键只发事件、不弹菜单。

## 图标

一律用 **exe 自己的图标**（`src/Res.rc` 里的 `100 ICON`）。
v1 **不支持自定义图标**，有需要再做（见 `arch/99-todo.md` 的 T013）。

## 生命周期

图标归**进程**，不归页面：所有实例共用一个隐藏窗口收系统回调，
它懒建、随进程存活。所以图标能活过创建它的那个页面。

**只要还有托盘图标，最后一个窗口关掉时应用不再自动退出。**
`quitWhenNoWindow` 会让位给托盘 —— 否则"关掉窗口、应用留在托盘里"
这个主场景就没了。要退出就在菜单里自己放一项（调 `app.exit()`）。

## 错误处理

同其他域：C++ 返回字符串而不是 `Error` 对象。

```ts
try {
	await tray.setToolTip('x');
} catch (e) {
	const msg = e && e.message ? e.message : e;
	console.error('托盘操作失败：', msg);
}
```

## 尚未支持

- 自定义图标（T013）—— 目前只能用 exe 自己的
- 改菜单（`setMenu`）—— 只能在 `create` 时传
- 弹通知气泡（`NIF_INFO` / balloon tip）
- 左键单击也弹菜单（现在只有右键弹）
