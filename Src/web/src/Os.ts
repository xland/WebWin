import { Ipc } from './Ipc';

/** 系统版本 */
export interface OsVersion {
	major: number;
	minor: number;
	build: number;
	/** 只有 Windows 10 / 11 会给（"win10" / "win11"）；更早的系统不猜，调用方自己看 build */
	versionName?: string;
}

/** 系统配色，形如 #RRGGBB */
export interface OsColors {
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
export interface NetAdapter {
	name: string;
	mac: string;
	/** IPv4 / IPv6 地址，不含 127.0.0.1 与 ::1 */
	addresses: string[];
}

/** osLock 事件的载荷 */
export interface OsLockPayload {
	locked: boolean;
}

/** `os.getPath` 支持的目录名 */
export type OsPathName =
	| 'home'      // 用户主目录（%USERPROFILE%）
	| 'appData'   // 每用户的应用数据目录（%APPDATA%）
	| 'temp'      // 临时目录
	| 'desktop'   // 桌面
	| 'documents' // 我的文档
	| 'downloads' // 下载
	| 'music'     // 音乐
	| 'pictures'  // 图片
	| 'videos'    // 视频
	| 'recent';   // 最近使用的文件（仅 Windows）

/**
 * 系统级能力（os 域）。
 *
 * os 与窗口无关，所以它是**全局单例** `window.os` ——
 * 不像 `win` 那样一个窗口一个实例，事件也是全局订阅（win 域的事件才绑在某个窗口上）。
 */
export class Os extends Ipc {
	private static readonly domain = 'os'; // 对应 C++ 侧的 Os 域处理器

	protected type = Os.domain;

	getOsVer(): Promise<OsVersion> { return this.call('getOsVer'); }
	getCpuId(): Promise<string> { return this.call('getCpuId'); }
	getDiskId(): Promise<string> { return this.call('getDiskId'); }
	getUserLang(): Promise<string> { return this.call('getUserLang'); }
	getOsLang(): Promise<string> { return this.call('getOsLang'); }
	getOsColor(): Promise<OsColors> { return this.call('getOsColor'); }
	getIpAddr(): Promise<NetAdapter[]> { return this.call('getIpAddr'); }

	/** 取系统常用目录的绝对路径（不带尾部分隔符） */
	getPath(name: OsPathName): Promise<string> { return this.call('getPath', { name }); }

	/** 阻止系统休眠（直到调用 stopPreventSleep 或进程退出） */
	preventSleep(): Promise<void> { return this.call('preventSleep'); }
	stopPreventSleep(): Promise<void> { return this.call('stopPreventSleep'); }
}
