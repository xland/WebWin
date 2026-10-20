import { Ipc, SharedBufferData } from './Ipc';

/**
 * `readFile` 的编码。
 *
 * - `utf8`（默认）：当文本读。**非 UTF-8 的内容会出乱码**，要原样字节用下面两个
 * - `base64`：二进制内容无损，代价是字符串体积涨 1/3
 * - `binary`：字节走共享内存，返回 ArrayBuffer。零拷贝、无膨胀，
 *   但**用完必须 `release()`**，且 buffer 是只读的
 */
export type FsEncoding = 'utf8' | 'base64' | 'binary';

export interface FsReadOptions {
	/** 默认 `utf8` */
	encoding?: FsEncoding;
	/**
	 * 分块读取的块大小（字节）。大于 0 时 `readFile` 进入分块模式：
	 * 每读一块触发一次 `callback`，而不是一次性返回整份内容。默认 1MB。
	 * 不传或 <=0 时退化为一次性读取（原有行为）
	 */
	chunkSize?: number;
}

/** 分块读取时传给 callback 的每一块 */
export interface FsChunk {
	/** 本块内容（字符串）：`utf8` 模式是文本，`base64` 模式是 base64 串（binary 分块也走 base64） */
	data: string;
	/** 第几块，从 0 开始 */
	index: number;
	/** 是否最后一块 */
	isLast: boolean;
}

/** 分块读取整体完成时的 resolve 值 */
export interface FsReadResult {
	/** 文件总字节数 */
	totalBytes: number;
	/** 总块数 */
	totalChunks: number;
}

export interface FsWriteOptions {
	/** `base64` 时把 data 当 base64 解成字节再写；默认 `utf8` */
	encoding?: 'utf8' | 'base64';
}

/** `readFile` 用 `binary` 时的返回：字节走共享内存，不进 JSON */
export interface FsBinary extends SharedBufferData {
	/** 字节数 */
	size: number;
}

/**
 * `stat()` 的返回。
 *
 * 是**纯数据**，不像 Node 的 `Stats` 那样带 `isFile()` 方法 —— 那些方法过不了
 * 消息通道，所以一律给布尔字段。
 */
export interface FsStats {
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
export class Fs extends Ipc {
	private static readonly domain = 'fs'; // 对应 C++ 侧的 Fs 域处理器

	protected type = Fs.domain;

	/** 分块读取用的流序号（每次 readFile 流式调用递增），配合隐藏事件 fs-chunk 路由 callback */
	private streamSeq = 0;
	/** streamId -> 本次 readFile 流式的 callback */
	private chunkHandlers = new Map<number, (chunk: FsChunk) => void>();
	/** fs-chunk 事件的固定监听：按 payload.streamId 路由到对应 callback（同一引用，Set 去重，只订阅一次） */
	private fsChunkHandler = (payload: { streamId: number; content: string; index: number; isLast: boolean }) => {
		const cb = this.chunkHandlers.get(payload.streamId);
		if (cb) cb({ data: payload.content, index: payload.index, isLast: payload.isLast });
	};

	/** `access()` 用的位掩码，跟 Node 的 `fs.constants` 一致 */
	readonly constants = { F_OK: 0, X_OK: 1, W_OK: 2, R_OK: 4 } as const;

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
	/**
	 * 读文件。
	 *
	 * - **一次性**（默认）：同 Node 的 `readFile`，整份内容一次性返回。
	 * - **分块**（传 `callback`）：每读一块触发一次 callback，适合大文件按需处理、
	 *   不必把整份内容一次性塞进内存。块内容按 `encoding` 给（`utf8` 文本 / `base64` 字节）。
	 *
	 * ```ts
	 * // 一次性
	 * const text = await fs.readFile('config.json');
	 * const bin  = await fs.readFile('big.zip', 'binary'); bin.release();
	 *
	 * // 分块：每 1MB 触发一次回调
	 * await fs.readFile('big.zip', { encoding: 'base64', chunkSize: 1024 * 1024 }, (c) => {
	 *   console.log('第', c.index, '块，是否最后', c.isLast, '内容长', c.data.length);
	 * });
	 * ```
	 *
	 * 注意：二进制分块（`encoding: 'binary'` + callback）会退化成 base64 字符串，
	 * 因为分块走的是普通消息通道，拿不到 `binary` 的 ArrayBuffer；要 ArrayBuffer 用
	 * 一次性的 `fs.readFile(path, 'binary')` 或 `fs.readFileChunk(path, 'binary', start, size)`
	 */
	readFile(path: string, options?: FsReadOptions | FsEncoding, callback?: (chunk: FsChunk) => void): Promise<string | FsBinary>;
	readFile(path: string, options: 'binary' | { encoding: 'binary' }, callback?: (chunk: FsChunk) => void): Promise<FsBinary>;
	async readFile(path: string, options: FsReadOptions | FsEncoding = {}, callback?: (chunk: FsChunk) => void): Promise<string | FsBinary | FsReadResult> {
		const encoding = typeof options === 'string' ? options : (options.encoding ?? 'utf8');
		if (callback) {
			const chunkSize = typeof options === 'string' ? 0 : (options.chunkSize ?? 1024 * 1024);
			// 分块走"隐藏事件"通道：每块带 streamId，JS 侧按 streamId 路由到本次 callback。
			// 不扩展 Ipc 请求-响应协议；事件订阅只登记一次（Set 去重，见 fsChunkHandler）
			const streamId = ++this.streamSeq;
			this.chunkHandlers.set(streamId, callback);
			this.on('fs-chunk', this.fsChunkHandler);
			const result = await this.call<FsReadResult>('readFile', { path, encoding, chunkSize, streamId });
			this.chunkHandlers.delete(streamId);
			return result;
		}
		return this.call<string | FsBinary>('readFile', { path, encoding });
	}

	/**
	 * 读文件的一段：从 `start` 偏移读 `size` 字节，返回**单块**内容。
	 * 适合大文件的随机访问（不必读进全文）。
	 *
	 * ```ts
	 * const head = await fs.readFileChunk('big.bin', 'binary', 0, 256); // 前 256 字节
	 * ```
	 */
	readFileChunk(path: string, options?: FsReadOptions | FsEncoding, start?: number, size?: number): Promise<string>;
	readFileChunk(path: string, options: 'binary' | { encoding: 'binary' }, start?: number, size?: number): Promise<FsBinary>;
	async readFileChunk(path: string, options: FsReadOptions | FsEncoding = {}, start = 0, size = 0): Promise<string | FsBinary> {
		const encoding = typeof options === 'string' ? options : (options.encoding ?? 'utf8');
		return this.call<string | FsBinary>('readFileChunk', { path, encoding, start, size });
	}

	/**
	 * 写文件（覆盖）。文件不存在就创建，**但不会自动建父目录**
	 * （Node 的 writeFile 也不建，静默建目录会把拼错的路径变成一堆垃圾目录）
	 */
	writeFile(path: string, data: string, options?: FsWriteOptions): Promise<void> {
		return this.call<void>('writeFile', { path, data, encoding: options?.encoding ?? 'utf8' });
	}

	/** 追加到文件末尾；文件不存在就创建 */
	appendFile(path: string, data: string, options?: FsWriteOptions): Promise<void> {
		return this.call<void>('appendFile', { path, data, encoding: options?.encoding ?? 'utf8' });
	}

	/** 删文件。文件不存在会 reject */
	unlink(path: string): Promise<void> { return this.call<void>('unlink', { path }); }

	/** 复制文件；目标已存在时覆盖 */
	copyFile(src: string, dest: string): Promise<void> {
		return this.call<void>('copyFile', { src, dest });
	}

	/** 移动 / 改名；目标已存在时覆盖。跨盘也行 */
	rename(oldPath: string, newPath: string): Promise<void> {
		return this.call<void>('rename', { oldPath, newPath });
	}

	/** 取文件信息。不存在会 reject */
	stat(path: string): Promise<FsStats> { return this.call<FsStats>('stat', { path }); }

	/**
	 * 检查能不能访问。默认只查存在（`F_OK`）。
	 *
	 * ```ts
	 * await fs.access(p, fs.constants.R_OK | fs.constants.W_OK);
	 * ```
	 *
	 * 权限位是**近似**的：Windows 没有 POSIX 权限，`W_OK` 只看只读属性，不查 ACL。
	 */
	access(path: string, mode?: number): Promise<void> {
		return this.call<void>('access', { path, mode: mode ?? this.constants.F_OK });
	}
}
