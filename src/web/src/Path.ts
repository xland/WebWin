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
export interface ParsedPath {
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
export interface PathObject {
	root?: string;
	dir?: string;
	base?: string;
	ext?: string;
	name?: string;
}

// ---- 字符常量 ----
const CH_SLASH = 0x2f;     // /
const CH_BACKSLASH = 0x5c; // \
const CH_DOT = 0x2e;       // .
const CH_COLON = 0x3a;     // :
const CH_A_UP = 0x41, CH_Z_UP = 0x5a;
const CH_A_LOW = 0x61, CH_Z_LOW = 0x7a;

/**
 * Windows 保留设备名：不管带不带扩展名，这些名字都指向设备而不是文件。
 * 后三个是上标 ¹²³ —— `COM¹` 这类也会被系统当成 `COM1`
 */
const RESERVED_NAMES = [
	'CON', 'PRN', 'AUX', 'NUL',
	'COM1', 'COM2', 'COM3', 'COM4', 'COM5', 'COM6', 'COM7', 'COM8', 'COM9',
	'LPT1', 'LPT2', 'LPT3', 'LPT4', 'LPT5', 'LPT6', 'LPT7', 'LPT8', 'LPT9',
	'COM¹', 'COM²', 'COM³', 'LPT¹', 'LPT²', 'LPT³',
];

/** `/` 和 `\` 都算分隔符 */
function isSep(code: number): boolean {
	return code === CH_SLASH || code === CH_BACKSLASH;
}

/** 只看 `/`。个别地方要区分"用户写的是哪种斜杠"，所以不能一律用 isSep */
function isPosixSep(code: number): boolean {
	return code === CH_SLASH;
}

/** 是否可能是盘符字母（A-Z / a-z） */
function isDeviceRoot(code: number): boolean {
	return (code >= CH_A_UP && code <= CH_Z_UP) || (code >= CH_A_LOW && code <= CH_Z_LOW);
}

/**
 * `path` 前 `colonIndex` 个字符是不是保留设备名。
 * `NUL:` → 取 `NUL` 命中；没有冒号时 colonIndex 传 -1，取到的是去掉末位字符
 */
function isReservedName(path: string, colonIndex: number): boolean {
	return RESERVED_NAMES.indexOf(path.slice(0, colonIndex).toUpperCase()) !== -1;
}

/**
 * 消掉路径里的 `.` 和 `..`，分隔符统一成 `separator`。
 * 移植自 Node 的 `normalizeString`：那几个 `lastSegmentLength` / `dots`
 * 分支是在处理 `....` 这类连续点的历史行为，别自己化简
 *
 * @param allowAboveRoot 为真时根之上的 `..` 原样保留，为假时吃掉
 */
function normalizeString(path: string, allowAboveRoot: boolean, separator: string): string {
	let res = '';
	let lastSegmentLength = 0;
	let lastSlash = -1;
	let dots = 0;
	let code = 0;
	for (let i = 0; i <= path.length; ++i) {
		if (i < path.length)
			code = path.charCodeAt(i);
		else if (isSep(code))
			break;
		else
			code = CH_SLASH;

		if (isSep(code)) {
			if (lastSlash === i - 1 || dots === 1) {
				// NOOP：连续的分隔符、或者孤立的单点，都不往结果里写
			} else if (dots === 2) {
				if (res.length < 2 || lastSegmentLength !== 2 ||
					res.charCodeAt(res.length - 1) !== CH_DOT ||
					res.charCodeAt(res.length - 2) !== CH_DOT) {
					if (res.length > 2) {
						const lastSlashIndex = res.length - lastSegmentLength - 1;
						if (lastSlashIndex === -1) {
							res = '';
							lastSegmentLength = 0;
						} else {
							res = res.slice(0, lastSlashIndex);
							lastSegmentLength = res.length - 1 - res.lastIndexOf(separator);
						}
						lastSlash = i;
						dots = 0;
						continue;
					} else if (res.length !== 0) {
						res = '';
						lastSegmentLength = 0;
						lastSlash = i;
						dots = 0;
						continue;
					}
				}
				if (allowAboveRoot) {
					res += res.length > 0 ? `${separator}..` : '..';
					lastSegmentLength = 2;
				}
			} else {
				if (res.length > 0)
					res += `${separator}${path.slice(lastSlash + 1, i)}`;
				else
					res = path.slice(lastSlash + 1, i);
				lastSegmentLength = i - lastSlash - 1;
			}
			lastSlash = i;
			dots = 0;
		} else if (code === CH_DOT && dots !== -1) {
			++dots;
		} else {
			dots = -1;
		}
	}
	return res;
}

/** 参数必须是字符串，否则抛 TypeError（与 Node 一致的强校验） */
function needString(value: unknown, name: string): string {
	if (typeof value !== 'string')
		throw new TypeError(`path：参数 "${name}" 必须是字符串，收到 ${value === null ? 'null' : typeof value}`);
	return value;
}

/** `ext` 前面补点，空的返回空 */
function formatExt(ext: string | undefined): string {
	return ext ? `${ext[0] === '.' ? '' : '.'}${ext}` : '';
}

export class Path {
	/** 路径分隔符 */
	readonly sep = '\\';
	/** PATH 那种环境变量里分隔多个路径用的符号 */
	readonly delimiter = ';';

	/**
	 * `resolve()` / `relative()` 的基准目录，启动时由 C++ 注入。
	 * @internal 开发者不要自己 new，用 `window.path`
	 */
	private readonly base: string;

	constructor(base: string = '') {
		this.base = base;
	}

	/** `resolve()` / `relative()` 用的基准目录 */
	getBase(): string {
		return this.base;
	}

	// ---------------- 组装 ----------------

	/**
	 * 拼接后规范化：多余分隔符、`.`、`..` 都会消掉。
	 * 参数里有绝对路径时**不会**重置（这点和 `resolve` 不同）。
	 *
	 * 任一段不是字符串就抛 TypeError。
	 */
	join(...args: string[]): string {
		if (args.length === 0) return '.';

		const parts: string[] = [];
		for (const arg of args) {
			needString(arg, 'path');
			if (arg.length > 0) parts.push(arg); // 空段直接丢掉
		}
		if (parts.length === 0) return '.';

		const firstPart = parts[0];
		let joined = parts.join('\\');

		// 开头不能留下两个分隔符，否则 normalize() 会把它当成 UNC 路径。
		// 但用户明显是奔着 UNC 来的（开头正好两个分隔符 + 后面还有非分隔符）时跳过
		let needsReplace = true;
		let slashCount = 0;
		if (isSep(firstPart.charCodeAt(0))) {
			++slashCount;
			const firstLen = firstPart.length;
			if (firstLen > 1 && isSep(firstPart.charCodeAt(1))) {
				++slashCount;
				if (firstLen > 2) {
					if (isSep(firstPart.charCodeAt(2))) ++slashCount;
					else needsReplace = false; // 用户就是要 UNC
				}
			}
		}
		if (needsReplace) {
			while (slashCount < joined.length && isSep(joined.charCodeAt(slashCount))) slashCount++;
			if (slashCount >= 2) joined = `\\${joined.slice(slashCount)}`;
		}

		// 带保留设备名（比如 `NUL:`）时不走 normalize：它是设备路径，
		// 规范化会把冒号那段当成盘符处理掉
		const segments: string[] = [];
		let seg = '';
		for (let i = 0; i < joined.length; i++) {
			if (joined[i] === '\\') {
				if (seg) segments.push(seg);
				seg = '';
				while (i + 1 < joined.length && joined[i + 1] === '\\') i++; // 跳过连续分隔符
			} else {
				seg += joined[i];
			}
		}
		if (seg) segments.push(seg);
		if (segments.some((s) => {
			const ci = s.indexOf(':');
			return ci !== -1 && isReservedName(s, ci);
		})) {
			let result = '';
			for (let i = 0; i < joined.length; i++) result += joined[i] === '/' ? '\\' : joined[i];
			return result;
		}

		return this.normalize(joined);
	}

	/**
	 * 从右往左解析：遇到第一个绝对路径就停，剩下的拼在它后面。
	 * 全是相对路径时以应用基准目录（`getBase()`）为底。
	 *
	 * ```ts
	 * path.resolve('data', 'a.db');  // '<基准目录>\data\a.db'
	 * path.resolve('C:\\x', 'y');    // 'C:\\x\\y'
	 * ```
	 */
	resolve(...args: string[]): string {
		let resolvedDevice = '';
		let resolvedTail = '';
		let resolvedAbsolute = false;

		for (let i = args.length - 1; i >= -1; i--) {
			let path: string;
			if (i >= 0) {
				path = needString(args[i], `paths[${i}]`);
				if (path.length === 0) continue; // 空段跳过
			} else if (resolvedDevice.length === 0) {
				path = this.base;
				// 快速通道：结果就是基准目录本身，不用再走一遍规范化
				if (args.length === 0 ||
					(args.length === 1 && (args[0] === '' || args[0] === '.') && isSep(path.charCodeAt(0)))) {
					return path;
				}
			} else {
				// 已经定了盘符、但还没遇到绝对路径：基准目录在别的盘上时不能混进来，
				// 退到"<盘符>:\"当底。（Node 这里查的是每个盘符各自的当前目录，
				// 环境变量 `=C:` 那套，我们没有）
				path = this.base;
				if (path.slice(0, 2).toLowerCase() !== resolvedDevice.toLowerCase() &&
					path.charCodeAt(2) === CH_BACKSLASH) {
					path = `${resolvedDevice}\\`;
				}
			}

			const len = path.length;
			let rootEnd = 0;
			let device = '';
			let isAbsolute = false;
			const code = path.charCodeAt(0);

			if (len === 1) {
				if (isSep(code)) { rootEnd = 1; isAbsolute = true; }
			} else if (isSep(code)) {
				// 以分隔符开头，至少是个根
				isAbsolute = true;
				if (isSep(path.charCodeAt(1))) {
					let j = 2;
					let last = j;
					while (j < len && !isSep(path.charCodeAt(j))) j++;
					if (j < len && j !== last) {
						const firstPart = path.slice(last, j);
						last = j;
						while (j < len && isSep(path.charCodeAt(j))) j++;
						if (j < len && j !== last) {
							last = j;
							while (j < len && !isSep(path.charCodeAt(j))) j++;
							if (j === len || j !== last) {
								if (firstPart !== '.' && firstPart !== '?') {
									device = `\\\\${firstPart}\\${path.slice(last, j)}`; // UNC
									rootEnd = j;
								} else {
									device = `\\\\${firstPart}`;                         // 设备根：`\\.\` / `\\?\`
									rootEnd = 4;
								}
							}
						}
					}
				} else {
					rootEnd = 1;
				}
			} else if (isDeviceRoot(code) && path.charCodeAt(1) === CH_COLON) {
				device = path.slice(0, 2);
				rootEnd = 2;
				if (len > 2 && isSep(path.charCodeAt(2))) { isAbsolute = true; rootEnd = 3; }
			}

			if (device.length > 0) {
				if (resolvedDevice.length > 0) {
					// 别的盘上的绝对路径：这段不算数
					if (device.toLowerCase() !== resolvedDevice.toLowerCase()) continue;
				} else {
					resolvedDevice = device;
				}
			}

			if (resolvedAbsolute) {
				if (resolvedDevice.length > 0) break;
			} else {
				resolvedTail = `${path.slice(rootEnd)}\\${resolvedTail}`;
				resolvedAbsolute = isAbsolute;
				if (isAbsolute && resolvedDevice.length > 0) break;
			}
		}

		resolvedTail = normalizeString(resolvedTail, !resolvedAbsolute, '\\');
		return resolvedAbsolute
			? `${resolvedDevice}\\${resolvedTail}`
			: `${resolvedDevice}${resolvedTail}` || '.';
	}

	/** 规范化：消掉 `.` / `..` / 重复分隔符，斜杠统一成反斜杠 */
	normalize(path: string): string {
		needString(path, 'path');
		const len = path.length;
		if (len === 0) return '.';
		if (len === 1) return isPosixSep(path.charCodeAt(0)) ? '\\' : path;

		let rootEnd = 0;
		let device: string | undefined;
		let isAbsolute = false;
		const code = path.charCodeAt(0);

		if (isSep(code)) {
			// 以分隔符开头，至少是个根（UNC 或 `\foo`）
			isAbsolute = true;
			if (isSep(path.charCodeAt(1))) {
				let j = 2;
				let last = j;
				while (j < len && !isSep(path.charCodeAt(j))) j++;
				if (j < len && j !== last) {
					const firstPart = path.slice(last, j);
					last = j;
					while (j < len && isSep(path.charCodeAt(j))) j++;
					if (j < len && j !== last) {
						last = j;
						while (j < len && !isSep(path.charCodeAt(j))) j++;
						if (j === len || j !== last) {
							if (firstPart === '.' || firstPart === '?') {
								// 设备根：`\\.\PHYSICALDRIVE0`
								device = `\\\\${firstPart}`;
								rootEnd = 4;
								const colonIndex = path.indexOf(':');
								// `\\?\COM1:` 这类保留设备名要单独认
								const possibleDevice = path.slice(4, colonIndex + 1);
								if (isReservedName(possibleDevice, possibleDevice.length - 1)) {
									device = `\\\\?\\${possibleDevice}`;
									rootEnd = 4 + possibleDevice.length;
								}
							} else if (j === len) {
								return `\\\\${firstPart}\\${path.slice(last)}\\`; // 整条就是 UNC 根
							} else {
								device = `\\\\${firstPart}\\${path.slice(last, j)}`;
								rootEnd = j;
							}
						}
					}
				}
			} else {
				rootEnd = 1;
			}
		} else {
			const colonIndex = path.indexOf(':');
			if (colonIndex > 0) {
				if (isDeviceRoot(code) && colonIndex === 1) {
					device = path.slice(0, 2);
					rootEnd = 2;
					if (len > 2 && isSep(path.charCodeAt(2))) { isAbsolute = true; rootEnd = 3; }
				} else if (isReservedName(path, colonIndex)) {
					// 保留设备名当根：`NUL:...`
					device = path.slice(0, colonIndex + 1);
					rootEnd = colonIndex + 1;
				}
			}
		}

		const tail = rootEnd < len
			? normalizeString(path.slice(rootEnd), !isAbsolute, '\\')
			: '';
		let out = tail;
		if (out.length === 0 && !isAbsolute) out = '.';
		if (out.length > 0 && isSep(path.charCodeAt(len - 1))) out += '\\';

		// CVE-2024-36139：相对路径里冒出 `X:` 这种，系统可能把它当绝对路径解释，
		// 补一个 `.\` 前缀锁死成相对路径
		if (!isAbsolute && device === undefined && path.indexOf(':') !== -1) {
			if (out.length >= 2 && isDeviceRoot(out.charCodeAt(0)) && out.charCodeAt(1) === CH_COLON) {
				return `.\\${out}`;
			}
			let index = path.indexOf(':');
			do {
				if (index === len - 1 || isSep(path.charCodeAt(index + 1))) return `.\\${out}`;
			} while ((index = path.indexOf(':', index + 1)) !== -1);
		}

		if (isReservedName(path, path.indexOf(':'))) return `.\\${device ?? ''}${out}`;
		if (device === undefined) return isAbsolute ? `\\${out}` : out;
		return isAbsolute ? `${device}\\${out}` : `${device}${out}`;
	}

	// ---------------- 拆分 ----------------

	/** 最后一段（含扩展名）。给了 `suffix` 且正好对上，就从尾部去掉 */
	basename(path: string, suffix?: string): string {
		if (suffix !== undefined) needString(suffix, 'suffix');
		needString(path, 'path');

		let start = 0;
		let end = -1;
		let matchedSlash = true;

		// 先把盘符让过去：否则 `C:` 后面那个分隔符会被当成"末尾多余的分隔符"吃掉
		if (path.length >= 2 && isDeviceRoot(path.charCodeAt(0)) && path.charCodeAt(1) === CH_COLON) {
			start = 2;
		}

		if (suffix !== undefined && suffix.length > 0 && suffix.length <= path.length) {
			if (suffix === path) return '';
			let extIdx = suffix.length - 1;
			let firstNonSlashEnd = -1;
			for (let i = path.length - 1; i >= start; --i) {
				const code = path.charCodeAt(i);
				if (isSep(code)) {
					if (!matchedSlash) { start = i + 1; break; }
				} else {
					if (firstNonSlashEnd === -1) {
						matchedSlash = false;
						firstNonSlashEnd = i + 1;
					}
					if (extIdx >= 0) {
						if (code === suffix.charCodeAt(extIdx)) {
							if (--extIdx === -1) end = i; // 扩展名对上了
						} else {
							extIdx = -1;                  // 对不上：整段都算
							end = firstNonSlashEnd;
						}
					}
				}
			}
			if (start === end) end = firstNonSlashEnd;
			else if (end === -1) end = path.length;
			return path.slice(start, end);
		}

		for (let i = path.length - 1; i >= start; --i) {
			if (isSep(path.charCodeAt(i))) {
				if (!matchedSlash) { start = i + 1; break; }
			} else if (end === -1) {
				matchedSlash = false;
				end = i + 1;
			}
		}
		if (end === -1) return '';
		return path.slice(start, end);
	}

	/** 所在目录 */
	dirname(path: string): string {
		needString(path, 'path');
		const len = path.length;
		if (len === 0) return '.';
		const code = path.charCodeAt(0);
		if (len === 1) return isSep(code) ? path : '.';

		let rootEnd = -1;
		let offset = 0;

		if (isSep(code)) {
			rootEnd = offset = 1;
			if (isSep(path.charCodeAt(1))) {
				let j = 2;
				let last = j;
				while (j < len && !isSep(path.charCodeAt(j))) j++;
				if (j < len && j !== last) {
					last = j;
					while (j < len && isSep(path.charCodeAt(j))) j++;
					if (j < len && j !== last) {
						last = j;
						while (j < len && !isSep(path.charCodeAt(j))) j++;
						if (j === len) return path; // 整条就是 UNC 根
						if (j !== last) rootEnd = offset = j + 1;
					}
				}
			}
		} else if (isDeviceRoot(code) && path.charCodeAt(1) === CH_COLON) {
			rootEnd = len > 2 && isSep(path.charCodeAt(2)) ? 3 : 2;
			offset = rootEnd;
		}

		let end = -1;
		let matchedSlash = true;
		for (let i = len - 1; i >= offset; --i) {
			if (isSep(path.charCodeAt(i))) {
				if (!matchedSlash) { end = i; break; }
			} else {
				matchedSlash = false;
			}
		}

		if (end === -1) {
			if (rootEnd === -1) return '.';
			end = rootEnd;
		}
		return path.slice(0, end);
	}

	/** 扩展名（含前导点）；没有、或最后一段以点开头时返回 `''` */
	extname(path: string): string {
		needString(path, 'path');
		let start = 0;
		let startDot = -1;
		let startPart = 0;
		let end = -1;
		let matchedSlash = true;
		let preDotState = 0;

		if (path.length >= 2 && path.charCodeAt(1) === CH_COLON && isDeviceRoot(path.charCodeAt(0))) {
			start = startPart = 2;
		}

		for (let i = path.length - 1; i >= start; --i) {
			const code = path.charCodeAt(i);
			if (isSep(code)) {
				if (!matchedSlash) { startPart = i + 1; break; }
				continue;
			}
			if (end === -1) {
				matchedSlash = false;
				end = i + 1;
			}
			if (code === CH_DOT) {
				if (startDot === -1) startDot = i;
				else if (preDotState !== 1) preDotState = 1;
			} else if (startDot !== -1) {
				preDotState = -1;
			}
		}

		if (startDot === -1 || end === -1 ||
			preDotState === 0 ||
			// 最后一段正好是 `..`
			(preDotState === 1 && startDot === end - 1 && startDot === startPart + 1)) {
			return '';
		}
		return path.slice(startDot, end);
	}

	/** 拆成 `{ root, dir, base, ext, name }` 五段 */
	parse(path: string): ParsedPath {
		needString(path, 'path');
		const ret: ParsedPath = { root: '', dir: '', base: '', ext: '', name: '' };
		const len = path.length;
		if (len === 0) return ret;

		let rootEnd = 0;
		let code = path.charCodeAt(0);

		if (len === 1) {
			if (isSep(code)) { ret.root = ret.dir = path; return ret; }
			ret.base = ret.name = path;
			return ret;
		}

		if (isSep(code)) {
			rootEnd = 1;
			if (isSep(path.charCodeAt(1))) {
				let j = 2;
				let last = j;
				while (j < len && !isSep(path.charCodeAt(j))) j++;
				if (j < len && j !== last) {
					last = j;
					while (j < len && isSep(path.charCodeAt(j))) j++;
					if (j < len && j !== last) {
						last = j;
						while (j < len && !isSep(path.charCodeAt(j))) j++;
						if (j === len) rootEnd = j;
						else if (j !== last) rootEnd = j + 1;
					}
				}
			}
		} else if (isDeviceRoot(code) && path.charCodeAt(1) === CH_COLON) {
			if (len <= 2) { ret.root = ret.dir = path; return ret; } // 光一个 `C:`
			rootEnd = 2;
			if (isSep(path.charCodeAt(2))) {
				if (len === 3) { ret.root = ret.dir = path; return ret; } // 光一个 `C:\`
				rootEnd = 3;
			}
		}
		if (rootEnd > 0) ret.root = path.slice(0, rootEnd);

		let startDot = -1;
		let startPart = rootEnd;
		let end = -1;
		let matchedSlash = true;
		let preDotState = 0;

		for (let i = len - 1; i >= rootEnd; --i) {
			code = path.charCodeAt(i);
			if (isSep(code)) {
				if (!matchedSlash) { startPart = i + 1; break; }
				continue;
			}
			if (end === -1) { matchedSlash = false; end = i + 1; }
			if (code === CH_DOT) {
				if (startDot === -1) startDot = i;
				else if (preDotState !== 1) preDotState = 1;
			} else if (startDot !== -1) {
				preDotState = -1;
			}
		}

		if (end !== -1) {
			if (startDot === -1 || preDotState === 0 ||
				(preDotState === 1 && startDot === end - 1 && startDot === startPart + 1)) {
				ret.base = ret.name = path.slice(startPart, end);
			} else {
				ret.name = path.slice(startPart, startDot);
				ret.base = path.slice(startPart, end);
				ret.ext = path.slice(startDot, end);
			}
		}

		// 目录就是根时，dir 带尾部分隔符（`C:\abc` → `C:\`）；否则剥掉
		if (startPart > 0 && startPart !== rootEnd) ret.dir = path.slice(0, startPart - 1);
		else ret.dir = ret.root;

		return ret;
	}

	/** `parse()` 的逆运算。给了 `base` 时忽略 `name` / `ext` */
	format(pathObject: PathObject): string {
		if (pathObject === null || typeof pathObject !== 'object')
			throw new TypeError(`path.format()：参数必须是对象，收到 ${pathObject === null ? 'null' : typeof pathObject}`);
		const dir = pathObject.dir || pathObject.root;
		const base = pathObject.base || `${pathObject.name || ''}${formatExt(pathObject.ext)}`;
		if (!dir) return base;
		return dir === pathObject.root ? `${dir}${base}` : `${dir}\\${base}`;
	}

	// ---------------- 判定 / 计算 ----------------

	/** 是否绝对路径（盘符加分隔符、UNC、或 `\foo` 这种根相对）。`C:foo` 不算 */
	isAbsolute(path: string): boolean {
		needString(path, 'path');
		const len = path.length;
		if (len === 0) return false;
		const code = path.charCodeAt(0);
		if (isSep(code)) return true;
		return len > 2 && isDeviceRoot(code) && path.charCodeAt(1) === CH_COLON && isSep(path.charCodeAt(2));
	}

	/**
	 * `from` 到 `to` 的相对路径。两边先按 `resolve()` 展开，
	 * 所以给相对路径时以应用基准目录为底。不同盘符算不出来，返回 `to`。
	 */
	relative(from: string, to: string): string {
		needString(from, 'from');
		needString(to, 'to');
		if (from === to) return '';

		const fromOrig = this.resolve(from);
		const toOrig = this.resolve(to);
		if (fromOrig === toOrig) return '';

		const fromLow = fromOrig.toLowerCase();
		const toLow = toOrig.toLowerCase();
		if (fromLow === toLow) return '';

		// 掐掉开头的分隔符
		let fromStart = 0;
		while (fromStart < fromLow.length && fromLow.charCodeAt(fromStart) === CH_BACKSLASH) fromStart++;
		// 掐掉结尾的分隔符（UNC 才会带）
		let fromEnd = fromLow.length;
		while (fromEnd - 1 > fromStart && fromLow.charCodeAt(fromEnd - 1) === CH_BACKSLASH) fromEnd--;
		const fromLen = fromEnd - fromStart;

		let toStart = 0;
		while (toStart < toLow.length && toLow.charCodeAt(toStart) === CH_BACKSLASH) toStart++;
		let toEnd = toLow.length;
		while (toEnd - 1 > toStart && toLow.charCodeAt(toEnd - 1) === CH_BACKSLASH) toEnd--;
		const toLen = toEnd - toStart;

		// 找从根开始最长的公共段
		const length = fromLen < toLen ? fromLen : toLen;
		let lastCommonSep = -1;
		let i = 0;
		for (; i < length; i++) {
			const fromCode = fromLow.charCodeAt(fromStart + i);
			if (fromCode !== toLow.charCodeAt(toStart + i)) break;
			else if (fromCode === CH_BACKSLASH) lastCommonSep = i;
		}

		if (i !== length) {
			if (lastCommonSep === -1) return toOrig; // 连第一段都不同（换盘了）
		} else {
			if (toLen > length) {
				if (toLow.charCodeAt(toStart + i) === CH_BACKSLASH) return toOrig.slice(toStart + i + 1);
				if (i === 2) return toOrig.slice(toStart + i);
			}
			if (fromLen > length) {
				if (fromLow.charCodeAt(fromStart + i) === CH_BACKSLASH) lastCommonSep = i;
				else if (i === 2) lastCommonSep = 0;
			}
			if (lastCommonSep === -1) lastCommonSep = 0;
		}

		let out = '';
		for (i = fromStart + lastCommonSep + 1; i <= fromEnd; ++i) {
			if (i === fromEnd || fromLow.charCodeAt(i) === CH_BACKSLASH) {
				if (out.length === 0) out += '..';
				else out += '\\..';
			}
		}
		toStart += lastCommonSep;
		if (out.length > 0) return `${out}${toOrig.slice(toStart, toEnd)}`;

		toStart++;
		if (toOrig.charCodeAt(toStart) === CH_BACKSLASH) toStart++;
		return toOrig.slice(toStart, toEnd);
	}

	// ---------------- Windows 专属校验（Node 没有） ----------------

	/**
	 * 单个文件/目录名在 Windows 上是否可用。查三件事，都是纯字符串能查的：
	 * 1. 非法字符：`<>:"|?*`、两个分隔符、以及 0-31 控制字符
	 * 2. 保留设备名：`CON` / `PRN` / `AUX` / `NUL` / `COM1-9` / `LPT1-9`
	 *    （带扩展名也算，`nul.txt` 同样指向设备；大小写不敏感）
	 * 3. 以点或空格结尾（系统会自动把尾巴上的点和空格去掉，存出来跟你要的不一样）
	 *
	 * 传的是**单段名字**，不要带分隔符。
	 */
	isValidName(name: string): boolean {
		needString(name, 'name');
		if (name.length === 0) return false;
		for (let i = 0; i < name.length; ++i) {
			if (name.charCodeAt(i) <= 31) return false;               // 控制字符
			if ('<>:"/\\|?*'.indexOf(name[i]) >= 0) return false;
		}
		const tail = name[name.length - 1];
		if (tail === '.' || tail === ' ') return false;
		// 保留设备名：`NUL.txt` 也指向设备，所以先掐掉扩展名再比
		const dot = name.indexOf('.');
		const stem = (dot === -1 ? name : name.slice(0, dot)).toUpperCase();
		return RESERVED_NAMES.indexOf(stem) === -1;
	}

	/**
	 * 整条路径在 Windows 上是否可用：按分隔符切段，逐段用 `isValidName()` 查。
	 * 盘符（`C:`）、UNC 根、`\\?\` 前缀、以及 `.` / `..` 都不参与名字校验。
	 */
	isValidPath(path: string): boolean {
		needString(path, 'path');
		if (path.length === 0) return false;

		let rest = path;
		// 掐掉 `\\?\` / `\\.\` 前缀
		if (rest.startsWith('\\\\?\\') || rest.startsWith('\\\\.\\')) rest = rest.slice(4);
		// 掐掉盘符
		if (rest.length >= 2 && isDeviceRoot(rest.charCodeAt(0)) && rest.charCodeAt(1) === CH_COLON) {
			rest = rest.slice(2);
			if (rest.length === 0) return true; // 光一个 `C:` 合法（相对该盘当前目录）
		}

		for (const seg of rest.split(/[\\/]/)) {
			if (seg === '' || seg === '.' || seg === '..') continue;
			if (!this.isValidName(seg)) return false;
		}
		return true;
	}
}
