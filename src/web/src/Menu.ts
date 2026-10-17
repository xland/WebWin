import { Ipc, addMessageHook } from './Ipc';

/** 菜单项类型；不写就是 normal */
export type MenuItemType = 'normal' | 'radio' | 'checkbox' | 'separator';

export interface MenuItem {
	type?: MenuItemType;
	/** 显示文字；separator 不需要 */
	label?: string;
	/** 不填会自动生成；**同一个菜单里出现两个相同的 id 会报错** */
	id?: string;
	/** radio / checkbox 的初始勾选状态 */
	checked?: boolean;
	disabled?: boolean;
	/** 子菜单，里面还可以再嵌子菜单 */
	submenu?: MenuItem[];
	onclick?: (info: MenuClickInfo) => void;
}

/** 菜单项点击回调的参数 */
export interface MenuClickInfo {
	/** 点击后的勾选状态；只有 radio / checkbox 才有 */
	checked?: boolean;
}

/**
 * 右键菜单。
 *
 * 菜单**与窗口无关**：建好之后可以挂到任意窗口上（`win.setContextMenu`），
 * 点击也一律推给**创建它的那个页面**（顶级文档或那个 iframe）。
 *
 * 菜单项分四种：
 * - `normal`：点了就触发 onclick
 * - `radio`：**连在一起**的 radio 算同一组，同组只留一个勾选
 * - `checkbox`：每次点击在勾选 / 不勾选之间切换
 * - `separator`：分隔线（分隔线也会断开 radio 组）
 *
 * 带 `submenu` 的项本身不可点击，给它配 id / onclick 无效。
 */
export class Menu extends Ipc {
	private static readonly domain = 'menu';

	/** 菜单 id → 实例：点击消息按 id 找回实例，再按 item id 找回回调 */
	private static readonly registry = new Map<number, Menu>();

	protected type = Menu.domain;

	/**
	 * C++ 分配的菜单 id。
	 *
	 * @internal 不是给开发者用的。之所以还得暴露，是因为 `Win.setContextMenu`
	 * 要把它交给 C++（同一包内的类之间要传）。想彻底私有就得让 Menu 反过来替
	 * Win 组装整个上行消息，为这点好处引入双向依赖不划算 —— 沿用本项目
	 * `@internal` 的约定（同 Menu.dispatch）即可。
	 */
	readonly id: number;

	private readonly handlers: Map<string, (info: MenuClickInfo) => void>;

	private constructor(id: number, handlers: Map<string, (info: MenuClickInfo) => void>) {
		super();
		this.id = id;
		this.handlers = handlers;
	}

	/**
	 * 建菜单。**是 async**：菜单是 C++ 里的 Win32 对象，得等它建好才拿得到 id。
	 */
	static async create(items: MenuItem[]): Promise<Menu> {
		const handlers = new Map<string, (info: MenuClickInfo) => void>();
		// 补 id 和校验都在 JS 侧做完，C++ 拿到的每一项都带 id，不必再查一遍
		const spec = Menu.toSpec(items, handlers, { used: new Set<string>(), seq: 0 });
		const id = await Menu.request<number>(Menu.domain, 'create', { items: spec });
		const menu = new Menu(id, handlers);
		Menu.registry.set(id, menu);
		return menu;
	}

	/** 在鼠标处弹出；给了坐标就在指定处弹（**屏幕坐标**） */
	popup(pos?: { x: number; y: number }): Promise<void> {
		return this.call('popup', pos ? { id: this.id, x: pos.x, y: pos.y } : { id: this.id });
	}

	/** 销毁菜单。忘了调，C++ 里的 HMENU 就一直占着 */
	destroy(): Promise<void> {
		Menu.registry.delete(this.id);
		return this.call('destroy', { id: this.id });
	}

	/** @internal 通道收到点击消息时转给对应菜单项的 onclick */
	static dispatch(msg: unknown): void {
		const click = msg as { menu: number; item: string; checked?: boolean };
		const menu = Menu.registry.get(click.menu);
		const handler = menu?.handlers.get(click.item);
		if (!handler) return; // 菜单已销毁，或这一项没挂 onclick
		handler(click.checked === undefined ? {} : { checked: click.checked });
	}

	/**
	 * 校验 + 补 id，同时把 onclick 收进 handlers（函数过不了通道，只能留在 JS 侧）。
	 * submenu 递归处理。
	 */
	private static toSpec(
		items: MenuItem[],
		handlers: Map<string, (info: MenuClickInfo) => void>,
		state: { used: Set<string>; seq: number }
	): Record<string, unknown>[] {
		return items.map((item) => {
			const type = item.type ?? 'normal';
			if (type !== 'normal' && type !== 'radio' && type !== 'checkbox' && type !== 'separator') {
				throw new Error(`未知的菜单项类型：${type}`);
			}
			if (type === 'separator') return { type };

			// 子菜单：它本身不可点击，所以不分配 id、也不接受 onclick
			if (item.submenu) {
				return { type: 'normal', label: item.label ?? '', submenu: Menu.toSpec(item.submenu, handlers, state) };
			}

			let id = item.id;
			if (!id) {
				// 生成的 id 也可能跟开发者手写的撞上，撞了就往后找一个
				do { id = `item-${++state.seq}`; } while (state.used.has(id));
			}
			if (state.used.has(id)) throw new Error(`菜单项 id 重复：${id}`);
			state.used.add(id);
			if (item.onclick) handlers.set(id, item.onclick);

			return { type, label: item.label ?? '', id, checked: !!item.checked, disabled: !!item.disabled };
		});
	}
}

/**
 * 菜单项点击是**通道层面**要认领的一种下行消息：
 * 它既不是框架事件（没有 name，也不按窗口分发），
 * 也不对应某次 API 请求（没有 id），只有本域认得 { menu, item } 这组字段。
 *
 * 所以在这里登记钩子自己领走 —— 通道那边不需要知道菜单的存在，
 * 以后加别的域也照这个来，都不用回去改 Ipc
 */
addMessageHook((msg) =>
{
	if (typeof msg.menu !== 'number') return false;
	Menu.dispatch(msg);
	return true;
});
