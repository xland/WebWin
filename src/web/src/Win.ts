import { Ipc } from './Ipc';
import type { Menu } from './Menu'; // 只要类型：不引运行时依赖，避免与 Ipc 成环

/** 新建窗口的配置：复用 package.json 里 window 那套配置结构 */
export interface WinOptions {
	[key: string]: unknown;
}

/** 窗口的一条边或一个角：铺把手、发起缩放时的方向 */
export type ResizeEdge =
	| 'top' | 'bottom' | 'left' | 'right'
	| 'topLeft' | 'topRight' | 'bottomLeft' | 'bottomRight';

/** 全部 8 个方向。顺序即创建顺序：角排在边后面，重叠处归角（后建的盖在上面） */
const ALL_EDGES: readonly ResizeEdge[] = [
	'top', 'bottom', 'left', 'right',
	'topLeft', 'topRight', 'bottomLeft', 'bottomRight',
];

/** 把手的厚度（CSS px）：角给得宽一点，更好点中 */
const EDGE_THICKNESS = 6;
const CORNER_SIZE = 8;

/** 各方向的鼠标指针：跟原生边框一致 */
const EDGE_CURSOR: Record<ResizeEdge, string> = {
	top: 'ns-resize',
	bottom: 'ns-resize',
	left: 'ew-resize',
	right: 'ew-resize',
	topLeft: 'nwse-resize',
	bottomRight: 'nwse-resize',
	topRight: 'nesw-resize',
	bottomLeft: 'nesw-resize',
};

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

	/** 当前铺着的 resize 把手 */
	private edgeEls: HTMLElement[] = [];
	/** 开发者显式干预过把手（set / remove）→ 框架不再自动铺 */
	private edgesHandled = false;
	/** resize 订阅只挂一次 */
	private edgeResizeBound = false;

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

	/** 窗口当前是否处于最大化 */
	isMaximized(): Promise<boolean> { return this.call<boolean>('isMaximized'); }

	/**
	 * 进入系统原生的"移动窗口"循环。
	 *
	 * 给自定义标题栏用：在标题栏区域 `mousedown` 时调它，之后的拖动就交给系统，
	 * 贴边分屏、最大化动画这些原生行为都还在。
	 */
	startDrag(): Promise<void> { return this.call('startDrag'); }

	/**
	 * 进入系统原生的"改变窗口大小"循环，方向由 edge 指定。
	 *
	 * 给自定义边框用：`win.setResizeEdges()` 铺出来的把手内部就是调它。
	 */
	startResize(edge: ResizeEdge): Promise<void> { return this.call('startResize', { edge }); }

	/** 弹出系统为窗口准备的那个菜单（还原 / 移动 / 大小 / 最小化 / 最大化 / 关闭） */
	showSystemMenu(): Promise<void> { return this.call('showSystemMenu'); }

	/**
	 * 挂右键菜单：在本窗口里点右键就弹它。
	 * 传 null 解绑 —— 之后重新走浏览器默认的右键菜单
	 */
	setContextMenu(menu: Menu | null): Promise<void> {
		return this.call('setContextMenu', { menuId: menu ? menu.id : -1 });
	}

	/**
	 * 在窗口的四边 / 四角铺透明把手，拖它们即改变窗口大小。
	 *
	 * - 不传参数：8 个全建（4 条边 + 4 个角）
	 * - 传一个或一组方向：只建指定的那些
	 * - **每次调用先清掉上一批**，不会叠加
	 *
	 * 窗口最大化时把手没意义（系统本来也不让改大小），会自动隐藏。
	 */
	async setResizeEdges(edges?: ResizeEdge | ResizeEdge[]): Promise<void> {
		const wanted = new Set<ResizeEdge>(edges === undefined ? ALL_EDGES : (Array.isArray(edges) ? edges : [edges]));
		for (const edge of wanted) {
			if (!ALL_EDGES.includes(edge)) throw new Error(`unknown resize edge: ${edge}`);
		}
		await this.removeResizeEdges();
		const host = document.body ?? document.documentElement;
		for (const edge of ALL_EDGES) {
			if (!wanted.has(edge)) continue; // 按既定顺序建，保证角压在边之上
			const el = this.createEdgeEl(edge);
			host.appendChild(el);
			this.edgeEls.push(el);
		}
		this.bindEdgeResize();
		await this.syncEdgeVisibility();
	}

	/**
	 * 拆掉全部把手。
	 *
	 * 顺带也**阻止**框架之后自动铺：在页面自己的脚本里（DOM ready 之前）调一次，
	 * 就等于对自动创建投了反对票。
	 */
	async removeResizeEdges(): Promise<void> {
		this.edgesHandled = true;
		for (const el of this.edgeEls) el.remove();
		this.edgeEls = [];
	}

	/**
	 * @internal 框架安装 SDK 后调用，开发者不用管。
	 *
	 * `autoEdges` 为 true 时（无边框 + 可缩放的窗口）自动铺 8 个把手；
	 * `[data-caption]` 的标题栏绑定则无条件做。
	 */
	bindFramelessChrome(autoEdges: boolean): void {
		if (window.top !== window) return; // iframe 不参与：它不该去拖顶层窗口
		this.bindCaptionDrag();
		if (!autoEdges) return;
		const run = () => { if (!this.edgesHandled) void this.setResizeEdges(); };
		// SDK 在文档创建时就注入了（比页面自己的脚本还早），那时 body 都还不存在。
		// 推迟到 DOM ready 有两个好处：能拿到 body，且页面有机会先调 removeResizeEdges 阻止
		if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', run, { once: true });
		else run();
	}

	/** 造一个把手：只用来接 mousedown，本身完全透明 */
	private createEdgeEl(edge: ResizeEdge): HTMLElement {
		const el = document.createElement('div');
		el.dataset.webwinResize = edge; // 便于在 DevTools 里认出它们
		const s = el.style;
		s.position = 'fixed';
		s.zIndex = '2147483647'; // 顶层：不被页面任何元素盖住
		s.userSelect = 'none';
		s.touchAction = 'none';
		s.cursor = EDGE_CURSOR[edge];

		const e = `${EDGE_THICKNESS}px`;
		const c = `${CORNER_SIZE}px`;
		// 边铺满整条（含两端）；角后建，压在边之上 —— 重叠处归角
		if (edge === 'top') { s.top = '0'; s.left = '0'; s.right = '0'; s.height = e; }
		else if (edge === 'bottom') { s.bottom = '0'; s.left = '0'; s.right = '0'; s.height = e; }
		else if (edge === 'left') { s.left = '0'; s.top = '0'; s.bottom = '0'; s.width = e; }
		else if (edge === 'right') { s.right = '0'; s.top = '0'; s.bottom = '0'; s.width = e; }
		else if (edge === 'topLeft') { s.top = '0'; s.left = '0'; s.width = c; s.height = c; }
		else if (edge === 'topRight') { s.top = '0'; s.right = '0'; s.width = c; s.height = c; }
		else if (edge === 'bottomLeft') { s.bottom = '0'; s.left = '0'; s.width = c; s.height = c; }
		else { s.bottom = '0'; s.right = '0'; s.width = c; s.height = c; } // bottomRight

		el.addEventListener('mousedown', (ev) => {
			if (ev.button !== 0) return;
			ev.preventDefault(); // 别选中文本
			void this.startResize(edge);
		});
		return el;
	}

	/** 最大化时把手藏起来：系统本就不允许改大小，留着只会挡住内容 */
	private async syncEdgeVisibility(): Promise<void> {
		if (this.edgeEls.length === 0) return;
		let max = false;
		try { max = await this.isMaximized(); }
		catch { return; } // 窗口已关：没什么好同步的
		for (const el of this.edgeEls) el.style.display = max ? 'none' : 'block';
	}

	private bindEdgeResize(): void {
		if (this.edgeResizeBound) return;
		this.edgeResizeBound = true;
		// 最大化 / 还原都会触发 resize 事件，据此同步把手的显隐
		this.on('resize', () => { void this.syncEdgeVisibility(); });
	}

	/** 把 `[data-caption]` 标出来的区域接成标题栏：拖拽移动、双击最大化、右键出系统菜单 */
	private bindCaptionDrag(): void {
		// 判"这次点击是否落在可拖的标题栏上"。
		// 委托到 document 而不是一次性查询元素：页面后来动态插入的标题栏也生效，
		// 且不必关心 DOM 变动
		const onCaption = (ev: Event): boolean => {
			const target = ev.target;
			if (!(target instanceof Element)) return false;
			if (!target.closest('[data-caption]')) return false;
			// 标题栏里的按钮：落在 [data-caption="no-drag"] 子树里就不拖，留给按钮自己收点击
			return !target.closest('[data-caption="no-drag"]');
		};

		document.addEventListener('mousedown', (ev) => {
			if (!onCaption(ev)) return;
			if (ev.button === 2) { void this.showSystemMenu(); return; }
			if (ev.button !== 0) return;
			ev.preventDefault(); // 拖的时候别选中文字
			// detail === 2 说明这是双击的第二下：直接走最大化 / 还原。
			// 不能在这里先起拖 —— 第一下已经进了系统的移动循环，第二下的消息到不了页面
			if (ev.detail === 2) { void this.toggleMaximize(); return; }
			// 最大化时拖动要"先还原再跟手"，这一步在 C++ 的 startDrag 里做
			// （见 Win.cpp 的 restoreUnderCursor）—— 放 JS 里多一次往返，还容易摆错位置
			void this.startDrag();
		});
		// 让位给系统菜单：否则浏览器的右键菜单会跟它一起冒出来
		document.addEventListener('contextmenu', (ev) => {
			if (onCaption(ev)) ev.preventDefault();
		});
	}

	private async toggleMaximize(): Promise<void> {
		if (await this.isMaximized()) await this.restore();
		else await this.maximize();
	}
}
