# 网络 API（`net` 域）

`window.Net` 是个**类型**，不是实例 —— 直接用它的静态方法，不要 `new Net()`。

```ts
const adapters = await Net.getAddress();
```

> 网卡信息原先挂在 `os` 域下（`os.getIpAddr()`），现已迁到这里并改名。
> 网络这块以后会往这个域里加东西（联网状态变化、本机端口占用 …），
> 跟"系统信息"不是一个方向。

## 方法

| 方法 | 返回 | 说明 |
| --- | --- | --- |
| `Net.getAddress()` | `Promise<NetAdapter[]>` | 取本机各网卡的名字、MAC 与 IP 地址 |
| `Net.createNamedPipeServer(name, options?)` | `Promise<Net>` | 建命名管道服务端（多客户端并发） |
| `Net.createNamedPipeClient(name, options?)` | `Promise<Net>` | 连命名管道服务端 |

```ts
interface NetAdapter {
	/** 网卡名（Windows 给的那个 FriendlyName，如 '以太网'） */
	name: string;
	/** MAC 地址，连字符分隔的大写十六进制，如 '1A-2B-3C-4D-5E-6F' */
	mac: string;
	/** IPv4 / IPv6 地址；不含回环地址（127.0.0.1 / ::1） */
	addresses: string[];
}
```

两点：

- 一台机器通常有**多块**网卡（有线、Wi-Fi、虚拟机网卡…），所以返回的是数组；
  一块网卡也可以有**多个**地址（IPv4 + IPv6）
- 返回值已过滤掉三类，让它直接可用：未启用的网卡、没有 MAC 的（回环、
  虚拟网卡之类）、回环地址。想看原始清单得另外加方法

## 命名管道（Named Pipe）

`Net` 还能做进程间通信：建命名管道服务端 / 连客户端，返回的都是 `Net`
实例，可双向收发。`createNamedPipeServer` 是**多客户端并发**的服务端 ——
每个连进来的客户端是一个独立的 `Net` 实例，通过 `connect` 事件交给你。

```ts
// 客户端
const client = await Net.createNamedPipeClient('my-pipe');
client.on('data', (chunk) => console.log('收到', chunk));
await client.send('hello');

// 服务端：每个客户端连接是一个独立的 conn 实例
const server = await Net.createNamedPipeServer('my-pipe');
server.on('connect', (conn) => {
  conn.on('data', (chunk) => conn.send('echo: ' + chunk)); // 收到什么回什么
});
```

实例的方法 / 事件：

| 方法 / 事件 | 说明 |
| --- | --- |
| `send(data, encoding?)` | 发数据。`encoding`：`utf8`（默认，文本）/ `base64`（data 是 base64 串） |
| `on('data', cb)` | 收到数据时触发，`cb(data)` 是字符串 |
| `on('connect', cb)` | 连接建立：`client` 实例自身连上即触发；`server` 每接受一个客户端触发，`cb(conn)` 是那个客户端的 `Net` 实例 |
| `on('close', cb)` | 连接断开 |
| `on('error', cb)` | 出错（如被动断开），`cb({ message })` |
| `close()` | 关闭：客户端关自己；服务端停止监听并关掉所有客户端连接 |

几个要点：

- **消息有帧边界**：管道是字节流，框架在每条消息前加 4 字节小端长度前缀，
  自动按帧拆包，你拿到的 `data` 是完整的一条，不会半条或半条拼一起。
- **编码**：发送端 `send(data, encoding)` 决定 payload 字节（utf8 文本 / base64 解码）；
  接收端由创建时的 `options.encoding` 决定如何呈现 payload 字节
  （`utf8` 文本 / `base64` 串）。`binary` 接收**当前降级为 base64 字符串**，
  真正的 `ArrayBuffer` 下行待做。
- **要关闭**：用完调 `close()`；页面销毁时该页建的连接会自动关（读线程不空转）。
- **与 socket 隔离**：命名管道和将来的 socket 通信在内部互不耦合 —— 共用同一套
  connId 路由 + 事件下行，但具体传输实现各自独立，加 socket 不动 pipe 代码。

## 错误处理

同其他域：C++ 返回字符串而不是 `Error` 对象。这个方法只在
**底层拿不到网卡信息**时才 reject（比如 Winsock 起不来）。

```ts
try {
	const adapters = await Net.getAddress();
} catch (e) {
	const msg = e && e.message ? e.message : e;
	console.error('取网卡信息失败：', msg);
}
```

## 尚未支持

- 输入法 / 代理 / 主机名等网络环境信息
- 联网状态变化的事件（`IsNetworkAlive` + `NotifyAddrChange` 那条路）
- 本机端口占用查询（`GetExtendedTcpTable`）
- 网卡状态的实时监听（插拔网线、切 Wi-Fi）
