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

## 退出

```ts
await app.exit();    // 退出码 0
await app.exit(3);   // 退出码 3
```

- 所有窗口直接关掉，不询问、不发退出前事件
- 进程立刻结束，**这个 Promise 不会 resolve**——日志要在调用之前写
- 退出码会作为进程退出码带出去（在启动它的 cmd 里 `echo %errorlevel%` 能看到）

## 错误处理

同其他域：C++ 返回字符串而不是 `Error` 对象。
