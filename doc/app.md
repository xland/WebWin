# 应用 API（`app` 域）

`window.app` 是**全局单例**（和窗口无关），提供应用级信息与进程控制。

## 应用信息

来自 `package.json` 的 `webwin` 段（完整字段见 [config.md](config.md)）：

```ts
await app.getName();     // "WebWin 演示应用"
await app.getID();       // "webwin.demo"
await app.getVersion();  // "1.0.0"（数组拼成点分串）
```

未配置时：`id` 回落 `webwin`，`version` 回落 `1.1.1`，`name` 为空串。

## 路径

| 方法 | 返回 |
| --- | --- |
| `app.getAppPath()` | `package.json` 所在目录（命令行 `--path=...` 指定，否则 exe 同目录） |
| `app.getExePath()` | exe 所在目录 |
| `app.getDataPath()` | 应用数据目录：`%APPDATA%\WebWin\{应用 id}` |

应用自己的数据应写进 `getDataPath()`：多用户机器上隔离、卸载时方便清理。

## 退出与重启

| 方法 | 返回 | 说明 |
| --- | --- | --- |
| `app.exit(exitCode = 0)` | `Promise<void>`（不 resolve） | 立即退出，不发事件 |
| `app.quit()` | `Promise<void>` | 优雅退出：先 `before-quit`，关所有窗口，再 `will-quit`，最后退出 |
| `app.stopQuit()` | `Promise<void>` | 在 `before-quit` / `will-quit` 处理函数里调用，阻止退出 |
| `app.relaunch(options?)` | `Promise<void>` | 登记一次重启请求（配合 `exit` / `quit` 触发） |

### 立即退出：app.exit()

```ts
await app.exit();    // 退出码 0
await app.exit(3);   // 退出码 3
```

- 所有窗口直接关掉，不询问、不发退出前事件
- 进程立刻结束，**这个 Promise 不会 resolve**——日志要在调用之前写
- 退出码会作为进程退出码带出去（在启动它的 cmd 里 `echo %errorlevel%` 能看到）

### 优雅退出：app.quit() + before-quit / will-quit

`app.quit()` 走一套可中断的退出流程，给 JS 一个收尾 / 确认的机会：

```ts
app.on('before-quit', () => {
  if (hasUnsavedChanges) app.stopQuit(); // 阻止退出
});
app.on('will-quit', () => {
  app.stopQuit(); // 窗口已全关，仍可阻止真正退出（常见用法：配合托盘常驻后台）
});
await app.quit();
```

流程：`before-quit` 事件 → 关闭所有窗口 →（全关完）`will-quit` 事件 → `PostQuitMessage` 真正退出。
任一事件的处理函数里调 `app.stopQuit()` 即可阻止退出；`will-quit` 在窗口已全关后触发，
此时阻止退出应用会**继续在后台运行**（托盘常驻的主场景）。

> 事件在 WebView2 里是**异步**下发的，框架给了 200ms 宽限窗口让 JS 调 `stopQuit()`；
> 对同步处理函数绰绰有余，若处理函数内部做耗时异步、超过 200ms 才调 `stopQuit()`，退出可能已推进。

### 重启：app.relaunch()

登记一次重启请求——**不会立刻退出**。要真正重启，紧接着调用 `app.exit()`（或 `app.quit()`）
触发退出即可；新实例会在旧实例退出前被拉起，独立存活。

```ts
const args = await Process.getArg();                      // [exe, ...真正参数]
app.relaunch({ args: args.slice(1).concat(['--relaunch']) });
app.exit();
```

- `args`：新实例的命令行参数数组（**不含** exe 自身路径，框架会自己补上）。
  通常塞个 `--relaunch` 之类标记，方便新实例识别"我是被重启出来的"
- `execPath`（可选）：新实例的可执行文件路径；缺省为当前 exe 自身
- **单实例应用也能正确重启**：旧实例退出前会先释放单例互斥锁并停掉接待管道 server，
  新实例得以拿到锁成为第一实例（否则新实例会被当成第二实例、向即将退出的旧实例申请而被拒）

## 错误处理

同其他域：C++ 返回字符串而不是 `Error` 对象。
