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
