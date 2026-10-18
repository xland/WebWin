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
	readFile(path: string, options?: FsReadOptions | 'utf8' | 'base64'): Promise<string>;
	readFile(path: string, options: 'binary' | { encoding: 'binary' }): Promise<FsBinary>;
	async readFile(path: string, options: FsReadOptions | FsEncoding = {}): Promise<string | FsBinary> {
		const encoding = typeof options === 'string' ? options : (options.encoding ?? 'utf8');
		return this.call('readFile', { path, encoding });
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
