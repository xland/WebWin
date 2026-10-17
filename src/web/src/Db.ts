import { Ipc } from './Ipc';

/**
 * 绑定参数。两种都收：
 * - 数组  → 按位置绑 `?`，第 1 个元素对应第 1 个问号
 * - 对象  → 按名字绑，SQL 里写 `:name` / `@name` / `$name` 都能对上
 *
 * 一律走绑定，**不要自己拼 SQL 字符串**（拼出来的既容易错，也是注入口子）
 */
export type DbParams = unknown[] | Record<string, unknown>;

/** 一行结果。SQLite 的 5 种类型映射过来只剩 number / string / null 三种 */
export type DbRow = Record<string, unknown>;

/** run() 的结果 */
export interface DbRunResult {
	/** 受影响的行数 */
	changes: number;
	/** 最后一次插入的 rowid */
	lastInsertRowid: number;
}

/**
 * SQLite 数据库。**一个实例 = 一个连接。**
 *
 * 库文件默认落在应用数据目录的 `db/` 下（跟 WebView2 的浏览器数据分开），
 * 也可以给完整路径自己指定。
 *
 * ```ts
 * const db = await Db.open('app.db');
 * await db.exec('CREATE TABLE IF NOT EXISTS t (id INTEGER PRIMARY KEY, v TEXT)');
 * await db.run('INSERT INTO t (v) VALUES (?)', ['hello']);
 * const rows = await db.all('SELECT * FROM t WHERE id = :id', { id: 1 });
 * await db.close();
 * ```
 *
 * 两个已知取舍：
 * - SQLite 的 INTEGER 是 64 位的，转成 JS number 后超过 2^53 会丢精度
 * - BLOB 以 base64 字符串返回；日期没有原生类型，不做自动转换
 */
export class Db extends Ipc {
	private static readonly domain = 'db';

	protected type = Db.domain;

	/** C++ 分配的连接 id；开发者不感知，所以是 private */
	private readonly id: number;

	private constructor(id: number) {
		super();
		this.id = id;
	}

	/**
	 * 打开（文件不存在则创建）一个数据库，返回实例。
	 *
	 * **是 async**：连接是 C++ 侧的资源，得等它建好才拿得到 id。
	 * 不传 name 就用默认库（应用数据目录下的 `db/webwin.db`）。
	 */
	static async open(name?: string): Promise<Db> {
		const id = await Db.request<number>(Db.domain, 'open', { name });
		return new Db(id);
	}

	/** 关闭连接。忘了调，C++ 里的 sqlite3* 会一直占着文件锁 */
	close(): Promise<void> {
		return this.call<void>('close', { id: this.id });
	}

	/**
	 * 执行一段不返回结果的 SQL，可以包含多条语句（建表、初始化用它）。
	 *
	 * **不支持参数绑定** —— 需要绑定就用 run / get / all
	 */
	exec(sql: string): Promise<void> {
		return this.call<void>('exec', { id: this.id, sql });
	}

	/** 执行写操作（INSERT / UPDATE / DELETE），返回影响行数和新 rowid */
	run(sql: string, params?: DbParams): Promise<DbRunResult> {
		return this.call<DbRunResult>('run', { id: this.id, sql, params });
	}

	/** 查一行；没查到返回 null（不报错） */
	get(sql: string, params?: DbParams): Promise<DbRow | null> {
		return this.call<DbRow | null>('get', { id: this.id, sql, params });
	}

	/** 查全部行 */
	all(sql: string, params?: DbParams): Promise<DbRow[]> {
		return this.call<DbRow[]>('all', { id: this.id, sql, params });
	}
}
