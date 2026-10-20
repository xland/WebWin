import { Ipc, SharedBufferData } from './Ipc';

/**
 * `get` 的编码。
 *
 * - `utf8`（默认）：当文本读。**内容不是合法 UTF-8 时会出乱码**，要原样字节用下面两个
 * - `base64`：二进制无损，代价是字符串体积涨 1/3
 * - `binary`：字节走共享内存，返回 ArrayBuffer。零拷贝、无膨胀，
 *   但**用完必须 `release()`**，且 buffer 是只读的
 */
export type ResEncoding = 'utf8' | 'base64' | 'binary';

/**
 * 资源类型的可选参数。
 *
 * 不传或传 `RCDATA` 都是 `RT_RCDATA`（默认值）。传别的标准类型名（`ICON` / `HTML` /
 * `MANIFEST` / `VERSION` …）会映射到对应的 `RT_*`；**不认识的名字不当错误** ——
 * PE 资源表允许任意字符串类型名，原样传下去即可。
 */
export interface ResTypeOptions {
	/** 默认 `RCDATA` */
	type?: string;
}

export interface ResGetOptions extends ResTypeOptions {
	/** 默认 `utf8` */
	encoding?: ResEncoding;
}

/** `get` 用 `binary` 时的返回：字节走共享内存，不进 JSON */
export interface ResBinary extends SharedBufferData {
	/** 字节数 */
	size: number;
}

/** `list` 里的一条资源 */
export interface ResItem {
	/** 资源类型名（标准类型是 `RCDATA` 这类大写名，非标准类型是数字字符串） */
	type: string;
	/** 资源名 */
	name: string;
	/** 字节数 */
	size: number;
}

/** `list` 的返回 */
export interface ResListResult {
	items: ResItem[];
	count: number;
}

/** `getInfo` 的返回，就是 exe 属性窗口「详细信息」那一栏的内容 */
export interface ResInfo {
	/** 如 `1.2.3.4` */
	fileVersion: string;
	productVersion: string;
	/** `[major, minor, build, revision]` */
	fileVersionRaw: number[];
	productVersionRaw: number[];
	/**
	 * `StringFileInfo` 里的字符串字段（CompanyName / FileDescription / ProductName …）。
	 * 取不到哪个就没有哪个；整个字符串表读不出来时不给这个字段
	 */
	strings?: Record<string, string>;
}

/**
 * exe 资源读写 + exe 版本信息（res 域）。
 *
 * **写不了正在运行的那个 exe**：Windows 以映像方式映射它，写资源要打开文件写入的
 * 那一下必然被拒（`ERROR_SHARING_VIOLATION`）。所以 `addFile` / `addText` / `remove`
 * 的 exePath 要指向别的文件 —— 安装包、待分发的目标程序，或者先复制一份出来再改。
 * 四个读方法（`get` / `saveToFile` / `list` / `getInfo`）不受此限，指向自己没问题。
 *
 * **全部异步**：资源可能几 MB，而写资源要**重写整个 PE 文件**，
 * 跑在 UI 线程上窗口会僵住，所以一律走后台线程。
 *
 * exePath 相对路径以 package.json 所在目录为底，跟 `path.resolve()` 一致。
 */
export class Res extends Ipc {
	private static readonly domain = 'res'; // 对应 C++ 侧的 Res 域处理器

	protected type = Res.domain;

	/**
	 * 把一个文件整个塞进 exe。
	 *
	 * `name` 不传就用文件名（含扩展名）。同名资源会被覆盖。
	 *
	 * ```ts
	 * await res.addFile('D:\\dist\\MyApp.exe', 'assets/logo.png');
	 * await res.addFile('D:\\dist\\MyApp.exe', 'ui/index.html',
	 *                   { name: 'index.html' });
	 * ```
	 */
	addFile(exePath: string, filePath: string,
		options?: ResTypeOptions & { name?: string }): Promise<void> {
		return this.call<void>('addFile', {
			exePath, filePath, name: options?.name ?? '', type: options?.type ?? 'RCDATA',
		});
	}

	/**
	 * 把一段文本塞进 exe。
	 *
	 * ```ts
	 * await res.addText('D:\\dist\\MyApp.exe', 'app.json', '{"v":1}');
	 * ```
	 */
	addText(exePath: string, name: string, text: string,
		options?: ResTypeOptions & { encoding?: 'utf8' | 'base64' }): Promise<void> {
		return this.call<void>('addText', {
			exePath, name, text,
			encoding: options?.encoding ?? 'utf8',
			type: options?.type ?? 'RCDATA',
		});
	}

	/**
	 * 读一个资源。
	 *
	 * ```ts
	 * const text = await res.get('D:\\dist\\MyApp.exe', 'app.json');
	 * const b64  = await res.get('D:\\dist\\MyApp.exe', 'logo.png', 'base64');
	 * const bin  = await res.get('D:\\dist\\MyApp.exe', 'big.zip', 'binary');
	 * bin.release(); // 用完必须还
	 * ```
	 */
	get(exePath: string, name: string, options?: ResGetOptions | 'utf8' | 'base64'): Promise<string>;
	get(exePath: string, name: string, options: 'binary' | { encoding: 'binary' }): Promise<ResBinary>;
	async get(exePath: string, name: string, options: ResGetOptions | ResEncoding = {}): Promise<string | ResBinary> {
		const encoding = typeof options === 'string' ? options : (options.encoding ?? 'utf8');
		const type = (typeof options === 'string' ? undefined : options.type) ?? 'RCDATA';
		return this.call('get', { exePath, name, encoding, type });
	}

	/** 把资源**原样**写成文件（不转码、不做任何加工），要的就是那串字节 */
	saveToFile(exePath: string, name: string, destPath: string, options?: ResTypeOptions): Promise<void> {
		return this.call<void>('saveToFile', {
			exePath, name, destPath, type: options?.type ?? 'RCDATA',
		});
	}

	/** 删掉一个资源。名字或类型对不上会 reject —— 先用 `list` 看看实际有哪些 */
	remove(exePath: string, name: string, options?: ResTypeOptions): Promise<void> {
		return this.call<void>('remove', { exePath, name, type: options?.type ?? 'RCDATA' });
	}

	/**
	 * 列出 exe 里的资源。**不传 `type` 就列全部类型**，传了只列那一类 ——
	 * 这点跟本域其他方法不同（它们默认 RCDATA），因为"列全部"才是列表的默认预期。
	 *
	 * ```ts
	 * const r = await res.list('D:\\dist\\MyApp.exe');
	 * // r.items → [{ type: 'RCDATA', name: 'webwin.js', size: 20789 }, ...]
	 * ```
	 */
	list(exePath: string, options?: ResTypeOptions): Promise<ResListResult> {
		// 空串 = 不限制类型。传 'RCDATA' 会变成"只列 RCDATA"
		return this.call<ResListResult>('list', { exePath, type: options?.type ?? '' });
	}

	/** 读 exe 的版本信息（exe 属性窗口「详细信息」那一栏） */
	/**
	 * 读 exe 的版本信息块。返回的不只是版本号 ——
	 * `strings` 里还有 CompanyName / FileDescription / ProductName 这些文案
	 */
	getInfo(exePath: string): Promise<ResInfo> {
		return this.call<ResInfo>('getInfo', { exePath });
	}
}
