/**
 * WebWin JS SDK —— 类型声明（自动生成，请勿手改）
 *
 * 生成方式：在 src/web 下执行 `npm run types`
 * 它由 tsc 输出的多个 .d.ts 合并而来，目的是让开发者只引一个文件。
 *
 * 用法（任选其一）：
 *   1. 在本文件旁 `/// <reference path="./WebWin.d.ts" />`
 *   2. 把本文件加进你工程的 tsconfig 的 include / files
 *
 * 框架注入到页面里的全局对象：
 *   win（当前窗口）· Win（窗口类型）· os · app · display · dialog · Menu
 */
// ---- Ipc ----
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
interface WebWinConfig {
    allowRemote: boolean;
    origin: string;
    /** 当前窗口的 id，由 C++ 注入；SDK 据此构造全局 win 对象 */
    winId?: number;
}
type Payload = unknown;
type Handler<T = unknown> = (payload: T) => void;
/**
 * C++ 侧下行的 error 字段。
 * 目前是字符串（错误码表尚未定义），将来会改成 { code, message }，故两种都兼容。
 */
type WebWinError = string | {
    code?: string;
    message?: string;
};
/**
 * 走共享内存返回的大块数据（见 arch/07-web.md 第八节）。
 *
 * 具体能力会在 `buffer` 之外再带上自己的字段（比如截图的 width / height）。
 * `buffer` 是**只读**的：往里写会让渲染进程崩，所以用完调 `release()` 还回去。
 */
interface SharedBufferData {
    /** 原始字节（PNG、文件内容 …） */
    buffer: ArrayBuffer;
    /** 用完必须调用：把共享内存还给 C++，不调会一直占着 */
    release(): void;
    [key: string]: unknown;
}
/**
 * 下行消息的通用形状。通道自己只认响应和事件两种，
 * 其余字段交给各能力域解释（比如菜单点击的 menu / item）
 */
type DownstreamMessage = {
    /** API 响应：与发起的那次调用对应 */
    id?: number;
    ok?: boolean;
    data?: unknown;
    error?: WebWinError;
    /** 框架事件：无 id，靠 name 区分 */
    name?: string;
    payload?: unknown;
    /** 消息所属窗口；null = 与窗口无关 */
    winId?: number;
    /**
     * 其余字段留给各能力域自己解释 —— 通道不认识它们，只原样往下传。
     * 菜单点击就靠 `menu` / `item` 这两个字段认领（见 Menu.ts 末尾的钩子）
     */
    [key: string]: unknown;
};
/**
 * 认领钩子：返回 true 表示"这条我处理了"，接收循环随即停止，
 * 不再走响应 / 事件的常规处理
 */
type MessageHook = (msg: DownstreamMessage) => boolean;
/**
 * @internal 能力域登记"某种下行消息我来认领"。
 *
 * 方向是**让域来登记**、而不是通道去 import 各域：
 * 域都 `extends Ipc`，通道反过来依赖它们会成环（且运行时可能踩到 TDZ）。
 * 好处是通道不认识任何具体的域，加新域只动它自己那个文件
 */
declare function addMessageHook(hook: MessageHook): void;
/**
 * 建立通道并注册接收。成功返回注入的配置，失败返回 null（此时不应安装 SDK）。
 * @internal
 */
declare function installChannel(): WebWinConfig | null;
declare class Ipc {
    /**
     * 能力域标识（`win` / `dialog` / `clipboard` ...），子类覆盖。
     * C++ 侧按它把消息路由到对应的域处理器；事件订阅也靠它带上域信息。
     */
    protected type: string;
    /** 关联的窗口 id；null 表示与窗口无关（clipboard / dialog 这类单例能力） */
    protected winId: number | null;
    /**
     * 调用 C++ 暴露的能力：type 指能力域，method 指该域下的具体动作。
     * additionalObjects 用于携带 File 等无法 JSON 序列化的对象
     */
    protected invoke<T = unknown>(type: string, method: string, params?: Payload, additionalObjects?: unknown[]): Promise<T>;
    /**
     * 调本域的方法：type 取 `this.type`，不必在每个方法里重复写域名。
     * 能力域里绝大多数调用都走这里；`invoke` 留给需要显式指定域的场合。
     */
    protected call<T = unknown>(method: string, params?: Payload, additionalObjects?: unknown[]): Promise<T>;
    /** 订阅事件；返回取消订阅的函数 */
    on<T = unknown>(name: string, handler: Handler<T>): () => void;
    /** 取消订阅 */
    off<T = unknown>(name: string, handler: Handler<T>): void;
    /** 供子类在还没有实例时发请求（如 Win.create 要先拿到窗口 id） */
    protected static request<T = unknown>(type: string, method: string, params?: Payload, winId?: number | null, additionalObjects?: unknown[]): Promise<T>;
}

// ---- App ----
/**
 * 应用级能力（app 域）。
 *
 * app 与窗口无关，所以它是**全局单例** `window.app` ——
 * 不像 `win` 那样一个窗口一个实例。这些信息都来自 package.json
 * 的 `webwin` 段（`id` / `name` / `version`）与运行时路径。
 */
declare class App extends Ipc {
    private static readonly domain;
    protected type: string;
    /**
     * 立即退出进程。
     *
     * 所有窗口直接关掉，不询问用户、不发退出前的事件。
     * 退出码缺省为 0。
     *
     * 注意：进程立刻就结束了，这个 Promise **不会** resolve ——
     * 要留记录就在调用之前写。
     */
    exit(exitCode?: number): Promise<void>;
    /** package.json 所在目录（命令行 `--path=...` 指定的那个，否则 exe 同目录） */
    getAppPath(): Promise<string>;
    /** exe 所在目录 */
    getExePath(): Promise<string>;
    /** 应用数据目录：`%APPDATA%\WebWin\{应用 id}` */
    getDataPath(): Promise<string>;
    /** 应用版本号，形如 `1.0.0` */
    getVersion(): Promise<string>;
    /** 应用名称 */
    getName(): Promise<string>;
    /** 应用 ID（任务栏分组、数据目录都按它区分） */
    getID(): Promise<string>;
}

// ---- Menu ----
/** 菜单项类型；不写就是 normal */
type MenuItemType = 'normal' | 'radio' | 'checkbox' | 'separator';
interface MenuItem {
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
interface MenuClickInfo {
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
declare class Menu extends Ipc {
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

// ---- Os ----
/** 系统版本 */
interface OsVersion {
    major: number;
    minor: number;
    build: number;
    /** 只有 Windows 10 / 11 会给（"win10" / "win11"）；更早的系统不猜，调用方自己看 build */
    versionName?: string;
}
/** 系统配色，形如 #RRGGBB */
interface OsColors {
    /** 窗口背景 */
    window: string;
    /** 按钮表面 */
    button: string;
    /** 窗口文字 */
    text: string;
    /** 标题栏文字 */
    caption: string;
    /** 活动边框 */
    border: string;
    /** 桌面背景 */
    desktop: string;
}
/** 一块网卡 */
interface NetAdapter {
    name: string;
    mac: string;
    /** IPv4 / IPv6 地址，不含 127.0.0.1 与 ::1 */
    addresses: string[];
}
/** osLock 事件的载荷 */
interface OsLockPayload {
    locked: boolean;
}
/** `os.getPath` 支持的目录名 */
type OsPathName = 'home' | 'appData' | 'temp' | 'desktop' | 'documents' | 'downloads' | 'music' | 'pictures' | 'videos' | 'recent';
/**
 * 系统级能力（os 域）。
 *
 * os 与窗口无关，所以它是**全局单例** `window.os` ——
 * 不像 `win` 那样一个窗口一个实例，事件也是全局订阅（win 域的事件才绑在某个窗口上）。
 */
declare class Os extends Ipc {
    private static readonly domain;
    protected type: string;
    getOsVer(): Promise<OsVersion>;
    getCpuId(): Promise<string>;
    getDiskId(): Promise<string>;
    getUserLang(): Promise<string>;
    getOsLang(): Promise<string>;
    getOsColor(): Promise<OsColors>;
    getIpAddr(): Promise<NetAdapter[]>;
    /** 取系统常用目录的绝对路径（不带尾部分隔符） */
    getPath(name: OsPathName): Promise<string>;
    /** 阻止系统休眠（直到调用 stopPreventSleep 或进程退出） */
    preventSleep(): Promise<void>;
    stopPreventSleep(): Promise<void>;
}

// ---- Win ----
/** 新建窗口的配置：复用 package.json 里 window 那套配置结构 */
interface WinOptions {
    [key: string]: unknown;
}
/**
 * 一个窗口。
 *
 * - 页面里的全局对象 `win` 就是**当前窗口**的实例
 * - 新窗口只能用 `Win.create()` 创建（不支持 `new Win(...)`）
 * - A 页面创建的 B 窗口，B 的事件会分发到 A 持有的这个对象上
 */
declare class Win extends Ipc {
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

// ---- Display ----
/** 矩形。坐标与尺寸都是**物理像素**（不是 CSS 像素） */
interface Rect {
    x: number;
    y: number;
    width: number;
    height: number;
}
/** 一块显示器 */
interface DisplayInfo {
    /** 设备名，形如 `\\.\DISPLAY1`；可以当标识用 */
    id: string;
    /** 是否主显示器 */
    primary: boolean;
    /** 缩放百分比：100 / 125 / 150 ... */
    scaleFactor: number;
    /** 屏幕矩形（物理像素） */
    bounds: Rect;
    /** 工作区矩形：屏幕矩形扣掉任务栏等 */
    workArea: Rect;
}
/** `display.getImage` 的返回值：PNG 字节走共享内存，不进 JSON */
interface CapturedImage extends SharedBufferData {
    /** 截图宽度（物理像素，等于传入的 width） */
    width: number;
    /** 截图高度 */
    height: number;
}
/**
 * 显示设备能力（display 域）。
 *
 * display 与窗口无关，所以它是**全局单例** `window.display` ——
 * 不像 `win` 那样一个窗口一个实例，事件也是全局订阅。
 *
 * 所有坐标 / 尺寸都是**物理像素**：150% 缩放的 4K 屏，bounds.width 是 3840。
 */
declare class Display extends Ipc {
    private static readonly domain;
    protected type: string;
    /** 所有显示器 */
    getAll(): Promise<DisplayInfo[]>;
    /** 主显示器 */
    getPrimary(): Promise<DisplayInfo>;
    /** 包含该点的显示器；点落在多屏之间的空隙时给最近的一块 */
    getByPos(x: number, y: number): Promise<DisplayInfo>;
    /** 与该矩形重叠面积最大的显示器；一块都不重叠时给中心点最近的那块 */
    getByRect(x: number, y: number, width: number, height: number): Promise<DisplayInfo>;
    /** 截屏幕的一块区域，返回 PNG 字节。用完记得 release() */
    getImage(x: number, y: number, width: number, height: number): Promise<CapturedImage>;
    /**
     * 取屏幕上某一点的颜色（`#RRGGBB`）。
     * 不传坐标就取**鼠标当前位置**的颜色 —— 做取色器时不用自己跟踪鼠标
     */
    getColor(x?: number, y?: number): Promise<string>;
}

// ---- Dialog ----
/** 一个文件过滤器：`[显示名, 通配符]`，如 `["图片", "*.png;*.jpg"]` */
type FileFilter = [string, string];
/** `dialog.openPath` 的参数 */
interface OpenPathConfig {
    /** 对话框标题 */
    title?: string;
    /** 确认按钮文字，如“选择文件” */
    okBtnText?: string;
    /** 初始目录；也可以给完整文件路径，会自动拆出目录部分 */
    defaultPath?: string;
    /** 'file'（默认）选文件；'dir' 选文件夹 */
    type?: 'file' | 'dir';
    /** 文件类型过滤器，仅 type='file' 时有效；不传即“所有文件” */
    filter?: FileFilter[];
    /** 是否允许多选，默认 false */
    multiSelection?: boolean;
    /** 是否显示隐藏文件，默认 false */
    showHiddenFile?: boolean;
}
/** `dialog.savePath` 的参数 */
interface SavePathConfig {
    title?: string;
    okBtnText?: string;
    /** 初始目录；给完整文件路径时会拆出目录和默认文件名 */
    defaultPath?: string;
    /** 文件类型过滤器；第一个过滤器的后缀会被当作默认扩展名自动补上 */
    filter?: FileFilter[];
    showHiddenFile?: boolean;
}
/** `dialog.msgBox` 的参数 */
interface MsgBoxConfig {
    title?: string;
    msg?: string;
    /** 图标，默认 'info' */
    icon?: 'info' | 'warn' | 'err' | 'question' | 'stop';
    /** 按钮组合，默认 'ok' */
    btn?: 'ok' | 'okCancel' | 'yesNo' | 'yesNoCancel' | 'retryCancel' | 'abortRetryIgnore';
}
/**
 * `dialog.openPath` / `savePath` 的返回值。
 *
 * 用户点“取消”是**正常结果不是错误**：不会 reject，而是 `cancel: true`。
 * 取消时 `data` 给空值（选文件是空数组，存文件是空串），调用方不用判 undefined。
 */
interface PathResult<T> {
    cancel: boolean;
    data: T;
}
/** `dialog.msgBox` 的返回值：用户点了哪个按钮 */
type MsgBoxResult = 'ok' | 'cancel' | 'yes' | 'no' | 'abort' | 'retry' | 'ignore';
/**
 * 系统对话框能力（dialog 域）。
 *
 * `window.dialog` 是全局单例，但**绑在当前窗口上**：安装时把当前窗口 id 存进来，
 * 之后每次调用都会把它带给 C++，C++ 据此把对话框模态到那个窗口
 * （禁用属主窗口、层级正确、任务栏不闪）。
 *
 * 所以它的语义是“当前窗口的对话框”，而不是“随便哪个窗口的对话框”。
 * 要给别的窗口弹，得拿到那个窗口的 Win 实例（TODO：win.dialog）。
 */
declare class Dialog extends Ipc {
    private static readonly domain;
    protected type: string;
    /** @param ownerWinId 属主窗口 id；null 表示不与任何窗口关联（此时对话框无属主） */
    constructor(ownerWinId?: number | null);
    /** 选择文件 / 文件夹。取消时返回 `{ cancel: true, data: [] }` */
    openPath(config?: OpenPathConfig): Promise<PathResult<string[]>>;
    /** 选择保存位置。取消时返回 `{ cancel: true, data: '' }` */
    savePath(config?: SavePathConfig): Promise<PathResult<string>>;
    /** 弹一个系统消息框，返回用户点了哪个按钮 */
    msgBox(config?: MsgBoxConfig): Promise<MsgBoxResult>;
    /** 打开资源管理器并选中指定文件 */
    itemInFolder(path: string): Promise<void>;
}

// ---- WebWin ----
/**
 * WebWin JS SDK 入口（打包成 dist/webwin.js，内嵌进 exe）
 *
 * 安装成功后，页面里可以直接使用：
 *   win            —— 当前窗口对象
 *   Win            —— 窗口类型，Win.create() 创建新窗口
 *   os / app / display / dialog —— 与窗口无关的全局单例能力
 *   （dialog 虽是单例，但绑在当前窗口上：弹出来的框模态到当前窗口）
 */







    interface Window {
        /** 当前窗口 */
        win: Win;
        /** 窗口类型 */
        Win: typeof Win;
        /** 系统级能力（全局单例，与窗口无关） */
        os: Os;
        /** 应用级能力（全局单例，与窗口无关） */
        app: App;
        /** 显示设备能力（全局单例，与窗口无关） */
        display: Display;
        /** 系统对话框能力（全局单例，但绑在当前窗口上） */
        dialog: Dialog;
        /** 菜单类型 */
        Menu: typeof Menu;
    }
