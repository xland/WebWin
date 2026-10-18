import { Ipc } from './Ipc';
import { Menu } from './Menu';

/** `Tray.create()` 的参数；两个都可以不传 */
export interface TrayOptions {
	/** 鼠标悬停时的提示文字；系统最多只有 128 个字符，超了会被截 */
	tooltip?: string;
	/** 右键时弹出的菜单（左键不弹）。不传就只有事件、没有菜单 */
	menu?: Menu;
}

/**
 * 系统托盘图标。
 *
 * ```ts
 * const menu = await Menu.create([{ label: '退出', onclick: () => app.quit() }]);
 * const tray = await Tray.create({ tooltip: '我的应用', menu });
 * tray.on('doubleClick', () => win.show());
 * ```
 *
 * 三点要留意：
 * - 图标一律用 **exe 自己的图标**，v1 不支持自定义
 * - 建了托盘图标后，**最后一个窗口关掉时应用不再自动退出**
 *   （quitWhenNoWindow 会让位给托盘）。要退出就自己在菜单里放一项
 * - 菜单是创建时传的，之后换不了。菜单所属的页面被销毁后菜单也就没了，
 *   但图标还在（右键只发事件、不弹菜单）
 *
 * 事件有三个：`click` / `rightClick` / `doubleClick`，用继承来的 `on()` 订阅。
 * 双击不会再连带出一次 `click` —— C++ 侧压后派发、等过双击窗口才发单击。
 */
export class Tray extends Ipc {
	private static readonly domain = 'tray';

	protected type = Tray.domain;

	/** C++ 分配的托盘 id；开发者不感知，所以是 private */
	private readonly id: number;

	private constructor(id: number) {
		super();
		this.id = id;
	}

	/**
	 * 建一个托盘图标。**是 async**：图标是 C++ 侧的 Win32 资源，得等它建好。
	 * 一个进程可以建多个。
	 */
	static async create(options?: TrayOptions): Promise<Tray> {
		const id = await Tray.request<number>(Tray.domain, 'create', {
			tooltip: options?.tooltip,
			// Menu 是个对象，过不了消息通道：只把它的 id 送过去（同 Win.setContextMenu）
			menuId: options?.menu ? options.menu.id : undefined,
		});
		return new Tray(id);
	}

	/** 改鼠标悬停时的提示文字 */
	setToolTip(text: string): Promise<void> {
		return this.call<void>('setToolTip', { id: this.id, text });
	}

	/** 移除图标。忘了调，托盘里会一直留着 */
	destroy(): Promise<void> {
		return this.call<void>('destroy', { id: this.id });
	}
}
