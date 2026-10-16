import { Ipc } from './Ipc';
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
export declare class Menu extends Ipc {
    private static readonly domain;
    /** 菜单 id → 实例：点击消息按 id 找回实例，再按 item id 找回回调 */
    private static readonly registry;
    protected type: string;
    /** C++ 分配的菜单 id */
    readonly id: number;
    private readonly handlers;
    private constructor();
    /**
     * 建菜单。**是 async**：菜单是 C++ 里的 Win32 对象，得等它建好才拿得到 id。
     */
    static create(items: MenuItem[]): Promise<Menu>;
    /** 在鼠标处弹出；给了坐标就在指定处弹（**屏幕坐标**） */
    popup(pos?: {
        x: number;
        y: number;
    }): Promise<void>;
    /** 销毁菜单。忘了调，C++ 里的 HMENU 就一直占着 */
    destroy(): Promise<void>;
    /** @internal 通道收到点击消息时转给对应菜单项的 onclick */
    static dispatch(msg: unknown): void;
    /**
     * 校验 + 补 id，同时把 onclick 收进 handlers（函数过不了通道，只能留在 JS 侧）。
     * submenu 递归处理。
     */
    private static toSpec;
}
