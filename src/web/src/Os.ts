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

/** osLock 事件的载荷 */
export interface OsLockPayload {
	locked: boolean;
}

/** `os.getLang` 的作用域：`'user'` = 当前用户，`'os'` = 系统 */
export type LangScope = 'user' | 'os';

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

	getVersion(): Promise<OsVersion> { return this.call('getVersion'); }
	getCpuId(): Promise<string> { return this.call('getCpuId'); }
	getDiskId(): Promise<string> { return this.call('getDiskId'); }

	/**
	 * 取语言（区域名，如 `zh-CN`）。
	 *
	 * `scope`：`'user'` = **当前用户**的语言，`'os'` = **系统**的语言 ——
	 * 两者在多用户机器上可能不同，所以这个参数不给默认值
	 */
	getLang(scope: LangScope): Promise<string> { return this.call('getLang', { scope }); }

	getColor(): Promise<OsColors> { return this.call('getColor'); }

	/** 取系统常用目录的绝对路径（不带尾部分隔符） */
	getPath(name: OsPathName): Promise<string> { return this.call('getPath', { name }); }

	/** 阻止系统休眠（直到调用 stopPreventSleep 或进程退出） */
	preventSleep(): Promise<void> { return this.call('preventSleep'); }
	stopPreventSleep(): Promise<void> { return this.call('stopPreventSleep'); }

	// ---------- 系统凭据管理器 ----------

	/**
	 * 写一条凭据。key 已存在则覆盖。
	 *
	 * 存在**本机**这一栏（CRED_PERSIST_LOCAL_MACHINE）：不随用户配置漫游到域里的
	 * 其它机器，只对当前用户可见。值按 UTF-16 原样存，任何 JS 字符串都能原样往返
	 */
	credWrite(key: string, value: string): Promise<void> {
		return this.call('credWrite', { key, value });
	}

	/** 读一条凭据。**没存过返回 `null`**（不 reject），系统级故障才 reject */
	credRead(key: string): Promise<string | null> {
		return this.call('credRead', { key });
	}

	/** 删一条凭据。本来就没有也不报错 —— 要的是"删完之后不存在"（幂等） */
	credDelete(key: string): Promise<void> {
		return this.call('credDelete', { key });
	}

	/**
	 * 列出已存的凭据 key（不含值）。
	 *
	 * `prefix` 是前缀匹配；不给就列全部 —— 凭据管理器是**系统级共享空间**，
	 * git、远程桌面这些也在里面存东西，它们会被一起列出来（只列 key，不含值）
	 */
	credList(prefix?: string): Promise<string[]> {
		return this.call('credList', { prefix });
	}
}
