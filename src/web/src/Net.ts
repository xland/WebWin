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

/** 命名管道创建选项 */
export interface NetPipeOptions {
	/**
	 * 接收端解码方式（数据下行时如何呈现 payload 字节）。默认 `utf8`：
	 * - `utf8`：当文本字符串
	 * - `base64`：把字节 base64 成字符串
	 * - `binary`：当字节（当前以 base64 字符串呈现，真正的 ArrayBuffer 下行待做）
	 */
	encoding?: 'utf8' | 'base64' | 'binary';
	/** 管道缓冲区大小（字节），可选 */
	bufferSize?: number;
}

/** `data` 事件回调收到的内容（字符串；binary 接收时也是 base64 字符串） */
export type NetData = string;

/**
 * 网络能力（net 域）。
 *
 * - **静态方法**：`Net.getAddress()` 取网卡信息（与窗口无关）。
 * - **命名管道**：`Net.createNamedPipeServer(name)` / `Net.createNamedPipeClient(name)`
 *   返回 `Net` 实例 —— 实例可 `send()` 发数据、`on('data' | 'connect' | 'close' | 'error', cb)`
 *   监听、 `close()` 关闭。
 *
 * **隔离**：命名管道与将来的 socket 通信在内部互不耦合 —— 两者共用同一套
 * connId 路由 + 事件下行（`net` 域隐藏事件），但具体的传输实现（pipe / socket）
 * 各自独立，将来加 socket 不动 pipe 的代码。
 *
 * ```ts
 * // 客户端
 * const client = await Net.createNamedPipeClient('my-pipe');
 * client.on('data', (chunk) => console.log('收到', chunk));
 * await client.send('hello');
 *
 * // 服务端（多客户端并发）：每个连进来的客户端是一个独立的 Net 实例
 * const server = await Net.createNamedPipeServer('my-pipe');
 * server.on('connect', (conn) => {
 *   conn.on('data', (chunk) => conn.send('echo: ' + chunk));
 * });
 * ```
 */
export class Net extends Ipc {
	private static readonly domain = 'net'; // 对应 C++ 侧的 Net 域处理器
	protected type = Net.domain;

	/**
	 * 不给外部 new：通过静态工厂拿到实例。
	 * `transport` 标记这个实例是 server 监听 / client 连接 / server 上的某个客户端连接，
	 * 只用于 `connect` 事件里决定回调参数形态。
	 */
	private constructor(private connId: number, private transport: 'pipe-server' | 'pipe-client' | 'pipe-conn') {
		super();
	}

	// ---- 命名管道：静态工厂（返回实例）----
	static createNamedPipeServer(name: string, options: NetPipeOptions = {}): Promise<Net> {
		return Net.request<{ connId: number }>(Net.domain, 'createNamedPipeServer', { name, ...options })
			.then(r => Net.adopt(r.connId, 'pipe-server'));
	}
	static createNamedPipeClient(name: string, options: NetPipeOptions = {}): Promise<Net> {
		return Net.request<{ connId: number }>(Net.domain, 'createNamedPipeClient', { name, ...options })
			.then(r => Net.adopt(r.connId, 'pipe-client'));
	}

	/** 取本机各网卡的名字、MAC 与 IP 地址（静态，与窗口无关） */
	static getAddress(): Promise<NetAdapter[]> {
		return Net.request<NetAdapter[]>(Net.domain, 'getAddress');
	}

	// ---- 实例级事件 + 静态路由 ----
	// 实例的 on/off 只操作本实例的 listeners，**不**向 C++ 订阅（避免多实例时一个 off 误伤其他实例）。
	// 真正向 net 域订阅的是下面那个内部 subscriber（进程级、订阅一次）。事件到达后按 connId 路由到实例。
	private static subscriber: Net | null = null;
	private static byConn = new Map<number, Net>();

	/** 登记实例并懒初始化域订阅 */
	private static adopt(connId: number, transport: 'pipe-server' | 'pipe-client' | 'pipe-conn'): Net {
		const inst = new Net(connId, transport);
		Net.byConn.set(connId, inst);
		Net.ensureSubscribed();
		return inst;
	}

	private static ensureSubscribed(): void {
		if (Net.subscriber) return;
		Net.subscriber = new Net(-1, 'pipe-server'); // 仅用于域订阅的内部实例，不入 byConn
		const route = (event: string) => (payload: any) => {
			const inst = Net.byConn.get(payload.connId);
			if (inst) inst.emit(event, payload);
		};
		// 用父类（Ipc）的域订阅，注册到 net 域的 4 个隐藏事件
		Net.subscriber.domainOn('data', route('data'));
		Net.subscriber.domainOn('connect', route('connect'));
		Net.subscriber.domainOn('close', route('close'));
		Net.subscriber.domainOn('error', route('error'));
	}

	/** 调用父类 Ipc 的域订阅（仅 subscriber 用，实例的 on 不走到这） */
	private domainOn(event: string, handler: (payload: any) => void): void {
		super.on(event, handler);
	}

	// 实例级监听者表（EventEmitter 风格）
	private listeners = new Map<string, Set<(payload: any) => void>>();

	on(event: 'data', cb: (data: NetData) => void): () => void;
	on(event: 'connect', cb: (conn: Net) => void): () => void;
	on(event: 'close', cb: () => void): () => void;
	on(event: 'error', cb: (err: { message: string }) => void): () => void;
	on(event: string, cb: (payload: any) => void): () => void {
		let set = this.listeners.get(event);
		if (!set) { set = new Set(); this.listeners.set(event, set); }
		set.add(cb);
		return () => this.off(event, cb);
	}

	off(event: string, cb: (payload: any) => void): void {
		const set = this.listeners.get(event);
		if (!set) return;
		set.delete(cb);
		if (set.size === 0) this.listeners.delete(event);
	}

	/** 把路由到的事件转交给本实例的监听者（按事件形态适配回调参数） */
	private emit(event: string, payload: any): void {
		const cbs = this.listeners.get(event);
		if (!cbs || cbs.size === 0) return;
		if (event === 'connect') {
			// server：每个新客户端连接是一个独立的 Net 实例；client：自身已连接
			const conn = this.transport === 'pipe-server'
				? Net.adopt(payload.clientConnId, 'pipe-conn')
				: this;
			cbs.forEach(cb => cb(conn));
		} else if (event === 'data') {
			cbs.forEach(cb => cb(payload.data));
		} else {
			cbs.forEach(cb => cb(payload));
		}
	}

	/** 发数据。encoding 决定如何把 data 转成管道字节（utf8 文本 / base64 串） */
	send(data: string, encoding: 'utf8' | 'base64' = 'utf8'): Promise<void> {
		return this.call<void>('send', { connId: this.connId, data, encoding });
	}

	/** 关闭连接（client / server 上的某个客户端连接），或停止监听并关闭所有连接（server） */
	close(): Promise<void> {
		const connId = this.connId;
		return this.call<void>('close', { connId }).then(() => {
			Net.byConn.delete(connId);
		});
	}
}
