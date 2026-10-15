import { Ipc } from './Ipc';

/** 新建窗口的配置：复用 package.json 里 window 那套配置结构 */
export interface WinOptions {
	[key: string]: unknown;
}

/**
 * 一个窗口。
 *
 * - 页面里的全局对象 `win` 就是**当前窗口**的实例
 * - 新窗口只能用 `Win.create()` 创建（不支持 `new Win(...)`）
 * - A 页面创建的 B 窗口，B 的事件会分发到 A 持有的这个对象上
 */
export class Win extends Ipc {
	private static readonly domain = 'win'; // 对应 C++ 侧的 Win 域处理器

	protected type = Win.domain;

	private constructor(id: number) {
		super();
		this.winId = id;
	}

	/** 创建一个新窗口，resolve 出来的对象窗口 id 已就绪 */
	static create(options: WinOptions = {}): Promise<Win> {
		return Win.request<number>(Win.domain, 'create', options).then((id) => new Win(id));
	}

	/** @internal 框架注入当前窗口对象用；遍历 / 查找窗口的公开 API 尚未定 */
	static fromId(id: number): Win {
		return new Win(id);
	}

	maximize(): Promise<void> { return this.call('maximize'); }
	minimize(): Promise<void> { return this.call('minimize'); }
	restore(): Promise<void> { return this.call('restore'); }
	hide(): Promise<void> { return this.call('hide'); }
	show(): Promise<void> { return this.call('show'); }
	resize(width: number, height: number): Promise<void> { return this.call('resize', { width, height }); }
	move(x: number, y: number): Promise<void> { return this.call('move', { x, y }); }
	close(): Promise<void> { return this.call('close'); }
	destroy(): Promise<void> { return this.call('destroy'); }
}
