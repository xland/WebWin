# 子进程 API（`process` 域）

`Process` 是**类型**（`window.Process`），不是实例。用静态方法 `Process.spawn()` 启动子进程，
返回的是一个 **Process 实例** —— 也就是一个"进程对象"：

```ts
const proc = await Process.spawn('ping', ['127.0.0.1', '-n', '3']);

proc.on('stdout', (p) => console.log(p.text));
proc.on('stderr', (p) => console.error(p.text));
proc.on('exit',   (p) => console.log('退出码', p.code));

console.log('pid =', proc.pid);
```

`spawn` 是 **async**：进程是 C++ 侧的 Win32 对象，得等它建好才拿得到 id。

> 这个 API 等于"在用户机器上执行任意程序" ——
> 只能用在**你自己完全信任**的内容上，见 `../README.md` 的安全口径。

## 方法

| 方法 | 返回 | 说明 |
| --- | --- | --- |
| `Process.spawn(file, args?, options?)` | `Promise<Process>` | 启动子进程（async） |
| `Process.getArg()` | `Promise<string[]>` | 取**当前应用**的命令行参数数组（首元素是 exe 路径） |
| `proc.on(name, fn)` | `() => void` | 订阅事件，返回退订函数 |
| `proc.off(name, fn)` | `void` | 取消订阅 |

## 参数

```ts
interface SpawnOptions {
	/** 子进程的工作目录；不给就用应用基准目录（package.json 所在目录） */
	cwd?: string;
	/** 是否**隐藏**控制台窗口，默认 true。想让用户看见就传 false */
	hide?: boolean;
	/** 脱离页面生命周期：页面销毁时不杀它，默认 false */
	detached?: boolean;
}
```

`file` 可以是完整路径，也可以是不带路径的程序名 —— 后者按 Win32 的惯例去 PATH 里找。
`args` 是参数数组；引号由 C++ 侧按规则加好，不用自己管。

## 属性

| 属性 | 类型 | 说明 |
| --- | --- | --- |
| `pid` | `number` | 系统进程 id |

## 命令行参数（getArg）

取**当前应用**的命令行参数数组，与 Node 的 `process.argv` 一致：
第一个元素是 exe 完整路径，其后才是真正的参数。

```ts
const args = await Process.getArg();
console.log(args[0]);        // exe 完整路径
console.log(args.slice(1));  // 真正的参数
```

框架自身注入的 `--path=...` 等启动参数也会如实包含在内。
用 Windows 自带解析器（`CommandLineToArgvW`）解析，正确还原引号与转义。

## 事件

| 事件 | 载荷 | 触发 |
| --- | --- | --- |
| `stdout` | `{ text }` | 子进程往标准输出写了一段 |
| `stderr` | `{ text }` | 子进程往标准错误写了一段 |
| `exit` | `{ code }` | 子进程退出，带退出码 |

三点要留意：

1. **事件只推给发起 spawn 的那个文档**（顶级文档或那个 iframe）。
   同时开着好几个子进程时各收各的，互不干扰。
2. **监听要紧跟在 await 后面同步挂上。** 子进程一建好，C++ 那边管道就开始读了，
   中间再去 await 别的东西，开头几行输出就漏了。
3. **输出是分段的**：一段可能是一行、半行、或几十行，取决于子进程怎么缓冲。
   想按行处理就自己在 JS 侧攒着切。

`exit` 到达时，之前的输出**保证已经全部推到了**（管道在进程退出时就到头，
C++ 侧会等两条管道读完再发 exit）。

## 输出解码

按 **UTF-8** 解；解不动就退回系统 ANSI 代码页（中文 Windows 上是 GBK）——
`cmd.exe` 这类老程序用的是控制台代码页，不是 UTF-8。

想要原样字节就换条路：让子进程把输出写成文件，再用
`fs.readFile(path, 'binary')` 读回来（见 [文件 API](fs.md)）。

## 生命周期

**页面被销毁时，它启动的、没标 `detached` 的子进程会被杀掉。**
谁启动谁负责 —— 切到别的演示页、或关掉窗口，子进程一起结束，不留孤儿进程。

传 `detached: true` 就不杀：想让子进程在页面没了之后继续跑用它。

## 错误处理

启动失败（找不到文件、工作目录不存在、权限不够 …）会 **reject**。
子进程**运行**失败不在这里体现 —— 看 `exit` 事件的退出码。

```ts
try {
	const proc = await Process.spawn('不存在的程序.exe');
} catch (e) {
	const msg = e && e.message ? e.message : e;
	console.error('启动失败：', msg);
}
```

## 尚未支持

- `proc.kill()` —— 现在只能靠页面销毁来结束子进程
- `stdin` / `proc.write()` —— 目前子进程的 stdin 是空设备（NUL）
- `env` 选项 —— 子进程一律继承当前进程的环境变量
- 一次性拿全部输出的 `exec` 形态
