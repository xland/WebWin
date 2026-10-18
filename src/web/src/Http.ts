import { Ipc } from './Ipc';

/** `Http.fetch()` 的可选项 */
export interface HttpFetchOptions {
	/** 请求方法，默认 GET。大小写不敏感 */
	method?: string;
	/** 请求头。key 大小写不敏感 —— 取回来时统一成小写 */
	headers?: Record<string, string>;
	/**
	 * 请求体。要发 JSON 就自己 `JSON.stringify`，
	 * 并带上 `content-type: application/json`（原生层不会替你猜）
	 */
	body?: string;
}

/** C++ 回传的原始数据 */
interface FetchData {
	ok: boolean;
	status: number;
	statusText: string;
	headers: Record<string, string>;
	body: string;
}

/**
 * `Http.fetch()` 的返回，字段跟 web 的 `Response` 对齐。
 *
 * 有一点不同：body 在响应到达时就**整个**取回来了（没有流式），
 * 所以 `text()` / `json()` 不会再发请求 —— 保留 async 只为跟 web fetch 用法一致。
 */
export class HttpResponse {
	/** 2xx / 3xx 为 true。4xx / 5xx **不会**让 fetch 失败，要看这个字段 */
	readonly ok: boolean;
	/** HTTP 状态码 */
	readonly status: number;
	/** 状态文本，如 `'OK'` */
	readonly statusText: string;
	/**
	 * 响应头，key **统一小写**（跟 web 的 `Headers` 一个规矩），
	 * 所以取的时候写 `res.headers['content-type']`
	 */
	readonly headers: Record<string, string>;

	private readonly body: string;

	/** @internal 由 `Http.fetch()` 构造，开发者不用自己 new */
	constructor(data: FetchData) {
		this.ok = data.ok;
		this.status = data.status;
		this.statusText = data.statusText;
		this.headers = data.headers ?? {};
		this.body = data.body ?? '';
	}

	/** 响应体原文 */
	async text(): Promise<string> {
		return this.body;
	}

	/** 按 JSON 解析响应体；不是合法 JSON 时抛 SyntaxError */
	async json<T = unknown>(): Promise<T> {
		return JSON.parse(this.body) as T;
	}
}

/**
 * http 域：发 HTTP / HTTPS 请求。
 *
 * **和页面里直接 `fetch()` 的区别**：那个走浏览器、要过 CORS —— 页面跑在
 * `https://{appId}` 这种全世界都不认识的 origin 上（`Page::loadPage` 把目录
 * 映射成了虚拟域名），服务端不白名单就拒。这个走 C++ 的 WinHTTP，
 * 不经过渲染进程，**没有 CORS 限制**。
 *
 * 这是个**类型**而不是实例：直接用 `Http.fetch()`，不要 `new Http()`。
 *
 * ```ts
 * const res = await Http.fetch('https://api.example.com/v1/ping');
 * if (res.ok) console.log(await res.json());
 * ```
 */
export class Http extends Ipc {
	private static readonly domain = 'http';

	protected type = Http.domain;

	/** 不给实例化：本域只有静态方法 */
	private constructor() {
		super();
	}

	/**
	 * 发一个请求。
	 *
	 * **网络层的失败才 reject**：URL 写错、DNS 解析不了、连不上、超时、
	 * 证书校验不过。**HTTP 层面的错误照常 resolve** —— 404 / 500 是服务端给的
	 * 正常答复，不是调用失败，要看 `res.ok` / `res.status`。这跟 web fetch 一致。
	 *
	 * ```ts
	 * const res = await Http.fetch('https://api.example.com/login', {
	 *   method: 'POST',
	 *   headers: { 'content-type': 'application/json' },
	 *   body: JSON.stringify({ user: 'a', pwd: 'b' }),
	 * });
	 * ```
	 */
	static async fetch(url: string, options: HttpFetchOptions = {}): Promise<HttpResponse> {
		const data = await Http.request<FetchData>(Http.domain, 'fetch', { url, ...options });
		return new HttpResponse(data);
	}
}
