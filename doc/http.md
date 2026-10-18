# HTTP API（`Http`）

`Http` 是个**类型**（不是实例），目前只有一个静态方法 `fetch`：

```ts
const res = await Http.fetch('https://api.example.com/v1/ping');
if (res.ok) console.log(await res.json());
```

## 为什么不用页面里的 `fetch()`

页面跑在 `https://{appId}` 这种**全世界都不认识的 origin** 上（框架把目录映射成了
虚拟域名，`appId` 在 package.json 里配，默认 `webwin`）。所以页面里直接
`fetch('https://api.example.com')` 会带上 `Origin: https://webwin` 走标准 CORS 检查，
服务端不白名单就直接拒 —— 而绝大多数服务端都不认识这个来源。

`Http.fetch()` 走 C++ 的 WinHTTP，**不经过渲染进程，没有 CORS 这回事**：

| | 页面里的 `fetch()` | `Http.fetch()` |
| --- | --- | --- |
| CORS | 受限制，服务端要配白名单 | **无限制** |
| 自定义请求头 | 受限（CORS 预检要放行） | 随便给 |
| 读到响应体 | 受 CORS 约束 | 都能读 |
| 跑在哪 | 渲染进程 | 原生进程（不占页面主线程） |

代价是它不像浏览器那样带 cookie 罐、缓存、凭据管理 —— 需要什么头自己给。

## 方法

| 方法 | 返回 | 说明 |
| --- | --- | --- |
| `Http.fetch(url, options?)` | `Promise<HttpResponse>` | 发一个 HTTP / HTTPS 请求 |

### 参数

```ts
interface HttpFetchOptions {
	method?: string;                    // 默认 GET，大小写不敏感
	headers?: Record<string, string>;   // 请求头
	body?: string;                      // 请求体
}
```

要发 JSON 就自己 `JSON.stringify`，**并带上 content-type** —— 原生层不会替你猜：

```ts
const res = await Http.fetch('https://api.example.com/login', {
	method: 'POST',
	headers: { 'content-type': 'application/json' },
	body: JSON.stringify({ user: 'a', pwd: 'b' }),
});
```

## 返回：`HttpResponse`

字段跟 web 的 `Response` 对齐：

| 成员 | 类型 | 说明 |
| --- | --- | --- |
| `ok` | `boolean` | 2xx / 3xx 为 true |
| `status` | `number` | HTTP 状态码 |
| `statusText` | `string` | 状态文本，如 `'OK'` |
| `headers` | `Record<string, string>` | 响应头，**key 统一小写** |
| `text()` | `Promise<string>` | 响应体原文 |
| `json<T>()` | `Promise<T>` | 按 JSON 解析；非法 JSON 抛 `SyntaxError` |

响应头 key 是小写的，所以取的时候写 `res.headers['content-type']`。

> body 在响应到达时就**整个**取回来了，没有流式。所以 `text()` / `json()`
> 不会再发请求 —— 保留 `async` 只是为了跟 web fetch 的用法一致。

## 错误处理：什么会 reject，什么不会

这条最容易搞错：

```ts
// ❌ 网络层失败才 reject：URL 写错 / DNS 解析不了 / 连不上 / 超时 / 证书校验不过
try {
	const res = await Http.fetch('https://api.example.com/a');
	// ✅ 走到这里说明网络通了，但可能是 404 / 500 —— 那不算调用失败
	if (!res.ok) {
		console.error('服务端返回', res.status);
		return;
	}
	const data = await res.json();
} catch (e) {
	console.error('请求没发出去：', e.message);
}
```

**404 / 500 是服务端给的正常答复，不是调用失败**，所以照常 resolve，要看
`res.ok` / `res.status`。这跟 web fetch 一致。

## 超时与重定向

- 超时是固定的：解析域名 10 秒、连接 10 秒、发送 30 秒、接收 30 秒。
  目前**不能自己配**（v1 没开这个参数）。
- **自动跟随重定向**，但拒绝 `https` 降级到 `http` —— 那会悄悄把加密连接
  变成明文。这是 WinHTTP 的默认策略。
- HTTPS **正常校验证书**。想连自签名证书的服务器，目前没有开关。

## 已知限制（v1）

都是"这次没做"，不是 bug：

- **响应体按 UTF-8 解**。GBK 之类的页面会出乱码，需要自己转。
- **不支持二进制**。下载文件、图片这类场景目前拿不到原始字节
  （响应体会被当文本解，二进制内容会被破坏）。
- **同源的多个响应头会被合并**：重复的头用 `, ` 接起来（HTTP 的规矩），
  但 `set-cookie` 按规矩该是数组，这里也一起被合并了。
- **没有流式 / 进度回调**：大响应要整个收完才 resolve。
- **没有取消**：请求发出去就没有句柄能中止它。页面关掉时框架会丢掉回包
  （不会用到已销毁的页面），但线程自己会跑到完。

## 尚未支持

- **`Http.createServer()`** —— 起一个 HTTP / HTTPS 服务器。已登记为待办
  （`arch/99-todo.md`），它不可能纯 JS 做，归在本域。
- 超时 / 重定向策略 / 代理的可配置项
- 二进制响应体、流式读取、下载进度
- 连接池与 cookie 罐

## 实现说明

请求跑在**后台线程**上。网络请求不能在 `WebMessageReceived` 的回调栈里同步跑
（会把 UI 线程卡死，窗口拖都拖不动），所以流程是：

```
handleApi（UI 线程）→ 登记"谁在等" → std::thread
                                       ↓
                           WinHTTP 同步发请求
                                       ↓
              App::runLater → finish（回到 UI 线程）→ page->send
```

两个约束都不能反：请求不能在 UI 线程上跑；`page->send` 又必须在 UI 线程上跑。
而"页面还在不在"这件事**也必须在 UI 线程上查** —— 页面销毁
（`Win` 的 `WM_DESTROY` → `Http::removeByPage`）同样发生在 UI 线程，两者互斥，
所以在 `finish()` 里查到的 `Page*` 一定是有效的。放在后台线程里查就会有
"刚查完、还没发，窗口没了"的窗口期。
