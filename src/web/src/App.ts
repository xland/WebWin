import { Ipc } from './Ipc';

/**
 * 应用级能力（app 域）。
 *
 * app 与窗口无关，所以它是**全局单例** `window.app` ——
 * 不像 `win` 那样一个窗口一个实例。这些信息都来自 package.json
 * 的 `webwin` 段（`id` / `name` / `version`）与运行时路径。
 *
 * 退出相关事件（用 `app.on` 订阅）：
 * - `before-quit`：应用**开始关闭窗口之前**触发，处理函数里调 `app.stopQuit()` 可阻止退出。
 * - `will-quit`：所有窗口都关掉、**即将真正退出**时触发，处理函数里调 `app.stopQuit()` 仍可阻止
 *   （此时窗口已关，阻止后应用继续在后台运行，常见用法是配合托盘常驻）。
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

	/**
	 * 优雅退出：先触发 `before-quit` 事件，再尝试关闭所有窗口；
	 * 等所有窗口都关掉后触发 `will-quit` 事件，最后才真正退出进程。
	 *
	 * 这两个事件的处理函数里调用 `app.stopQuit()` 可以**阻止**退出
	 * （例如"有未保存的改动，先弹窗确认"）：
	 *
	 * ```js
	 * app.on('before-quit', () => {
	 *   if (hasUnsavedChanges) app.stopQuit(); // 阻止退出
	 * });
	 * app.on('will-quit', () => { app.stopQuit(); }); // 窗口已关，仍可阻止真正退出
	 * await app.quit();
	 * ```
	 *
	 * 注意：before-quit / will-quit 是异步下发给 JS 的，框架给一个宽限窗口
	 * （默认 200ms）来调用 stopQuit；超过这个窗口仍未阻止才会继续推进。
	 * 已经处于退出流程中时再次调用会被忽略。
	 */
	quit(): Promise<void> { return this.call('quit'); }

	/**
	 * 在 `before-quit` / `will-quit` 事件的处理函数里调用，阻止应用退出。
	 * 在事件之外调用无效（没有正在进行的退出流程）。
	 */
	stopQuit(): Promise<void> { return this.call('stopQuit'); }

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

	/**
	 * 允许当前正在申请的第二实例继续启动。
	 *
	 * 仅在 `second-instance` 事件的处理函数里调用才有意义：框架默认**拒绝**所有
	 * 第二实例（直接退出），只有你显式调用本方法，才会放行那"一个"申请。
	 * 多个第二实例并发时，每调用一次放行其中最早的一个；在事件之外调用无效（无在等的申请）。
	 *
	 * ```js
	 * app.on('second-instance', (info) => {
	 *   console.log('又启动了一次，参数：', info.argv, '工作目录：', info.cwd);
	 *   app.allowSecondInstanceOnce(); // 放行这个第二实例
	 * });
	 * ```
	 */
	allowSecondInstanceOnce(): Promise<void> { return this.call('allowSecondInstanceOnce'); }
}
