# 显示设备 API（`display` 域）

`window.display` 是**全局单例**——和窗口无关（不像 `win` 一个窗口一个实例）。

```ts
const list = await display.getAll();
const main = await display.getPrimary();
```

> **坐标口径：本域所有坐标与尺寸都是物理像素**，不是 CSS 像素。
> 150% 缩放的 4K 屏，`bounds.width` 报的是 3840，不是 2560。
> 需要 CSS 像素时自己除 `scaleFactor / 100`。

## 显示器信息

```ts
{
  id: string,           // 设备名，形如 \\.\DISPLAY1；可当标识用
  primary: boolean,     // 是否主显示器
  scaleFactor: number,  // 缩放百分比：100 / 125 / 150 ...
  bounds:   { x, y, width, height },  // 屏幕矩形
  workArea: { x, y, width, height },  // 工作区：屏幕矩形扣掉任务栏等
}
```

`bounds` 是虚拟桌面坐标系下的矩形（主屏不一定是 `0,0`；副屏在左边时 `x` 为负）。
`workArea` 是同一坐标系下的可用区域。

## 方法

| 方法 | 返回 | 说明 |
| --- | --- | --- |
| `display.getAll()` | `DisplayInfo[]` | 所有显示器 |
| `display.getPrimary()` | `DisplayInfo` | 主显示器 |
| `display.getByPos(x, y)` | `DisplayInfo` | 包含该点的显示器 |
| `display.getByRect(x, y, w, h)` | `DisplayInfo` | 与该矩形**重叠面积最大**的显示器 |
| `display.getColor(x?, y?)` | `string` | 该点颜色，形如 `#RRGGBB` |
| `display.getImage(x, y, w, h)` | `CapturedImage` | 截该区域，返回 PNG 字节 |

### getByPos

坐标落在多屏之间的空隙里（或完全在所有屏幕之外）时，返回**最近的一块**，
不会返回 `null`——调用方不必自己兜底。

### getByRect

窗口跨两块屏时归给**占得最多**的那块（Electron 的 `getDisplayMatching` 口径）。
矩形在所有屏幕之外时，退化为中心点最近的那块，同样保证有返回。

```ts
// 窗口在 (100, 100)、800×600，问它主要在哪个屏上
const d = await display.getByRect(100, 100, 800, 600);
```

### getColor

```ts
await display.getColor();        // 鼠标当前位置的颜色
await display.getColor(100, 100); // 指定点
```

不传坐标就取**鼠标当前位置**——做取色器时不用自己跟踪鼠标。
取的是屏幕合成后的最终像素（含所有窗口），不是某个窗口的内容。

### getImage

```ts
const shot = await display.getImage(0, 0, 800, 600);
const blob = new Blob([new Uint8Array(shot.buffer)], { type: 'image/png' });
img.src = URL.createObjectURL(blob);
shot.release();  // 用完必须还回去
```

| 字段 | 说明 |
| --- | --- |
| `buffer` | PNG 原始字节。**只读**：往里写会让渲染进程崩溃 |
| `width` / `height` | 截图尺寸（物理像素，等于传入的 w / h） |
| `release()` | 释放共享内存 |

大块数据走**共享内存**（`PostSharedBufferToScript`），不进 JSON 信封——
见 `arch/07-web.md` 第八节。`release()` 不调用的话底层映射会一直占着，
所以用完务必调；`Blob` 会拷一份数据，建完 `Blob` 就可以释放了。

## 事件

```ts
const off = display.on('display-added', d => console.log('插上了', d.id));
off();  // 取消订阅
```

| 事件 | 触发时机 | 载荷 |
| --- | --- | --- |
| `display-added` | 接入显示器 | 新显示器的 `DisplayInfo` |
| `display-removed` | 拔掉显示器 | 该显示器**最后一次**的 `DisplayInfo` |

载荷里的 `id` 可以用来和 `getAll()` 的结果对上。

> Windows 没有"显示器插拔"的专用消息：插拔、改分辨率、改排列都会广播
> `WM_DISPLAYCHANGE`，C++ 收到后重新枚举并与上次快照比对——
> 只有真正出现增删才推事件，同一条消息来多次不会重复推。

**`display-dpi-changed` 暂未实现**：Windows 没有专门的显示器 DPI 变更消息，
只能在同一批消息里顺带 diff，粒度（按屏还是按进程）尚未定。
已登记为 `arch/99-todo.md` 的 T012。

## 错误处理

同其他域：C++ 返回字符串而不是 `Error` 对象。

```ts
try {
  await display.getImage(0, 0, 800, 600);
} catch (e) {
  const msg = e && e.message ? e.message : e;
  console.error('截图失败：', msg);
}
```

## 演示

`demo/display.html`：列表、定位、取色、截图、插拔事件各一节。
