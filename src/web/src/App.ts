import { Ipc } from './Ipc';

/**
 * 应用级能力（app 域）。
 *
 * app 与窗口无关，所以它是**全局单例** `window.app` ——
 * 不像 `win` 那样一个窗口一个实例。这些信息都来自 package.json
 * 的 `webwin` 段（`id` / `name` / `version`）与运行时路径。
 */
export class App extends Ipc {
	private static readonly domain = 'app'; // 对应 C++ 侧的 App 域处理器

	protected type = App.domain;

	/**
	 * 立即退出进程。
	 *
	 * 所有窗口直接关掉，不询问用户、不发退出前的事件。
	 * 退出码缺省为 0。
	 *
	 * 注意：进程立刻就结束了，这个 Promise **不会** resolve ——
	 * 要留记录就在调用之前写。
	 */
	exit(exitCode = 0): Promise<void> { return this.call('exit', { exitCode }); }

	/** package.json 所在目录（命令行 `--path=...` 指定的那个，否则 exe 同目录） */
	getAppPath(): Promise<string> { return this.call('getAppPath'); }

	/** exe 所在目录 */
	getExePath(): Promise<string> { return this.call('getExePath'); }

	/** 应用数据目录：`%APPDATA%\WebWin\{应用 id}` */
	getDataPath(): Promise<string> { return this.call('getDataPath'); }

	/** 应用版本号，形如 `1.0.0` */
	getVersion(): Promise<string> { return this.call('getVersion'); }

	/** 应用名称 */
	getName(): Promise<string> { return this.call('getName'); }

	/** 应用 ID（任务栏分组、数据目录都按它区分） */
	getID(): Promise<string> { return this.call('getID'); }
}
