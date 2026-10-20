import { Ipc } from './Ipc';

/** 一块网卡 */
export interface NetAdapter {
	/** 网卡名（Windows 给的那个 FriendlyName，如 `'以太网'`） */
	name: string;
	/** MAC 地址，连字符分隔的大写十六进制，如 `'1A-2B-3C-4D-5E-6F'` */
	mac: string;
	/** IPv4 / IPv6 地址；**不含**回环地址（`127.0.0.1` / `::1`） */
	addresses: string[];
}

/**
 * 网络能力（net 域）。
 *
 * 这是个**类型**而不是实例：直接用 `Net.getAddress()`，不要 `new Net()`。
 *
 * ```ts
 * for (const adapter of await Net.getAddress()) {
 *   console.log(adapter.name, adapter.addresses);
 * }
 * ```
 *
 * 网卡信息原来挂在 `os` 域下（`os.getIpAddr()`），现已迁到这里 ——
 * 网络这块以后会往这个域里加东西（联网状态变化、本机端口占用 …），
 * 跟"系统信息"不是一个方向
 */
export class Net extends Ipc {
	private static readonly domain = 'net'; // 对应 C++ 侧的 Net 域处理器

	protected type = Net.domain;

	/** 不给实例化：本域只有静态方法 */
	private constructor() {
		super();
	}

	/**
	 * 取本机各网卡的名字、MAC 与 IP 地址。
	 *
	 * **已过滤掉**三类，让返回值直接可用（想看原始清单得另外加方法）：
	 * 未启用的网卡、没有 MAC 的（回环、虚拟网卡之类）、回环地址。
	 *
	 * 一台机器通常有多块网卡（有线、Wi-Fi、虚拟机网卡…），所以返回的是**数组**；
	 * 且一块网卡可以有多个地址（IPv4 + IPv6）
	 */
	static getAddress(): Promise<NetAdapter[]> {
		return Net.request<NetAdapter[]>(Net.domain, 'getAddress');
	}
}
