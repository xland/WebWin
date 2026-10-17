import { Ipc } from './Ipc';

/** 一个文件过滤器：`[显示名, 通配符]`，如 `["图片", "*.png;*.jpg"]` */
export type FileFilter = [string, string];

/** `dialog.openPath` 的参数 */
export interface OpenPathConfig {
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
export interface SavePathConfig {
	title?: string;
	okBtnText?: string;
	/** 初始目录；给完整文件路径时会拆出目录和默认文件名 */
	defaultPath?: string;
	/** 文件类型过滤器；第一个过滤器的后缀会被当作默认扩展名自动补上 */
	filter?: FileFilter[];
	showHiddenFile?: boolean;
}

/** `dialog.msgBox` 的参数 */
export interface MsgBoxConfig {
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
export interface PathResult<T> {
	cancel: boolean;
	data: T;
}

/** `dialog.msgBox` 的返回值：用户点了哪个按钮 */
export type MsgBoxResult = 'ok' | 'cancel' | 'yes' | 'no' | 'abort' | 'retry' | 'ignore';

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
export class Dialog extends Ipc {
	private static readonly domain = 'dialog'; // 对应 C++ 侧的 Dialog 域处理器

	protected type = Dialog.domain;

	/** @param ownerWinId 属主窗口 id；null 表示不与任何窗口关联（此时对话框无属主） */
	constructor(ownerWinId: number | null = null) {
		super();
		this.winId = ownerWinId;
	}

	/** 选择文件 / 文件夹。取消时返回 `{ cancel: true, data: [] }` */
	openPath(config: OpenPathConfig = {}): Promise<PathResult<string[]>> {
		return this.call('openPath', config);
	}

	/** 选择保存位置。取消时返回 `{ cancel: true, data: '' }` */
	savePath(config: SavePathConfig = {}): Promise<PathResult<string>> {
		return this.call('savePath', config);
	}

	/** 弹一个系统消息框，返回用户点了哪个按钮 */
	msgBox(config: MsgBoxConfig = {}): Promise<MsgBoxResult> {
		return this.call('msgBox', config);
	}

	/** 打开资源管理器并选中指定文件 */
	itemInFolder(path: string): Promise<void> {
		return this.call('itemInFolder', { path });
	}
}
