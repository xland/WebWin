# 路径 API（`path`）

`path` 是全局对象（`window.path`），做**路径字符串**的运算。

```ts
path.join(path.getBase(), 'data', 'app.db'); // '<基准目录>\data\app.db'
path.dirname('C:\\a\\b.txt');                // 'C:\\a'
path.extname('C:\\a\\b.txt');                // '.txt'
path.parse('C:\\a\\b.txt').name;             // 'b'
path.relative('C:\\a\\b', 'C:\\a\\c');       // '..\\c'
```

## 三件要先知道的事

**1. 它是同步的 —— 整个 SDK 里唯一的一个。**

其余能力都要过消息通道到 C++，所以全是 `async`。`path` 是纯字符串运算，
一次 IPC 都不发，**直接返回值，不用 `await`**：

```ts
const p = path.join('a', 'b');   // ✅ 直接就是字符串
const p = await path.join(...);  // ❌ 多此一举（await 一个非 Promise 也能拿到值，但写法误导）
```

**2. 只支持 Windows，语义对齐 Node 的 `path.win32`。**

输入同时接受 `/` 和 `\`，输出统一 `\`。没有 `path.posix`，也没有
`path.win32` 这层自引用（就一个平台，不需要）。

**3. `path` 不碰文件系统。**

它只做字符串运算：**不检查路径存不存在、不读写任何东西**。
要读写文件是别的能力的事。

## 属性

| 属性 | 值 | 说明 |
| --- | --- | --- |
| `path.sep` | `'\\'` | 路径分隔符 |
| `path.delimiter` | `';'` | PATH 那种环境变量里分隔多个路径用的符号 |

## 方法

| 方法 | 返回 | 说明 |
| --- | --- | --- |
| `join(...parts)` | `string` | 拼接后规范化 |
| `resolve(...parts)` | `string` | 从右往左找第一个绝对路径；全是相对时以基准目录为底 |
| `normalize(p)` | `string` | 消掉 `.` / `..` / 重复分隔符，斜杠统一成 `\` |
| `dirname(p)` | `string` | 所在目录 |
| `basename(p, suffix?)` | `string` | 最后一段；给了 `suffix` 且正好对上就去掉 |
| `extname(p)` | `string` | 扩展名（含前导点）；没有则 `''` |
| `parse(p)` | `ParsedPath` | 拆成 `{ root, dir, base, ext, name }` |
| `format(obj)` | `string` | `parse` 的逆运算；给了 `base` 时忽略 `name` / `ext` |
| `isAbsolute(p)` | `boolean` | 是否绝对路径 |
| `relative(from, to)` | `string` | `from` 到 `to` 的相对路径 |
| `getBase()` | `string` | `resolve` / `relative` 用的基准目录 |

## 基准目录：取代 `process.cwd()`

Node 的 `path.resolve()` 用 `process.cwd()` 当基准。**本框架没有 cwd 概念**
（全库不调用 `GetCurrentDirectory`），改以**应用基准目录**为底 —— 也就是
`package.json` 所在的目录（命令行 `--path=...` 指定过就是那个）。启动时由 C++
注入，所以 `getBase()` 是同步的。

```ts
path.resolve('data', 'a.db');  // '<基准目录>\data\a.db'
path.resolve('C:\\x', 'y');    // 'C:\x\y' —— 有绝对路径就用它，不看基准
```

**为什么不用 cwd**：桌面应用的 cwd 取决于启动方式 —— 双击、快捷方式、
命令行各不相同，`resolve('data/a.db')` 落在哪儿不可预测。基准目录则始终
指向应用自己那一摊。

`relative()` 同理：给相对路径时两边都先按基准目录展开。

## Windows 专属校验（Node 没有）

Node 不管这些，但桌面应用建文件时很实用，而且纯字符串就能查：

```ts
path.isValidName('a.txt');   // true
path.isValidName('NUL');     // false —— 保留设备名
path.isValidName('nul.txt'); // false —— 带扩展名也指向设备
path.isValidName('a<b');     // false —— 非法字符
path.isValidName('a.');      // false —— 不能以点或空格结尾
path.isValidName('a ');      // false

path.isValidPath('C:\\a\\b.txt'); // true（盘符 / UNC 根 / `\\?\` 前缀都认）
```

| 方法 | 说明 |
| --- | --- |
| `isValidName(name)` | 单个文件/目录名是否可用。查非法字符（`<>:"\|?*`、两个分隔符、0-31 控制字符）、保留设备名（`CON` / `PRN` / `AUX` / `NUL` / `COM1-9` / `LPT1-9`，大小写不敏感）、以及是否以点或空格结尾 |
| `isValidPath(path)` | 整条路径。按分隔符切段逐段用 `isValidName()` 查，`.` / `..` 跳过 |

> **保留设备名这条最容易被忽略**：`NUL.txt`、`COM1.log` 在 Windows 上**不是文件**，
> 读写它们会打到设备上去。系统还会自动去掉名字末尾的点和空格 ——
> 存 `a.` 出来的是 `a`，跟你要的不一样，所以也判非法。

## 错误处理

参数不是字符串时**抛 `TypeError`**，跟 Node 一致：

```ts
path.join('a', 42); // TypeError: path：参数 "path" 必须是字符串，收到 number
```

同步抛出，用 `try / catch` 接：

```ts
try {
	const p = path.join(userInput);
} catch (e) {
	console.error('路径不合法：', e.message);
}
```

## 实现说明

`normalize()` / `join()` 里有些看着绕的分支，别当冗余删掉：

- 前半段处理 `\\.\` / `\\?\` 设备根（`\\.\PIPE` 这种）和 `NUL:` 这类保留设备名
- 末尾那个 `.\\` 前缀是修 **CVE-2024-36139** 的：相对路径里冒出 `X:` 时，
  Windows 可能把它当绝对路径解释，补一个 `.\` 锁死成相对路径

改动后请跑一遍差分测试：把 `Path.ts` 用 esbuild 打成 CJS，跟 Node 的
`path.win32` 逐条对比（固定语料 + 随机畸形路径各跑一轮）。

## 尚未支持

- `path.matchesGlob()` —— Node 22 才加，glob 本身是一大块面
- `path.toNamespacedPath()` —— `\\?\` 前缀转换，给系统 API 用的内部玩意儿
- 大小写折叠：`C:\a` 和 `c:\A` 在 Windows 上是同一个文件，但 `path` 不做归一化，
  比较前自己 `toLowerCase()`
