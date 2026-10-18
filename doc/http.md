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
| `Http.fetch(url, options?)` | `Promise<HttpResponse>` | 发一个 HTTP / HTTPS 请求（文本 / base64 响应） |
| `Http.fetch(url, { encoding: 'binary' })` | `Promise<HttpBinaryResponse>` | 同上，响应体**字节**走共享内存，拿到 ArrayBuffer |

### 参数

```ts
interface HttpFetchOptions {
	method?: string;                    // 默认 GET，大小写不敏感
	headers?: Record<string, string>;   // 请求头
	body?: string;                      // 请求体
	encoding?: 'utf8' | 'base64' | 'binary';  // 响应体怎么给，默认 utf8
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

## 响应体：`encoding`

`encoding` 决定响应体的字节怎么交给你 —— **跟 `fs.readFile` 的 `encoding` 是同一个
概念、同一套取值**，学一次两处通用。

| encoding | 返回 | 适用 |
| --- | --- | --- |
| `utf8`（默认） | `HttpResponse`，`text()` 给字符串 | JSON、HTML、日志 |
| `base64` | `HttpResponse`，`text()` 给 base64 字符串 | 二进制无损，体积涨 1/3 |
| `binary` | `HttpBinaryResponse`，`buffer` 是 `ArrayBuffer` | 大响应、零拷贝 |

```ts
const png = await Http.fetch(url, { encoding: 'binary' });
png.buffer;      // ArrayBuffer，走共享内存
png.release();   // 用完必须还
```

> 默认 `utf8` 会把响应体**当文本解**，拿二进制会被**破坏** —— 不是"乱码"那么轻，
> 字节直接丢了。下载文件、图片、protobuf 一定要指定 `encoding`。

两个容易混的点：

- 这跟响应头里的字符集（`content-type: text/html; charset=gbk`）**不是一回事**。
  这里说的是"字节怎么过消息通道交给你"，原生层**不会**替你按字符集解码 ——
  GBK 页面用默认 `utf8` 拿回来仍然是按 UTF-8 解的结果（乱码），
  要原样字节就用 `base64` 或 `binary` 自己转。
- `binary` 走共享内存，`buffer` **只读**且必须 `release()`（见下节）。

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

## 返回：`HttpBinaryResponse`

`encoding: 'binary'` 时返回的是这个，**不是 `HttpResponse`**：

| 成员 | 类型 | 说明 |
| --- | --- | --- |
| `ok` | `boolean` | 2xx / 3xx 为 true |
| `status` | `number` | HTTP 状态码 |
| `statusText` | `string` | 状态文本 |
| `headers` | `Record<string, string>` | 响应头，**key 统一小写** |
| `size` | `number` | 响应体字节数 |
| `buffer` | `ArrayBuffer` | 响应体字节，走共享内存。**只读** |
| `release()` | `() => void` | 把共享内存还回去 |

状态行和响应头**照常是普通字段** —— 它们小，没必要走共享内存，只有 `buffer` 是。

`release()` **必须调**：不调会一直占着共享内存。而且 `buffer` 是**只读**的，
往里写会让渲染进程崩。要留一份就先拷出来：

```ts
const res = await Http.fetch(url, { encoding: 'binary' });
const blob = new Blob([res.buffer]);   // Blob 会拷一份
res.release();                          // 之后 buffer 就不能再读了
```

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

- **默认按 UTF-8 解响应体**。GBK 之类的页面会出乱码 —— 原生层不按字符集解码，
  要原样字节就用 `encoding: 'base64'` / `'binary'` 自己转。
- **同源的多个响应头会被合并**：重复的头用 `, ` 接起来（HTTP 的规矩），
  但 `set-cookie` 按规矩该是数组，这里也一起被合并了。
- **没有流式 / 进度回调**：大响应要整个收完才 resolve。
- **没有取消**：请求发出去就没有句柄能中止它。页面关掉时框架会丢掉回包
  （不会用到已销毁的页面），但线程自己会跑到完。

## 尚未支持

- **`Http.createServer()`** —— 起一个 HTTP / HTTPS 服务器。已登记为待办
  （`arch/99-todo.md`），它不可能纯 JS 做，归在本域。
- 超时 / 重定向策略 / 代理的可配置项
- 流式读取、下载进度（`binary` 已经能拿到完整字节，但仍是"整个收完才 resolve"）
- 连接池与 cookie 罐

## 实现说明

请求跑在**后台线程**上。网络请求不能在 `WebMessageReceived` 的回调栈里同步跑
（会把 UI 线程卡死，窗口拖都拖不动），所以流程是：

```
handleApi（UI 线程）→ 登记"谁在等" → co_await resume_background()（WinRT 线程池）
                                       ↓
                           WinHTTP 同步发请求
                                       ↓
        co_await resume_foreground(App::dispatcherQueue()) → finish → page->send
```

去程用 **WinRT 线程池**：线程复用，并发多个请求也不会一个请求一条 OS 线程。
回程**明确指名** UI 线程的 `DispatcherQueue` 而不是用 `winrt::apartment_context` ——
后者靠 C++/WinRT 的单元记账，而本进程是用 `CoInitializeEx` 起 COM 的，
没走 `winrt::init_apartment`，一旦没回到本线程就是"请求永远不回包、Promise 一直挂着"。

两个约束都不能反：请求不能在 UI 线程上跑；`page->send` 又必须在 UI 线程上跑。
而"页面还在不在"这件事**也必须在 UI 线程上查** —— 页面销毁
（`Win` 的 `WM_DESTROY` → `Async::removeByPage`）同样发生在 UI 线程，两者互斥，
所以在 `finish()` 里查到的 `Page*` 一定是有效的。放在后台线程里查就会有
"刚查完、还没发，窗口没了"的窗口期。

这套"后台线程 + 回 UI 线程"的机制**不是 http 域独有的**：它已抽成 `Async`
（`src/Async.h`），`fs` 域的文件读写走同一条路，清理挂起的入口也统一成
`Async::removeByPage` 一个。

`encoding: 'binary'` 时字节走共享内存（`Page::postSharedBuffer`，跟
`display.getImage`、`fs.readFile` 一条路），先 Post 再 Close。判据是显式的
`viaBuffer` 标志而不是"字节数是否为 0" —— 0 字节的响应（204）也要给页面一个空
`ArrayBuffer`，退化成普通响应的话页面拿到的是一个没有 `buffer` 字段的对象。
