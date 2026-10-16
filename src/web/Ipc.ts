/**
 * WebWin JS SDK —— 通信基类
 *
 * 开发者不会直接操作本类：通道方法是 protected，也不出现在 .d.ts 的公开面上。
 * 各能力域（Win / Clipboard / Dialog ...）都继承它，对外只暴露语义化的方法。
 *
 * 消息协议（见 arch/02-architecture.md）：
 *   上行请求 { id, type, method, params, winId? }
 *   下行响应 { id, ok, data | error }
 *   下行事件 { name, payload, winId? }
 *
 * type 是能力域（`win` / `dialog` ...），C++ 按它把消息路由到对应的域处理器。
 * winId 是消息的路由凭据：一条消息从 A 页面发出时，C++ 靠它知道要操作哪个窗口。
 */

/** 框架注入 SDK 前写入 window 的配置 */
export interface WebWinConfig {
	allowRemote: boolean;
	origin: string;
	/** 当前窗口的 id，由 C++ 注入；SDK 据此构造全局 win 对象 */
	winId?: number;
}

export type Payload = unknown;
export type Handler<T = unknown> = (payload: T) => void;

/**
 * C++ 侧下行的 error 字段。
 * 目前是字符串（错误码表尚未定义），将来会改成 { code, message }，故两种都兼容。
 */
export type WebWinError = string | { code?: string; message?: string };

/**
 * 走共享内存返回的大块数据（见 arch/07-web.md 第八节）。
 *
 * 具体能力会在 `buffer` 之外再带上自己的字段（比如截图的 width / height）。
 * `buffer` 是**只读**的：往里写会让渲染进程崩，所以用完调 `release()` 还回去。
 */
export interface SharedBufferData {
	/** 原始字节（PNG、文件内容 …） */
	buffer: ArrayBuffer;
	/** 用完必须调用：把共享内存还给 C++，不调会一直占着 */
	release(): void;
	[key: string]: unknown;
}

/** C++ 用 PostSharedBufferToScript 推下来的事件（页面侧是 sharedbufferreceived） */
interface SharedBufferReceivedEvent {
	/** additionalDataAsJson 解析后的对象：C++ 把 { id, ok, data } 信封放在这里 */
	additionalData?: unknown;
	/** 取共享内存里的字节；同一个事件只能取一次 */
	getBuffer(): ArrayBuffer;
}

interface WebViewChannel {
	postMessage(message: unknown): void;
	postMessageWithAdditionalObjects?(message: unknown, additionalObjects: unknown[]): void;
	addEventListener(type: 'message', listener: (event: { data: unknown }) => void): void;
	addEventListener(type: 'sharedbufferreceived', listener: (event: SharedBufferReceivedEvent) => void): void;
	releaseBuffer(buffer: ArrayBuffer): void;
}

let channel: WebViewChannel | null = null;
const pending = new Map<number, { resolve: (value: unknown) => void; reject: (reason: unknown) => void }>();
const listeners = new Map<string, Set<Handler>>();
let nextId = 1;

/**
 * 菜单点击的分发器，由 webwin.ts 启动时接上。
 * 这里不能直接 import Menu：Menu extends Ipc，反向依赖会成环（且运行时可能踩到 TDZ）
 */
let dispatchMenuClick: ((msg: unknown) => void) | undefined;

/** @internal 接上菜单点击的分发器 */
export function setMenuClickDispatcher(fn: (msg: unknown) => void): void {
	dispatchMenuClick = fn;
}

/** 订阅表按"窗口 + 事件名"分组：同一个事件可被多个页面订阅，彼此不冲突 */
function keyOf(winId: number | null, name: string): string {
	return `${winId === null ? '' : winId}:${name}`;
}

/**
 * 建立通道并注册接收。成功返回注入的配置，失败返回 null（此时不应安装 SDK）。
 * @internal
 */
export function installChannel(): WebWinConfig | null {
	const w = window as unknown as {
		win?: unknown;
		chrome?: { webview?: WebViewChannel };
		__webwinConfig__?: WebWinConfig;
	};
	// 装不上就说明"当前文档不是应用自己的页面"：通常是来源没对上。
	// 静默失败最难排查（页面里只会表现为 win 未定义），所以每条分支都留一句警告
	const warn = (reason: string, detail?: unknown) => {
		console.warn(`[WebWin] SDK 未安装：${reason}`, detail ?? '');
	};

	if (w.win) return null; // 每个 frame 都会执行本脚本（含 iframe），只装一次
	const ch = w.chrome?.webview;
	if (!ch) { warn('不在 WebView2 环境中（chrome.webview 缺失）'); return null; }
	const config = w.__webwinConfig__;
	if (!config) { warn('window.__webwinConfig__ 缺失（C++ 未注入配置）', location.href); return null; }

	// 默认只装到应用自身的页面；allowRemote 打开后不做来源判断
	const origin = location.origin;
	const isAppOrigin = origin === config.origin || origin === 'null' || origin === '' || origin === 'file://';
	if (!config.allowRemote && !isAppOrigin) {
		warn('页面来源与配置不符', { 页面来源: origin, 配置来源: config.origin });
		return null;
	}

	channel = ch;
	ch.addEventListener('message', (event) => {
		const raw = typeof event.data === 'string' ? parse(event.data) : event.data;
		if (!raw || typeof raw !== 'object') return;
		const msg = raw as {
			id?: number; ok?: boolean; data?: unknown; error?: WebWinError;
			name?: string; payload?: unknown; winId?: number;
			menu?: number; // 菜单项点击：既不是事件，也不是某个请求的响应
		};

		// 菜单项点击：靠 menu 字段认（menu = 菜单 id，item = 菜单项 id）
		if (typeof msg.menu === 'number') {
			dispatchMenuClick?.(msg);
			return;
		}
		// 事件推送：无 id，靠 name 区分
		if (typeof msg.name === 'string') {
			dispatch(msg.winId ?? null, msg.name, msg.payload);
			return;
		}
		// api 响应：靠 id 关联到发起的调用
		if (typeof msg.id === 'number') {
			const waiter = pending.get(msg.id);
			if (!waiter) return;
			pending.delete(msg.id);
			if (msg.ok) waiter.resolve(msg.data);
			else waiter.reject(toError(msg.error));
		}
	});

	// 共享内存：C++ 用 PostSharedBufferToScript 推下来的大块数据（截图 / 读文件）。
	// 它不进主通道，靠 additionalData 里的 id 关联回某次调用
	ch.addEventListener('sharedbufferreceived', (event) => {
		const buffer = event.getBuffer(); // 必须先取出来，否则无从释放
		const release = () => ch.releaseBuffer(buffer);
		const raw = event.additionalData;
		if (!raw || typeof raw !== 'object') { release(); return; }
		const msg = raw as { id?: number; ok?: boolean; data?: unknown; error?: WebWinError };
		const waiter = typeof msg.id === 'number' ? pending.get(msg.id) : undefined;
		if (!waiter) { release(); return; } // 没人在等（页面已切走）：直接还回去
		pending.delete(msg.id as number);
		if (!msg.ok) { release(); waiter.reject(toError(msg.error)); return; }
		// data 里的字段（width / height …）和 buffer 一起交给调用方
		waiter.resolve({ ...(msg.data as Record<string, unknown> | undefined), buffer, release });
	});
	return config;
}

export class Ipc {
	/**
	 * 能力域标识（`win` / `dialog` / `clipboard` ...），子类覆盖。
	 * C++ 侧按它把消息路由到对应的域处理器；事件订阅也靠它带上域信息。
	 */
	protected type: string = '';

	/** 关联的窗口 id；null 表示与窗口无关（clipboard / dialog 这类单例能力） */
	protected winId: number | null = null;

	/**
	 * 调用 C++ 暴露的能力：type 指能力域，method 指该域下的具体动作。
	 * additionalObjects 用于携带 File 等无法 JSON 序列化的对象
	 */
	protected invoke<T = unknown>(type: string, method: string, params?: Payload, additionalObjects?: unknown[]): Promise<T> {
		return Ipc.request<T>(type, method, params, this.winId, additionalObjects);
	}

	/**
	 * 调本域的方法：type 取 `this.type`，不必在每个方法里重复写域名。
	 * 能力域里绝大多数调用都走这里；`invoke` 留给需要显式指定域的场合。
	 */
	protected call<T = unknown>(method: string, params?: Payload, additionalObjects?: unknown[]): Promise<T> {
		return Ipc.request<T>(this.type, method, params, this.winId, additionalObjects);
	}

	// 下面两个是**开发者用**的事件 API（`win.on(...)` / `os.on(...)`），所以是公开的；
	// 上面几个通道方法才 protected —— 开发者不需要自己拼消息
	/** 订阅事件；返回取消订阅的函数 */
	on<T = unknown>(name: string, handler: Handler<T>): () => void {
		const key = keyOf(this.winId, name);
		let set = listeners.get(key);
		if (!set) {
			set = new Set();
			listeners.set(key, set);
		}
		const first = set.size === 0;
		set.add(handler as Handler);
		// 订阅后推：只有第一个订阅者出现时才通知 C++，避免推没人要的事件
		if (first) void Ipc.request(this.type, 'on', { name }, this.winId).catch(reportError);
		return () => this.off(name, handler);
	}

	/** 取消订阅 */
	off<T = unknown>(name: string, handler: Handler<T>): void {
		const key = keyOf(this.winId, name);
		const set = listeners.get(key);
		if (!set) return;
		set.delete(handler as Handler);
		// 最后一个订阅者撤走时才通知 C++，之后这个事件不再推送
		if (set.size === 0) {
			listeners.delete(key);
			 void Ipc.request(this.type, 'off', { name }, this.winId).catch(reportError);
		}
	}

	/** 供子类在还没有实例时发请求（如 Win.create 要先拿到窗口 id） */
	protected static request<T = unknown>(
		type: string,
		method: string,
		params?: Payload,
		winId: number | null = null,
		additionalObjects?: unknown[]
	): Promise<T> {
		return new Promise<T>((resolve, reject) => {
			if (!channel) {
				reject(new Error('WebWin SDK is not available'));
				return;
			}
			const id = nextId++;
			pending.set(id, { resolve: resolve as (value: unknown) => void, reject });
			const message: Record<string, unknown> = { id, type, method, params };
			if (winId !== null) message.winId = winId;
			if (additionalObjects && additionalObjects.length && channel.postMessageWithAdditionalObjects) {
				channel.postMessageWithAdditionalObjects(message, additionalObjects);
			} else {
				channel.postMessage(message);
			}
		});
	}
}

function dispatch(winId: number | null, name: string, payload: unknown): void {
	const set = listeners.get(keyOf(winId, name));
	if (!set) return;
	for (const handler of set) {
		try {
			handler(payload);
		} catch (e) {
			console.error('[WebWin] event handler error:', name, e);
		}
	}
}

function reportError(e: unknown): void {
	console.error('[WebWin]', e instanceof Error ? e.message : e);
}

function parse(text: string): unknown {
	try {
		return JSON.parse(text);
	} catch {
		return undefined;
	}
}

function toError(error: WebWinError | undefined): Error {
	if (typeof error === 'string') return new Error(error);
	const message = error?.message ?? error?.code ?? 'unknown error';
	const err = new Error(message);
	if (error?.code) (err as unknown as { code: string }).code = error.code;
	return err;
}
