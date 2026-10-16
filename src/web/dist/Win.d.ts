import { Ipc } from './Ipc';
import type { Menu } from './Menu';
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
export declare class Win extends Ipc {
    private static readonly domain;
    protected type: string;
    private constructor();
    /** 创建一个新窗口，resolve 出来的对象窗口 id 已就绪 */
    static create(options?: WinOptions): Promise<Win>;
    /** @internal 框架注入当前窗口对象用；遍历 / 查找窗口的公开 API 尚未定 */
    static fromId(id: number): Win;
    maximize(): Promise<void>;
    minimize(): Promise<void>;
    restore(): Promise<void>;
    hide(): Promise<void>;
    show(): Promise<void>;
    resize(width: number, height: number): Promise<void>;
    move(x: number, y: number): Promise<void>;
    close(): Promise<void>;
    destroy(): Promise<void>;
    /**
     * 挂右键菜单：在本窗口里点右键就弹它。
     * 传 null 解绑 —— 之后重新走浏览器默认的右键菜单
     */
    setContextMenu(menu: Menu | null): Promise<void>;
}
