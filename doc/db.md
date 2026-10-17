# 本地数据库 API（`db` 域）

`Db` 是**类型**（`window.Db`），不是实例。数据库要显式打开：

```ts
const db = await Db.open('app.db');
await db.exec('CREATE TABLE IF NOT EXISTS t (id INTEGER PRIMARY KEY, v TEXT)');
await db.run('INSERT INTO t (v) VALUES (?)', ['hello']);
const rows = await db.all('SELECT * FROM t');
await db.close();
```

`Db.open(...)` 是 async：连接是 C++ 里的 `sqlite3*`，得等它建好。

**一个 `Db` 实例 = 一个连接。** 开发者**不感知连接 id** —— 它在实例内部，
每次调用由实例自己带上，跟 `Win` / `Menu` 一个路子。

## 方法

| 方法 | 返回 | 说明 |
| --- | --- | --- |
| `Db.open(name?)` | `Promise<Db>` | 建连接（async） |
| `db.exec(sql)` | `Promise<void>` | 多条语句，建表 / 初始化用；**不收参数** |
| `db.run(sql, params?)` | `Promise<{changes, lastInsertRowid}>` | INSERT / UPDATE / DELETE |
| `db.get(sql, params?)` | `Promise<object \| null>` | 单行；没查到返回 `null` |
| `db.all(sql, params?)` | `Promise<object[]>` | 全部行 |
| `db.close()` | `Promise<void>` | 关连接；**忘调会一直占着文件锁** |

`get` 没查到时返回 `null` 而不是报错 —— 查不到是正常结果。

## 库文件放哪

| 传什么 | 落在哪 |
| --- | --- |
| 不传 | 默认库：应用数据目录下的 `db/webwin.db` |
| 只给文件名（不含路径分隔符） | 应用数据目录下的 `db/<name>` |
| 完整路径 | 按你给的用 |

多垫一层 `db/` 是因为**应用数据目录同时是 WebView2 的 profile 目录**
（Cookie / LocalStorage / IndexedDB 都在那儿），不隔开就跟浏览器数据混在一起了。

## 参数绑定

一律走绑定，**不要自己拼 SQL 字符串**。两种都收：

```ts
await db.run('INSERT INTO t (a, b) VALUES (?, ?)', ['x', 1]);  // 数组 → 按位置
await db.all('SELECT * FROM t WHERE id = :id', { id: 1 });     // 对象 → 按名字
```

具名参数**三种前缀都认**：SQL 里写 `:name` / `@name` / `$name` 都能对上，
调用方不用关心 SQL 用的是哪一种。

`exec` 不支持绑定（它走 `sqlite3_exec`，可以一次跑多条语句），
需要绑定就用 `run` / `get` / `all`。

## 类型映射

| SQLite | → JS | 说明 |
| --- | --- | --- |
| INTEGER | `number` | **64 位**：超过 2^53 会丢精度 |
| REAL | `number` | |
| TEXT | `string` | |
| BLOB | `string` | base64 编码 |
| NULL | `null` | 不是 `undefined` |

两个取舍：

- **整数精度**：JS 的 number 是 double，SQLite 的 INTEGER 是 64 位的，
  超过 2^53 会**静默**丢精度。目前一律转 number；
  真有场景用得到大整数，再考虑给 `Db.open()` 加开关。
- **日期**没有原生类型，不做自动转换 —— 存 epoch 整数还是 ISO 字符串，
  交给调用方自己定。

## 事务

`exec` 能跑多条语句，所以事务直接用 SQL 写：

```ts
await db.exec('BEGIN');
try {
  await db.run('UPDATE account SET bal = bal - ? WHERE id = ?', [100, 1]);
  await db.run('UPDATE account SET bal = bal + ? WHERE id = ?', [100, 2]);
  await db.exec('COMMIT');
} catch (e) {
  await db.exec('ROLLBACK');
  throw e;
}
```

**一个坑**：每次调用都是一次异步往返，`BEGIN` 和 `COMMIT` 之间
**会漏进别的操作**。事务期间别并发操作同一个连接。

（将来会提供 `execBatchByTrans` 之类的批量接口，把整段包在一次往返里，
这个坑也就没了。）

## 生命周期

连接**属于创建它的那个页面**。窗口关掉 / iframe 销毁时，C++ 会自动关掉它开的连接。

但**还是建议显式 `close()`** —— 自动清理是兜底，不是让你不关。
多窗口各开一个连接、都不关，文件锁会一直握着。

## 安全

`open` 能指定路径，等于给了页面读写真实文件的能力。所以额外加了两道：

- `SQLITE_DBCONFIG_DEFENSIVE`：禁止直接写 `sqlite_master` 这类系统表
- **禁用 `ATTACH`**：否则任意 SQL 能把磁盘上别的库挂进来，
  `open` 那边的路径约定就形同虚设

```sql
ATTACH DATABASE 'C:\...\secret.db' AS x;  -- 会被拒
```

只限制 `open` 的路径是不够的 —— 只要还能执行任意 SQL，就总有办法绕出去。

## 错误处理

同其他域：C++ 返回字符串而不是 `Error` 对象。错误里带 SQLite 的 `errmsg`，
不是光秃秃的错误码。

```ts
try {
  await db.exec('SELEC 1');   // 故意写错
} catch (e) {
  const msg = e && e.message ? e.message : e;
  console.error('执行失败：', msg);
}
```

## 性能

查询**同步跑在 UI 线程**（跟 dialog / menu 一致的取舍）。
本地库绝大多数场景没问题，但一条没走索引的全表扫描会把界面卡住，
**包括 WebView2 的渲染**。有大查询的话先给表加索引。

## 尚未支持

- 批量事务接口（`execBatchByTrans`）—— 目前用 `exec('BEGIN')` 手写
- 后台线程执行 —— 目前同步跑在 UI 线程
- 预编译语句复用、自定义 SQL 函数
