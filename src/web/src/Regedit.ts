import { Ipc } from './Ipc';

/**
 * 注册表根键。
 *
 * 简写（`HKCU`）与全名（`HKEY_CURRENT_USER`）都认，不分大小写 ——
 * 少一个"写错就报错"的坑
 */
export type RegRoot =
	| 'HKCR' | 'HKEY_CLASSES_ROOT'
	| 'HKCU' | 'HKEY_CURRENT_USER'
	| 'HKLM' | 'HKEY_LOCAL_MACHINE'
	| 'HKU' | 'HKEY_USERS'
	| 'HKCC' | 'HKEY_CURRENT_CONFIG';

/** 键的完整路径，写法跟 reg.exe 一样：`'HKCU\\Software\\MyApp'` */
export type RegKeyPath = `${RegRoot}\\${string}` | RegRoot;

/** 注册表值的类型 */
export type RegType =
	| 'REG_SZ'        // 字符串
	| 'REG_EXPAND_SZ' // 字符串，里面可能有 `%PATH%` 这种待展开的引用
	| 'REG_MULTI_SZ'  // 字符串数组
	| 'REG_DWORD'     // 32 位无符号整数
	| 'REG_QWORD'     // 64 位无符号整数
	| 'REG_BINARY';   // 二进制

/**
 * 一个注册表值。`type` 决定 `data` 的形态：
 *
 * | `type` | `data` |
 * | --- | --- |
 * | `REG_SZ` / `REG_EXPAND_SZ` | `string` |
 * | `REG_MULTI_SZ` | `string[]` |
 * | `REG_DWORD` / `REG_QWORD` | `number` |
 * | `REG_BINARY` | base64 字符串 |
 *
 * `REG_EXPAND_SZ` 与 `REG_SZ` 是两个类型：前者里的 `%PATH%` 这类引用
 * **留着不展开**（要展开得调系统 API），混着用会出问题，所以分开给
 *
 * `REG_QWORD` 超过 2^53 会丢精度 —— JS 的 number 是 double，
 * 跟 SQLite 的 INTEGER 一个毛病
 */
export type RegValue =
	| { type: 'REG_SZ' | 'REG_EXPAND_SZ'; data: string }
	| { type: 'REG_MULTI_SZ'; data: string[] }
	| { type: 'REG_DWORD' | 'REG_QWORD'; data: number }
	| { type: 'REG_BINARY'; data: string };

/**
 * 注册表（regedit 域）。
 *
 * 与窗口无关，所以是**全局单例** `window.regedit`。
 *
 * ```ts
 * await regedit.set('HKCU\\Software\\MyApp', 'greeting', 'hello');
 * const value = await regedit.get('HKCU\\Software\\MyApp', 'greeting');
 * // → { type: 'REG_SZ', data: 'hello' }
 * ```
 *
 * 权限上要留意：**写 `HKLM` 需要管理员**，普通权限下会 reject；
 * `HKCU` 是当前用户的，随便写
 */
export class Regedit extends Ipc {
	private static readonly domain = 'regedit'; // 对应 C++ 侧的 Regedit 域处理器

	protected type = Regedit.domain;

	/**
	 * 读一个值。**键或值不存在返回 `null`**（不 reject），真正的故障才 reject。
	 *
	 * `name` 不传（或给空串）= 读这个键的默认值
	 *（也就是注册表编辑器里那个"(默认)"）
	 */
	get(key: RegKeyPath, name?: string): Promise<RegValue | null> {
		return this.call('get', { key, name });
	}

	/**
	 * 写一个值。键不存在时**自动逐层创建**（跟 reg.exe add 一样）。
	 *
	 * 类型按 `value` 的 JS 类型挑：字符串 → `REG_SZ`；数字 → `REG_DWORD`
	 *（32 位放得下）或 `REG_QWORD`。要写 `REG_EXPAND_SZ` / `REG_BINARY`
	 * 这类，等加了显式 type 选项再说
	 */
	set(key: RegKeyPath, name: string, value: string | number): Promise<void> {
		return this.call('set', { key, name, value });
	}

	/** 删掉整个键，连子键和值一起（递归）。键本来就不在也不报错 */
	deleteKey(key: RegKeyPath): Promise<void> {
		return this.call('deleteKey', { key });
	}

	/** 列出该键下的子键名，只下一层，不递归 */
	listKeys(key: RegKeyPath): Promise<string[]> {
		return this.call('listKeys', { key });
	}

	/**
	 * 列出该键下的值名（**不含值**，要值就 `get` 逐个读）
	 *
	 * 默认值也在里面，它的名字是空串
	 */
	listValues(key: RegKeyPath): Promise<string[]> {
		return this.call('listValues', { key });
	}
}
