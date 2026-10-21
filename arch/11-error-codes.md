# 错误码表（error code）

协议里 `error` 字段从「纯字符串」升级为「结构化对象」，便于跨域一致性、程序化判断
（`if (e.code === 'fs.notFound')`），同时保留人类可读的 `message`。

> 关联：`02-architecture.md` 的消息信封、`09-...`（消息协议）、`99-todo.md` 的 T011。
> JS 侧 `WebWinError` / `toError` 早已预留 `{ code, message }` 结构，本表是补上 C++ 侧的 `code` 产出
> 并约定命名。

## 错误结构

```jsonc
{ "ok": false, "error": { "code": "fs.notFound", "message": "文件不存在：x.txt" } }
```

- `code`：点分字符串 `domain.reason`，全局唯一、可读、可程序化匹配。
- `message`：人类可读文本（可能含动态细节，如路径、系统错误码），**不做**本地化约定。
- 向后兼容：旧调用若只传 `message`，仍输出纯字符串；JS `toError` 两种形态都兼容。

## 命名约定

```
code = {domain}.{reason}
```

- `domain`：能力域名（见各域 `handleApi` 的 `type`），如 `fs` / `net` / `win` / `dialog` /
  `db` / `http` / `display` / `tray` / `process` / `menu` / `os` / `env` / `res` / `app`。
- `reason`：错误原因。优先复用下方**通用 reason 词表**；域特有的原因才自定义。

### 通用 reason 词表（跨域复用，避免各域各起一套）

| reason | 含义 | 示例 |
| --- | --- | --- |
| `invalidParam` | 参数缺失 / 非法 / 类型不对 | `path` 为空、`size` 非正数、base64 不合法 |
| `notFound` | 目标不存在 | 文件不存在、连接已关闭、db 不存在 |
| `notSupported` | 当前环境 / 配置不支持该能力 | WebView2 版本过低不支持共享内存 |
| `permissionDenied` | 权限不足 | 文件无读 / 写权限 |
| `busy` | 资源忙 / 已占用 | 管道实例忙 |
| `destroyed` | 对象已销毁（窗口关了、页面没了） | 弹对话框时窗口已没 |
| `timeout` | 超时 | |
| `error` | 兜底：底层失败透传（message 带具体原因） | sqlite 执行失败、发送失败 |

### 各域专属 reason（示例，非穷举；域可在此基础上扩展）

| code | 触发场景 |
| --- | --- |
| `core.unknownMethod` | 调用了域未实现的方法名 |
| `core.invalidParam` | 协议层参数非法（如事件订阅名非法） |
| `fs.notFound` | 文件 / 目录不存在 |
| `fs.isDirectory` | 期望文件却是目录 |
| `fs.encodeInvalid` | `data` 不是合法 base64（写文件时） |
| `fs.ioError` | 读写失败（message 带系统错误） |
| `net.pipe.connectFailed` | 连不上命名管道服务端 |
| `net.connClosed` | 连接不存在或已关闭 |
| `net.pipe.createFailed` | 创建管道实例失败 |
| `win.invalidSubscription` | 窗口事件订阅名非法 |
| `win.notFound` | 窗口不存在（id 找不到） |
| `win.destroyed` | 窗口 / 页面已销毁 |
| `win.invalidState` | 窗口状态不允许该操作（如已 destroy 再 close） |
| `dialog.windowGone` | 弹框时所属窗口已没 |
| `dialog.showFailed` | 文件 / 保存对话框创建、显示失败，消息框显示失败、调度失败 |
| `dialog.error` | 兜底：COM 调用 / 读取所选文件等细节失败 |
| `db.notFound` | 指定 id 的数据库连接不存在 |
| `db.sqlMissing` | 没给 SQL 语句 |
| `db.sqlError` | SQL 执行 / 查询失败（message 带 sqlite 报错） |
| `http.invalidParam` | `url` 为空 |
| `http.createFailed` | 创建请求线程失败 |
| `display.notFound` | 找不到主显示器 / 没有显示器 |
| `display.captureFailed` | 截图失败（DC / 编码 / 共享内存） |
| `display.invalidRegion` | 截图区域无效 |
| `display.unknownEvent` | 订阅了不存在的显示器事件 |
| `app.invalidSubscription` | 事件订阅名非法 |

## JS 侧用法

```ts
try {
  await fs.readFile('x.txt');
} catch (e) {
  if (e.code === 'fs.notFound') { /* 处理文件不存在 */ }
  else throw e; // 不认识的 code 原样抛出
}
```

`e` 是普通 `Error`，`code` 挂在上面（`e.code`）；`e.message` 仍是可读文本。

## 接入状态

- 协议 / JS：`Json::setError(result, code, message)` 重载已支持；`toError` 已兼容两种形态。
- **已接入**：`fs`、`net`（见各自 `handleApi`）。
- **按约定渐进补**：其余域按上表在各自 `handleApi` / 私有方法里把 `setError(result, msg)`
  升级为 `setError(result, "domain.reason", msg)`；动态透传类（如 sqlite 报错）用 `domain.error`。
  新增域时默认带 `domain.error` 兜底，再挑高频错误给具体 reason。
