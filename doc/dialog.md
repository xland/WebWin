# 系统对话框 API（`dialog` 域）

```ts
const r = await dialog.openPath({ title: '选一个文件' });
if (!r.cancel) console.log(r.data[0]);
```

## 关于 `cancel`

`openPath` / `savePath` 返回 `{ cancel, data }`。

用户点"取消"是**正常结果，不是错误**——所以不会 reject，而是 `cancel: true`。
真正的失败（比如 COM 初始化出了问题）才 reject。

取消时 `data` 给空值而不是 `undefined`，调用方不用多判一层：

| 方法 | 取消时的 `data` |
| --- | --- |
| `openPath` | `[]` |
| `savePath` | `''` |

## 方法

### `dialog.openPath(config?)`

选文件或文件夹。

```ts
interface OpenPathConfig {
  title?: string;           // 对话框标题
  okBtnText?: string;       // 确认按钮文字，如“选择文件”
  defaultPath?: string;     // 初始目录（也可以给完整文件路径，会自动拆出目录）
  type?: 'file' | 'dir';    // 默认 'file'
  filter?: FileFilter[];    // 文件类型过滤器，仅 type='file' 时有效
  multiSelection?: boolean; // 默认 false
  showHiddenFile?: boolean; // 默认 false
}

type FileFilter = [string, string]; // [显示名, 通配符]，如 ['图片', '*.png;*.jpg']
```

返回 `{ cancel: boolean, data: string[] }` —— `data` 一律是路径数组，单选时只装一个元素。

```ts
const r = await dialog.openPath({
  title: '选几张图片',
  okBtnText: '选择图片',
  multiSelection: true,
  filter: [['图片', '*.png;*.jpg'], ['所有文件', '*.*']],
});
```

不传 `filter` 就是"所有文件"。`type: 'dir'` 时过滤器无效（文件夹对话框不接受文件类型）。

### `dialog.savePath(config?)`

选保存位置。参数比 `openPath` 少了 `type` 和 `multiSelection`。

返回 `{ cancel: boolean, data: string }` —— `data` 是单个路径字符串。

```ts
const r = await dialog.savePath({
  title: '另存为...',
  okBtnText: '保存',
  defaultPath: 'C:\\Users\\新建文件.png',
  filter: [['图片', '*.png']],
});
```

两点固定行为：
- 不加 `FOS_FILEMUSTEXIST`（要存的通常是还没建的新文件），
  但会加 `FOS_PATHMUSTEXIST`（目录必须真实存在）。
- 给了 `filter` 时，第一个过滤器的后缀会成为默认扩展名——用户只输入文件名
  不写后缀时会自动补上。

### `dialog.msgBox(config?)`

弹一个系统消息框，返回用户点了哪个按钮。

```ts
interface MsgBoxConfig {
  title?: string;
  msg?: string;
  icon?: 'info' | 'warn' | 'err' | 'question' | 'stop';          // 默认 'info'
  btn?: 'ok' | 'okCancel' | 'yesNo' | 'yesNoCancel'
      | 'retryCancel' | 'abortRetryIgnore';                       // 默认 'ok'
}

type MsgBoxResult = 'ok' | 'cancel' | 'yes' | 'no' | 'abort' | 'retry' | 'ignore';
```

```ts
const r = await dialog.msgBox({
  title: '确认',
  msg: '要覆盖已有文件吗？',
  icon: 'question',
  btn: 'yesNoCancel',
});
// r === 'yes' | 'no' | 'cancel'
```

### `dialog.itemInFolder(path)`

打开资源管理器并选中指定文件（相当于"打开文件所在的位置"）。没有返回值。

```ts
await dialog.itemInFolder('C:\\Windows\\System32\\cmd.exe');
```

## 属主窗口

`window.dialog` 是**全局单例**，但绑在当前窗口上：SDK 安装时把当前窗口 id 存进来，
每次调用都带过去，C++ 据此把对话框挂到那个窗口上——属主窗口被禁用、层级正确、
任务栏不会多闪一个图标。

所以它的语义是"**当前窗口**的对话框"。要给别的窗口弹，需要那个窗口的 `Win` 实例
（`win.dialog` 尚未实现）。

方法在 **UI 线程上执行，但不是当场执行**——要先经 `App::runLater` 排到下一次
消息循环（原因见下）。调用方感知不到：API 本来就是 `Promise`。

### 为什么不能当场弹

`IFileDialog::Show` / `MessageBox` 内部是**嵌套的模态消息循环**，而本域的入口
`handleApi` 就跑在 `WebMessageReceived` 的回调栈上。在那里开循环等于"回调套回调"，
WebView2 Runtime 会判为重入并直接 `__debugbreak()`：

> Running a message loop synchronously in an event handler in Webview can cause
> reentrancy issue.

微软的建议是"把工作安排到事件处理器完成之后"，`App::runLater` 就是干这个的：
底层是启动时建的 `DispatcherQueueController`，任务由本线程的消息泵分派，
等 handler 返回、栈退干净了才跑。

不上后台线程：WebView2 的接口必须在 UI 线程上调，跨线程回传结果反而多一层出错面；
而且跨线程传 `HWND` 当属主，模态层级和禁用关系都不如同线程可靠。

同样的坑 `Menu` 先踩过（`TrackPopupMenu` 也是模态循环），现在两个域统一走
`App::runLater`。**新增能力时记住**：凡是会起模态循环 / 消息循环的 Win32 调用，
都要先 `App::runLater`。

## 错误处理

同其他域：C++ 返回字符串而不是 `Error` 对象。

```ts
try {
  await dialog.savePath({ title: '另存为' });
} catch (e) {
  const msg = e && e.message ? e.message : e;
  console.error('对话框失败：', msg);
}
```
