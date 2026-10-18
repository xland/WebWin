/**
 * WebWin JS SDK —— 类型声明（自动生成，请勿手改）
 *
 * 生成方式：在 src/web 下执行 `npm run types`
 * 它由 tsc 输出的多个 .d.ts 合并而来，目的是让开发者只引一个文件。
 *
 * 用法（任选其一）：
 *   1. 在本文件旁 `/// <reference path="./webwin.d.ts" />`
 *   2. 把本文件加进你工程的 tsconfig 的 include / files
 *
 * 框架注入到页面里的全局对象：
 *   win（当前窗口）· Win（窗口类型）· os · app · display · dialog
 * 需要自己建实例的类型：Menu · Db
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
    /**
     * 应用基准目录（package.json 所在目录），由 C++ 注入。
     * `path.resolve()` / `path.relative()` 拿它当基准 —— 框架里没有 cwd 概念
     * （全库不调 GetCurrentDirectory），这里取代 Node 的 process.cwd()
     */
    appPath?: string;
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
    /**
     * C++ 分配的菜单 id。
     *
     * @internal 不是给开发者用的。之所以还得暴露，是因为 `Win.setContextMenu`
     * 要把它交给 C++（同一包内的类之间要传）。想彻底私有就得让 Menu 反过来替
     * Win 组装整个上行消息，为这点好处引入双向依赖不划算 —— 沿用本项目
     * `@internal` 的约定（同 Menu.dispatch）即可。
     */
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

// ---- Path ----
/**
 * 路径字符串工具（`window.path`）。
 *
 * **这是 SDK 里唯一不走 C++ 的能力**：全是纯字符串运算，一次 IPC 都不发，
 * 所以方法全是**同步**的，直接返回值而不是 Promise。其余能力域都要过消息通道、
 * 都是 async，别把这里的写法套过去。
 *
 * 语义对齐 Node.js 的 `path.win32`（本框架只支持 Windows，所以没有 `path.posix`、
 * 也没有 `path.win32` 这层自引用）。输入**同时接受 `/` 和 `\`**，输出统一 `\`。
 *
 * ```ts
 * path.join(base, 'data', 'app.db');  // '<基准目录>\data\app.db'
 * path.dirname('C:\\a\\b.txt');       // 'C:\\a'
 * path.extname('C:\\a\\b.txt');       // '.txt'
 * path.parse('C:\\a\\b.txt').name;    // 'b'
 * ```
 *
 * 与 Node 的两处不同，都是有意为之：
 * - `resolve()` / `relative()` 的基准是**应用基准目录**（package.json 所在目录，
 *   启动时由 C++ 注入），不是 `process.cwd()`。桌面应用的 cwd 取决于启动方式
 *   （双击 / 快捷方式 / 命令行各不相同），拿它当基准结果不可预测。
 * - 多了一组 Node 没有的 Windows 专属校验：`isValidName()` / `isValidPath()`。
 *
 * 实现细节：`normalize()` / `join()` 里那些看着绕的分支不是冗余 ——
 * 前半段处理 `\\.\` / `\\?\` 设备根和 `NUL:` 这类保留设备名，
 * 末尾那段 `.\\` 前缀是修 CVE-2024-36139 的（防止相对路径被系统误判成绝对路径）。
 * 别为了"简化"删掉它们，改之前先跑一遍差分测试。
 */
/** `path.parse()` 的返回 */
interface ParsedPath {
    /** 根：`'C:\\'` / `'\\\\server\\share\\'` / `''` */
    root: string;
    /** 所在目录；没有目录时等于 root */
    dir: string;
    /** 最后一段（含扩展名） */
    base: string;
    /** 扩展名（含前导点）；没有则 `''` */
    ext: string;
    /** 最后一段去掉扩展名 */
    name: string;
}
/** `path.format()` 的参数。给了 `base` 时忽略 `name` / `ext` */
interface PathObject {
    root?: string;
    dir?: string;
    base?: string;
    ext?: string;
    name?: string;
}
declare class Path {
    /** 路径分隔符 */
    readonly sep = "\\";
    /** PATH 那种环境变量里分隔多个路径用的符号 */
    readonly delimiter = ";";
    /**
     * `resolve()` / `relative()` 的基准目录，启动时由 C++ 注入。
     * @internal 开发者不要自己 new，用 `window.path`
     */
    private readonly base;
    constructor(base?: string);
    /** `resolve()` / `relative()` 用的基准目录 */
    getBase(): string;
    /**
     * 拼接后规范化：多余分隔符、`.`、`..` 都会消掉。
     * 参数里有绝对路径时**不会**重置（这点和 `resolve` 不同）。
     *
     * 任一段不是字符串就抛 TypeError。
     */
    join(...args: string[]): string;
    /**
     * 从右往左解析：遇到第一个绝对路径就停，剩下的拼在它后面。
     * 全是相对路径时以应用基准目录（`getBase()`）为底。
     *
     * ```ts
     * path.resolve('data', 'a.db');  // '<基准目录>\data\a.db'
     * path.resolve('C:\\x', 'y');    // 'C:\\x\\y'
     * ```
     */
    resolve(...args: string[]): string;
    /** 规范化：消掉 `.` / `..` / 重复分隔符，斜杠统一成反斜杠 */
    normalize(path: string): string;
    /** 最后一段（含扩展名）。给了 `suffix` 且正好对上，就从尾部去掉 */
    basename(path: string, suffix?: string): string;
    /** 所在目录 */
    dirname(path: string): string;
    /** 扩展名（含前导点）；没有、或最后一段以点开头时返回 `''` */
    extname(path: string): string;
    /** 拆成 `{ root, dir, base, ext, name }` 五段 */
    parse(path: string): ParsedPath;
    /** `parse()` 的逆运算。给了 `base` 时忽略 `name` / `ext` */
    format(pathObject: PathObject): string;
    /** 是否绝对路径（盘符加分隔符、UNC、或 `\foo` 这种根相对）。`C:foo` 不算 */
    isAbsolute(path: string): boolean;
    /**
     * `from` 到 `to` 的相对路径。两边先按 `resolve()` 展开，
     * 所以给相对路径时以应用基准目录为底。不同盘符算不出来，返回 `to`。
     */
    relative(from: string, to: string): string;
    /**
     * 单个文件/目录名在 Windows 上是否可用。查三件事，都是纯字符串能查的：
     * 1. 非法字符：`<>:"|?*`、两个分隔符、以及 0-31 控制字符
     * 2. 保留设备名：`CON` / `PRN` / `AUX` / `NUL` / `COM1-9` / `LPT1-9`
     *    （带扩展名也算，`nul.txt` 同样指向设备；大小写不敏感）
     * 3. 以点或空格结尾（系统会自动把尾巴上的点和空格去掉，存出来跟你要的不一样）
     *
     * 传的是**单段名字**，不要带分隔符。
     */
    isValidName(name: string): boolean;
    /**
     * 整条路径在 Windows 上是否可用：按分隔符切段，逐段用 `isValidName()` 查。
     * 盘符（`C:`）、UNC 根、`\\?\` 前缀、以及 `.` / `..` 都不参与名字校验。
     */
    isValidPath(path: string): boolean;
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

// ---- Fs ----
/**
 * `readFile` 的编码。
 *
 * - `utf8`（默认）：当文本读。**非 UTF-8 的内容会出乱码**，要原样字节用下面两个
 * - `base64`：二进制内容无损，代价是字符串体积涨 1/3
 * - `binary`：字节走共享内存，返回 ArrayBuffer。零拷贝、无膨胀，
 *   但**用完必须 `release()`**，且 buffer 是只读的
 */
type FsEncoding = 'utf8' | 'base64' | 'binary';
interface FsReadOptions {
    /** 默认 `utf8` */
    encoding?: FsEncoding;
}
interface FsWriteOptions {
    /** `base64` 时把 data 当 base64 解成字节再写；默认 `utf8` */
    encoding?: 'utf8' | 'base64';
}
/** `readFile` 用 `binary` 时的返回：字节走共享内存，不进 JSON */
interface FsBinary extends SharedBufferData {
    /** 字节数 */
    size: number;
}
/**
 * `stat()` 的返回。
 *
 * 是**纯数据**，不像 Node 的 `Stats` 那样带 `isFile()` 方法 —— 那些方法过不了
 * 消息通道，所以一律给布尔字段。
 */
interface FsStats {
    /** 字节数 */
    size: number;
    isFile: boolean;
    isDirectory: boolean;
    isSymbolicLink: boolean;
    /** 只读属性（Windows 上才有意义） */
    readonly: boolean;
    hidden: boolean;
    /** 最后修改时间，毫秒 */
    mtimeMs: number;
    /** 最后访问时间，毫秒 */
    atimeMs: number;
    /** 创建时间，毫秒 */
    birthtimeMs: number;
    /**
     * 权限位。**近似值**：Windows 没有 POSIX 权限位，
     * 只读给 `0o444`、否则给 `0o666`，不代表真实 ACL
     */
    mode: number;
}
/**
 * 文件读写（fs 域）。
 *
 * **全部异步**，没有同步版本 —— 文件操作看着快，实际可能很慢：网络盘（UNC）、
 * 被杀毒软件盯上的写入、几百 MB 的读取都能卡住几秒。而且操作跑在**后台线程**上，
 * 不会把窗口冻住。
 *
 * **只管文件**：目录的创建 / 遍历 / 删除另归一处（见 arch/01-api.md 第 12 项），
 * 所以这里没有 mkdir / readdir / rmdir。
 *
 * **相对路径以 package.json 所在目录为底**，跟 `path.resolve()` 一致。
 * 不能用"进程当前目录"当基准 —— GUI 进程本来就没有有意义的 cwd，
 * 而且 Windows 的文件对话框还会悄悄改掉它。
 */
declare class Fs extends Ipc {
    private static readonly domain;
    protected type: string;
    /** `access()` 用的位掩码，跟 Node 的 `fs.constants` 一致 */
    readonly constants: {
        readonly F_OK: 0;
        readonly X_OK: 1;
        readonly W_OK: 2;
        readonly R_OK: 4;
    };
    /**
     * 读整个文件。
     *
     * ```ts
     * const text = await fs.readFile('config.json');          // 当文本读
     * const b64  = await fs.readFile('logo.png', 'base64');   // 二进制无损
     * const bin  = await fs.readFile('big.zip', 'binary');    // ArrayBuffer
     * bin.release();                                          // 用完必须还
     * ```
     */
    readFile(path: string, options?: FsReadOptions | 'utf8' | 'base64'): Promise<string>;
    readFile(path: string, options: 'binary' | {
        encoding: 'binary';
    }): Promise<FsBinary>;
    /**
     * 写文件（覆盖）。文件不存在就创建，**但不会自动建父目录**
     * （Node 的 writeFile 也不建，静默建目录会把拼错的路径变成一堆垃圾目录）
     */
    writeFile(path: string, data: string, options?: FsWriteOptions): Promise<void>;
    /** 追加到文件末尾；文件不存在就创建 */
    appendFile(path: string, data: string, options?: FsWriteOptions): Promise<void>;
    /** 删文件。文件不存在会 reject */
    unlink(path: string): Promise<void>;
    /** 复制文件；目标已存在时覆盖 */
    copyFile(src: string, dest: string): Promise<void>;
    /** 移动 / 改名；目标已存在时覆盖。跨盘也行 */
    rename(oldPath: string, newPath: string): Promise<void>;
    /** 取文件信息。不存在会 reject */
    stat(path: string): Promise<FsStats>;
    /**
     * 检查能不能访问。默认只查存在（`F_OK`）。
     *
     * ```ts
     * await fs.access(p, fs.constants.R_OK | fs.constants.W_OK);
     * ```
     *
     * 权限位是**近似**的：Windows 没有 POSIX 权限，`W_OK` 只看只读属性，不查 ACL。
     */
    access(path: string, mode?: number): Promise<void>;
}

// ---- Http ----
/**
 * 响应体怎么给。**跟 `fs.readFile` 的 encoding 是同一个概念、同一套取值**。
 *
 * - `utf8`（默认）：当文本解。二进制内容会被**破坏**，非 UTF-8 的文本会出乱码
 * - `base64`：二进制无损，代价是体积涨 1/3
 * - `binary`：字节走共享内存，返回 `HttpBinaryResponse`（带 ArrayBuffer）
 */
type HttpEncoding = 'utf8' | 'base64' | 'binary';
/** `Http.fetch()` 的可选项 */
interface HttpFetchOptions {
    /** 请求方法，默认 GET。大小写不敏感 */
    method?: string;
    /** 请求头。key 大小写不敏感 —— 取回来时统一成小写 */
    headers?: Record<string, string>;
    /**
     * 请求体。要发 JSON 就自己 `JSON.stringify`，
     * 并带上 `content-type: application/json`（原生层不会替你猜）
     */
    body?: string;
    /**
     * 响应体怎么给，默认 `utf8`。取值见 `HttpEncoding`。
     *
     * 这跟响应头里的字符集（`content-type: text/html; charset=gbk`）**不是一回事**：
     * 这里说的是"字节怎么过消息通道交给你"，原生层不会替你按字符集解码
     */
    encoding?: HttpEncoding;
}
/** C++ 回传的原始数据（文本模式） */
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
declare class HttpResponse {
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
    private readonly body;
    /** @internal 由 `Http.fetch()` 构造，开发者不用自己 new */
    constructor(data: FetchData);
    /**
     * 响应体原文。
     *
     * 这是**按 UTF-8 解出来的文本**（默认 encoding）。拿二进制会被破坏 ——
     * 那种情况要改用 `encoding: 'base64'` 或 `'binary'`
     */
    text(): Promise<string>;
    /** 按 JSON 解析响应体；不是合法 JSON 时抛 SyntaxError */
    json<T = unknown>(): Promise<T>;
}
/**
 * `encoding: 'binary'` 时的返回：响应体字节走共享内存，不进 JSON。
 *
 * 状态行和响应头**照常是普通字段**（它们小，没必要走共享内存），
 * 只有 `buffer` 是共享内存里的字节。用完必须 `release()`。
 */
interface HttpBinaryResponse extends SharedBufferData {
    /** 2xx / 3xx 为 true。4xx / 5xx **不会**让 fetch 失败，要看这个字段 */
    ok: boolean;
    /** HTTP 状态码 */
    status: number;
    /** 状态文本，如 `'OK'` */
    statusText: string;
    /** 响应头，key 统一小写（跟 web 的 `Headers` 一个规矩） */
    headers: Record<string, string>;
    /** 响应体字节数 */
    size: number;
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
 *
 * 要拿**二进制**（图片、zip、protobuf）必须指定 `encoding`，否则会被当文本解而破坏：
 *
 * ```ts
 * const png = await Http.fetch(url, { encoding: 'binary' });
 * png.buffer;      // ArrayBuffer，走共享内存
 * png.release();   // 用完必须还
 * ```
 */
declare class Http extends Ipc {
    private static readonly domain;
    protected type: string;
    /** 不给实例化：本域只有静态方法 */
    private constructor();
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
    static fetch(url: string, options?: HttpFetchOptions & {
        encoding?: 'utf8' | 'base64';
    }): Promise<HttpResponse>;
    static fetch(url: string, options: HttpFetchOptions & {
        encoding: 'binary';
    }): Promise<HttpBinaryResponse>;
}
{};

// ---- Db ----
/**
 * 绑定参数。两种都收：
 * - 数组  → 按位置绑 `?`，第 1 个元素对应第 1 个问号
 * - 对象  → 按名字绑，SQL 里写 `:name` / `@name` / `$name` 都能对上
 *
 * 一律走绑定，**不要自己拼 SQL 字符串**（拼出来的既容易错，也是注入口子）
 */
type DbParams = unknown[] | Record<string, unknown>;
/** 一行结果。SQLite 的 5 种类型映射过来只剩 number / string / null 三种 */
type DbRow = Record<string, unknown>;
/** run() 的结果 */
interface DbRunResult {
    /** 受影响的行数 */
    changes: number;
    /** 最后一次插入的 rowid */
    lastInsertRowid: number;
}
/**
 * SQLite 数据库。**一个实例 = 一个连接。**
 *
 * 库文件默认落在应用数据目录的 `db/` 下（跟 WebView2 的浏览器数据分开），
 * 也可以给完整路径自己指定。
 *
 * ```ts
 * const db = await Db.open('app.db');
 * await db.exec('CREATE TABLE IF NOT EXISTS t (id INTEGER PRIMARY KEY, v TEXT)');
 * await db.run('INSERT INTO t (v) VALUES (?)', ['hello']);
 * const rows = await db.all('SELECT * FROM t WHERE id = :id', { id: 1 });
 * await db.close();
 * ```
 *
 * 两个已知取舍：
 * - SQLite 的 INTEGER 是 64 位的，转成 JS number 后超过 2^53 会丢精度
 * - BLOB 以 base64 字符串返回；日期没有原生类型，不做自动转换
 */
declare class Db extends Ipc {
    private static readonly domain;
    protected type: string;
    /** C++ 分配的连接 id；开发者不感知，所以是 private */
    private readonly id;
    private constructor();
    /**
     * 打开（文件不存在则创建）一个数据库，返回实例。
     *
     * **是 async**：连接是 C++ 侧的资源，得等它建好才拿得到 id。
     * 不传 name 就用默认库（应用数据目录下的 `db/webwin.db`）。
     */
    static open(name?: string): Promise<Db>;
    /** 关闭连接。忘了调，C++ 里的 sqlite3* 会一直占着文件锁 */
    close(): Promise<void>;
    /**
     * 执行一段不返回结果的 SQL，可以包含多条语句（建表、初始化用它）。
     *
     * **不支持参数绑定** —— 需要绑定就用 run / get / all
     */
    exec(sql: string): Promise<void>;
    /** 执行写操作（INSERT / UPDATE / DELETE），返回影响行数和新 rowid */
    run(sql: string, params?: DbParams): Promise<DbRunResult>;
    /** 查一行；没查到返回 null（不报错） */
    get(sql: string, params?: DbParams): Promise<DbRow | null>;
    /** 查全部行 */
    all(sql: string, params?: DbParams): Promise<DbRow[]>;
}

// ---- Tray ----
/** `Tray.create()` 的参数；两个都可以不传 */
interface TrayOptions {
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
declare class Tray extends Ipc {
    private static readonly domain;
    protected type: string;
    /** C++ 分配的托盘 id；开发者不感知，所以是 private */
    private readonly id;
    private constructor();
    /**
     * 建一个托盘图标。**是 async**：图标是 C++ 侧的 Win32 资源，得等它建好。
     * 一个进程可以建多个。
     */
    static create(options?: TrayOptions): Promise<Tray>;
    /** 改鼠标悬停时的提示文字 */
    setToolTip(text: string): Promise<void>;
    /** 移除图标。忘了调，托盘里会一直留着 */
    destroy(): Promise<void>;
}

// ---- WebWin ----
/**
 * WebWin JS SDK 入口（打包成 dist/webwin.js，内嵌进 exe）
 *
 * 安装成功后，页面里可以直接使用：
 *   win            —— 当前窗口对象
 *   Win            —— 窗口类型，Win.create() 创建新窗口
 *   os / app / display / dialog / path / fs —— 与窗口无关的全局单例能力
 *   （dialog 虽是单例，但绑在当前窗口上：弹出来的框模态到当前窗口）
 *   path 是纯 JS 的字符串工具，不经过 C++
 *   fs 只管文件，全部异步（跑在后台线程上），相对路径以应用基准目录为底
 *   Menu / Db / Tray —— 需要自己建实例的类型（Menu.create() / Db.open() / Tray.create()）
 *   Http —— 只有静态方法的类型（Http.fetch() 发请求，走 C++ 所以没有 CORS 限制）
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
        /** 路径字符串工具（纯 JS，同步；不经过 C++） */
        path: Path;
        /** 显示设备能力（全局单例，与窗口无关） */
        display: Display;
        /** 系统对话框能力（全局单例，但绑在当前窗口上） */
        dialog: Dialog;
        /** 文件读写（全局单例）。只管文件；全部异步，跑在后台线程上 */
        fs: Fs;
        /** 菜单类型 */
        Menu: typeof Menu;
        /** 数据库类型：Db.open() 建连接，实例上再跑 SQL */
        Db: typeof Db;
        /** 托盘类型：Tray.create() 建图标，实例上改提示、销毁、听事件 */
        Tray: typeof Tray;
        /** HTTP 请求类型：Http.fetch() 发请求（走 C++，不受 CORS 限制） */
        Http: typeof Http;
    }
