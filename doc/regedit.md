# 注册表 API（`regedit` 域）

`window.regedit` 是**全局单例**——和窗口无关（不像 `win` 一个窗口一个实例）。

键路径写成一整条，跟 reg.exe 一个写法：

```ts
await regedit.set('HKCU\\Software\\MyApp', 'greeting', 'hello');

const value = await regedit.get('HKCU\\Software\\MyApp', 'greeting');
// → { type: 'REG_SZ', data: 'hello' }
```

根键那一节的**简写与全名都认，不分大小写**：`HKCU` / `HKEY_CURRENT_USER`、
`HKLM` / `HKEY_LOCAL_MACHINE`、`HKCR` / `HKEY_CLASSES_ROOT`、
`HKU` / `HKEY_USERS`、`HKCC` / `HKEY_CURRENT_CONFIG`。

> 权限上要留意：**写 `HKLM` 需要管理员**，普通权限下会 reject；
> `HKCU` 是当前用户的，随便写。

## 方法

| 方法 | 返回 | 说明 |
| --- | --- | --- |
| `regedit.get(key, name?)` | `Promise<RegValue \| null>` | 读一个值；键或值不存在返回 `null` |
| `regedit.set(key, name, value)` | `Promise<void>` | 写一个值；键不存在时自动逐层创建 |
| `regedit.deleteKey(key)` | `Promise<void>` | 删掉整个键，连子键和值一起 |
| `regedit.listKeys(key)` | `Promise<string[]>` | 列下一层的子键名 |
| `regedit.listValues(key)` | `Promise<string[]>` | 列该键下的值名（不含值） |

`get` 的 `name` 不传（或给空串）就读这个键的**默认值** ——
也就是注册表编辑器里那个 "(默认)"。

`deleteKey` 是**递归**删：子键和值一起走，不做"只能删空键"的限制。
键本来就不在也不报错 —— 要的是"删完之后不存在"。

## 值类型

`get` 返回 `{ type, data }`，`data` 的形态跟着 `type` 走：

| `type` | `data` |
| --- | --- |
| `REG_SZ` / `REG_EXPAND_SZ` | `string` |
| `REG_MULTI_SZ` | `string[]` |
| `REG_DWORD` / `REG_QWORD` | `number` |
| `REG_BINARY` | base64 字符串 |

两点：

- `REG_EXPAND_SZ` 与 `REG_SZ` 是两个类型：前者里的 `%PATH%` 这类引用
  **留着不展开**（要展开得走系统 API），混着用会出问题，所以分开给
- `REG_QWORD` 超过 2^53 会丢精度 —— JS 的 number 是 double，
  跟 SQLite 的 INTEGER 一个毛病

`set` 按 value 的 JS 类型挑类型：字符串 → `REG_SZ`；数字 → `REG_DWORD`
（32 位放得下时）或 `REG_QWORD`。要写 `REG_EXPAND_SZ` / `REG_BINARY`
这类，等加了显式 type 选项再说。

## 错误处理

同其他域：C++ 返回字符串而不是 `Error` 对象。

```ts
try {
	const value = await regedit.get('HKCU\\Software\\MyApp', 'greeting');
} catch (e) {
	const msg = e && e.message ? e.message : e;
	console.error('读注册表失败：', msg);
}
```

"键或值不存在"**不**走这里 —— 它返回 `null`。

## 尚未支持

- 删单个值（`deleteValue`）
- 写默认值（`set` 的 name 给空串算"缺参数"，不是"写默认值"）
- 判断键 / 值是否存在（`has`）
- 显式指定值类型（`REG_EXPAND_SZ` / `REG_BINARY` / `REG_MULTI_SZ`）
- 32 / 64 位注册表视图（`KEY_WOW64_64KEY` / `KEY_WOW64_32KEY`）
- 递归枚举、监听注册表变化（`RegNotifyChangeKeyValue`）
