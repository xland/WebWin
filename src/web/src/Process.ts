import { Ipc, addMessageHook } from './Ipc';
import type { DownstreamMessage, Handler } from './Ipc';

/** `Process.spawn()` 的选项；都可以不传 */
export interface SpawnOptions {
	/** 子进程的工作目录；不给就用应用基准目录（package.json 所在目录） */
	cwd?: string;
	/** 是否**隐藏**控制台窗口，默认 true。想让用户看见就传 false */
	hide?: boolean;
	/** 脱离页面生命周期：页面销毁时不杀它，默认 false */
	detached?: boolean;
}

/** `stdout` / `stderr` 事件的载荷 */
export interface ProcessOutput {
	/**
	 * 这一段输出。按 UTF-8 解码；解不动时退回系统 ANSI 代码页
	 *（中文 Windows 上是 GBK，cmd.exe 这类老程序用的就是它）
	 */
	text: string;
}

/** `exit` 事件的载荷 */
export interface ProcessExit {
	/** 子进程的退出码 */
	code: number;
}

/**
 * 子进程（process 域）。
 *
 * `Process` 是**类型**（`window.Process`），不是实例 —— 跟 `Menu` / `Db` / `Tray` 一个路子。
 * 用静态方法 `Process.spawn()` 启动子进程，返回的是一个 **Process 实例**，
 * 也就是一个"进程对象"；输出与退出都作为**该对象**的事件推回。
 *
 * ```ts
 * const proc = await Process.spawn('cmd.exe', ['/c', 'echo hello']);
 * proc.on('stdout', (p) => log(p.text));
 * proc.on('exit',   (p) => log(`退出码 ${p.code}`));
 * ```
 *
 * 三点要留意：
 * - `spawn` 是 **async**：进程是 C++ 侧的 Win32 对象，得等它建好
 * - **监听要紧跟 await 同步挂上**。子进程一启动就开始输出，
 *   晚一步（比如先去 await 别的）开头几行就漏了
 * - 事件只推给**发起 spawn 的那个页面**；同时开着好几个子进程时各收各的
 *
 * 生命周期：页面被销毁时（切走 / 关窗口），它启动的、没标 `detached` 的
 * 子进程会被杀掉 —— 谁启动谁负责，不留孤儿进程
 */
export class Process extends Ipc {
	private static readonly domain = 'process'; // 对应 C++ 侧的 Process 域处理器

	/** 子进程 id → 实例：下行的输出 / 退出消息按 id 找回实例 */
	private static readonly registry = new Map<number, Process>();

	protected type = Process.domain;

	/**
	 * C++ 分配的子进程 id，只用于路由。
	 * @internal 开发者用不到 —— 要的是 `pid`（系统进程 id）
	 */
	readonly id: number;

	/** 系统进程 id */
	readonly pid: number;

	private readonly handlers: Map<string, Set<Handler>>;

	private constructor(id: number, pid: number) {
		super();
		this.id = id;
		this.pid = pid;
		this.handlers = new Map<string, Set<Handler>>();
	}

	/**
	 * 启动子进程，返回进程对象。**是 async**：得等 C++ 把进程建起来。
	 *
	 * 启动失败（找不到文件、权限不够 …）会 reject。
	 * 子进程**运行**失败不在这里体现 —— 看 `exit` 事件的退出码
	 */
	static async spawn(file: string, args?: string[], options?: SpawnOptions): Promise<Process> {
		const data = await Process.request<{ id: number; pid: number }>(
			Process.domain, 'spawn', { file, args: args ?? [], options });
		const proc = new Process(data.id, data.pid);
		Process.registry.set(proc.id, proc);
		return proc;
	}

	/**
	 * 订阅事件；返回退订函数。
	 *
	 * 这里**不**像 `os.on()` / `win.on()` 那样还要通知 C++ 一声 ——
	 * 子进程一建好就开始推了，订阅与否是页面自己的事（没人听就丢掉）。
	 * 所以这是实例自己的订阅表，不是 Ipc 那张"窗口 + 事件名"的全局表
	 */
	on<T = unknown>(name: string, handler: Handler<T>): () => void {
		let set = this.handlers.get(name);
		if (!set) {
			set = new Set();
			this.handlers.set(name, set);
		}
		set.add(handler as Handler);
		return () => this.off(name, handler as Handler);
	}

	/** 取消订阅 */
	off<T = unknown>(name: string, handler: Handler<T>): void {
		const set = this.handlers.get(name);
		if (!set) return;
		set.delete(handler as Handler);
		if (set.size === 0) this.handlers.delete(name);
	}

	/** @internal 通道收到子进程事件时，按 proc 字段转给对应实例的监听者 */
	static dispatch(msg: DownstreamMessage): void {
		const id = msg.proc as number;
		const proc = Process.registry.get(id);
		const set = proc?.handlers.get(msg.name as string);
		if (set) {
			// 拷贝一份再遍历：回调里可能退订，直接遍历原集合会漏掉后面的
			for (const handler of [...set]) {
				try {
					handler(msg.payload);
				} catch (e) {
					console.error('[WebWin] process event handler error:', msg.name, e);
				}
			}
		}
		// exit 是最后一个事件：这个实例不会再收到东西了，从注册表里摘掉
		if (msg.name === 'exit') Process.registry.delete(id);
	}
}

/**
 * 子进程事件是**通道层面**要认领的一类下行消息：
 * 它带 name / payload，但**不是**全局事件 —— 事件属于某个具体的 Process 实例，
 * 全局那张"窗口 + 事件名"的订阅表路由不到它。
 *
 * 所以在这里登记钩子自己领走，靠 `proc` 字段（子进程 id）认。
 * 通道那边不需要知道 process 域的存在，照这个套路加别的域也不用回去改 Ipc
 */
addMessageHook((msg) =>
{
	if (typeof msg.proc !== 'number') return false;
	Process.dispatch(msg);
	return true;
});
