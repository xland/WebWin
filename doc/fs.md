# 文件 API（`fs`）

`fs` 是**全局单例** `window.fs`，对标 Node 的 fsPromises —— **全部异步，没有同步版本**。

```ts
await fs.writeFile('notes.txt', 'hello');
const text = await fs.readFile('notes.txt');
```

## 为什么没有同步 API

框架里没有同步 API。而且文件操作看着快，实际可能很慢：网络盘（UNC）、
被杀毒软件盯上的写入、几百 MB 的读取，都能卡住几秒。所以每个方法都跑在
**后台线程**上，不会把窗口冻住（线程机制见文末"实现说明"）。

## 方法

| 方法 | 返回 | 说明 |
| --- | --- | --- |
| `fs.readFile(path, options?)` | `Promise<string>` / `Promise<FsBinary>` | 读整个文件（支持分块流式，见下） |
| `fs.readFileChunk(path, options?, start?, size?)` | `Promise<string>` / `Promise<FsBinary>` | 读一段（随机访问） |
| `fs.writeFile(path, data, options?)` | `Promise<void>` | 写文件（覆盖） |
| `fs.appendFile(path, data, options?)` | `Promise<void>` | 追加到末尾 |
| `fs.unlink(path)` | `Promise<void>` | 删文件 |
| `fs.copyFile(src, dest)` | `Promise<void>` | 复制文件 |
| `fs.rename(oldPath, newPath)` | `Promise<void>` | 移动 / 改名 |
| `fs.stat(path)` | `Promise<FsStats>` | 取文件信息 |
| `fs.access(path, mode?)` | `Promise<void>` | 检查能不能访问 |

**只管文件**：目录的创建 / 遍历 / 删除另归一处（`arch/01-api.md` 第 12 项仍是待办），
所以这里没有 `mkdir` / `readdir` / `rmdir`。

## 读文件：三种编码

`readFile` 的 `encoding` 决定拿回什么：

| encoding | 返回 | 适用 |
| --- | --- | --- |
| `utf8`（默认） | 字符串 | 配置文件、JSON、日志 |
| `base64` | 字符串 | 二进制内容无损，体积涨 1/3 |
| `binary` | `FsBinary`（带 `ArrayBuffer`） | 大文件、零拷贝 |

```ts
const text = await fs.readFile('config.json');          // 当文本读
const b64  = await fs.readFile('logo.png', 'base64');   // 图片、zip 无损
const bin  = await fs.readFile('big.zip', 'binary');    // ArrayBuffer
bin.release();                                          // 用完必须还
```

> `utf8` 对**非 UTF-8 的内容会出乱码**（GBK 文本之类）。要原样字节就用 `base64` 或 `binary`。

`binary` 走共享内存，不进 JSON，所以没有 base64 那 1/3 膨胀。代价是
**用完必须 `release()`** —— 不调会一直占着共享内存。`buffer` 是**只读**的，
往里写会让渲染进程崩。

## 分块读取（流式）

大文件不必一次性读进内存：`readFile` 传入 `callback` 就进入分块模式，**每读一块触发一次
回调**，适合边读边处理（比如边读边上传、边解析）。`chunkSize` 控制块大小（字节），默认 1MB。

```ts
// 分块：每 1MB 触发一次回调
await fs.readFile('big.zip', { encoding: 'base64', chunkSize: 1024 * 1024 }, (c) => {
  console.log('第', c.index, '块，是否最后', c.isLast, '内容长', c.data.length);
});
```

`callback` 收到的是 `FsChunk = { data: string; index: number; isLast: boolean }`：

- `data` 按 `encoding` 给（`utf8` 文本 / `base64` 字节），与一次性读取一致；
- `index` 从 0 起；`isLast` 标最后一块。`readFile` 整体完成后 resolve 的不再是文件内容，
  而是 `{ totalBytes, totalChunks }`（`FsReadResult`）。

几个要点：

- **JS 端是真正增量的**：块内容逐块交给 callback，不会一次性塞进内存。
- **utf8 切块做了边界对齐**：若块尾巴卡在某个多字节字符中间，会把那个字符整体留到下一块，
  避免乱码；`base64` 不受影响。
- **二进制分块降级为 base64**：分块走的是普通消息通道，拿不到 `binary` 的 `ArrayBuffer`，
  所以 `encoding: 'binary' + callback` 时 `data` 是 base64 串；要真正的 `ArrayBuffer` 用
  一次性 `fs.readFile(path, 'binary')` 或下面的 `readFileChunk`。

> 分块走的是 `fs` 内部的隐藏事件（`fs.on('fs-chunk')` 由 `readFile` 自动订阅、按 `streamId`
> 路由），**没有改动 Ipc 的请求-响应协议**——这是刻意让 `fs` 自包含、不动核心通道。

## 读一段：随机访问

`readFileChunk` 从 `start` 偏移读 `size` 字节，返回**单块**内容，适合大文件的随机访问
（不必读进全文）。`start` / `size` 按 32 位整数（≤2GB）。

```ts
const head = await fs.readFileChunk('big.bin', 'binary', 0, 256); // 前 256 字节
head.release();
const tail = await fs.readFileChunk('big.bin', 'utf8', 1024, 512); // 中段 512 字节当文本
```

## 写文件

```ts
await fs.writeFile('config.json', JSON.stringify(cfg), { encoding: 'utf8' });
```

`encoding: 'base64'` 时会把 `data` 当 base64 解成字节再写 ——
配合 `readFile(..., 'base64')` 可以无损搬运二进制。

**不会自动建父目录**（Node 的 `writeFile` 也不建）。静默建目录会把拼错的路径
变成一堆垃圾目录，所以目录不存在时直接报错。

## stat()

`fs.stat(path)` 取文件信息（大小、类型、时间等），返回**纯数据** `FsStats`，
不像 Node 的 `Stats` 那样带 `isFile()` 方法 —— 那些方法过不了消息通道。

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `size` | `number` | 字节数 |
| `isFile` / `isDirectory` / `isSymbolicLink` | `boolean` | 类型 |
| `readonly` / `hidden` | `boolean` | Windows 属性 |
| `mtimeMs` / `atimeMs` / `birthtimeMs` | `number` | 修改 / 访问 / 创建时间，毫秒 |
| `mode` | `number` | 权限位，**近似值** |

两个和 Node 不一样的地方，都是 Windows 没有对应概念导致的：

- **没有 `ctimeMs`**。Windows 没有 POSIX 的 inode 变更时间，与其给个假数字不如不给。
- **`mode` 是近似的**：只读给 `0o444`，否则给 `0o666`，**不代表真实 ACL**。

## `access()` 与权限位

```ts
await fs.access(p);                                              // 默认只查存在
await fs.access(p, fs.constants.R_OK | fs.constants.W_OK);       // 读写都要能
await fs.access(p, fs.constants.W_OK);                           // 只要能写
```

`fs.constants` 跟 Node 一致：`F_OK: 0` / `X_OK: 1` / `W_OK: 2` / `R_OK: 4`。

权限检查是**近似**的：

- `R_OK` 会真的开一次句柄确认能读（只读属性挡的是写，挡不住读）
- `W_OK` 只看只读属性，**不查 ACL**
- `X_OK` 在 Windows 上没有对应概念，能取到属性就算通过

## 路径基准

**相对路径一律以 package.json 所在目录为底**，跟 `path.resolve()` 一致。

不能用"进程当前目录"当基准 —— GUI 进程本来就没有有意义的 cwd（框架全库不调
`GetCurrentDirectory`），而且 **Windows 的文件对话框还会悄悄改掉进程的当前目录**，
同一个相对路径在弹过一次文件选择框后可能指向别处。想可预测就写全盘符：

```ts
await fs.readFile('data/a.json');            // → <应用目录>/data/a.json
await fs.readFile('C:\\Temp\\a.json');       // 绝对路径，原样用
```

> `\a` 这种只有根分隔符、没有盘符的路径仍依赖"当前盘"（Win32 的既有行为），不推荐。

## 错误处理

**失败一律 reject**，没有回调式的 `err` 参数。常见的失败原因：

| 情况 | 结果 |
| --- | --- |
| 文件不存在（读 / 删 / 改名 / stat） | reject |
| 没权限、文件被别的程序占用 | reject |
| 所在目录不存在（写） | reject |
| `data` 不是合法 base64 | reject |

错误信息里带上了 Win32 错误码（比如"拒绝访问（错误码 5）"），方便排查。

## 已知限制（v1）

都是"这次没做"，不是 bug：

- **只管文件**，没有 mkdir / readdir / rmdir / rm
- **没有流式写入**：`writeFile` 一次写全部（读取侧已支持分块流式，见上文）
- **没有打开 / 关闭句柄**（没有 `FileHandle`、没有 `open()`），也就没有随机读写、没有文件锁
- **没有 `watch()`**：文件变化监听待做
- **`mode` 和 `access` 的权限检查是近似的**，不查 ACL（见上文）
- `size` 走 JSON 数字，超过 2^53 会掉精度（8PB 以上才碰到）
- **`binary` 的 buffer 只读**，且必须手动 `release()`

## 尚未支持

- 目录操作：创建 / 遍历 / 删除（`arch/01-api.md` 第 12 项）
- 流式写入、文件句柄（`open()` / `FileHandle`）、随机读写（读取侧靠 `readFileChunk`，写入侧尚无）
- 文件变化监听（`watch()`）
- 真实 ACL 权限检查
- 符号链接的创建与解析（现在只能靠 `stat` 认出它是链接）

## 实现说明

所有方法跑在**后台线程**上。这不是可选项 —— 网络盘和大文件会卡住 UI 线程：

```
handleApi（UI 线程）→ Async::run 登记 → co_await resume_background()（WinRT 线程池）
                                          ↓
                                        文件操作
                                          ↓
                  co_await resume_foreground(App::dispatcherQueue())
                                          ↓
                                        回到 UI 线程 → page->send
```

去程用 **WinRT 线程池**（线程复用，不必每个操作开一条 OS 线程）；回程**明确指名**
UI 线程的 `DispatcherQueue`，不用 `winrt::apartment_context` —— 后者靠 C++/WinRT
的单元记账，而本进程是用 `CoInitializeEx` 起 COM 的，没走 `winrt::init_apartment`，
那条路能不能正确回到本线程没有保证。

线程、回包、以及"页面还在不在"的判定统一由 `Async` 管（`src/Async.h`），
**Http 的请求也走同一条路**。两个约束都不能反：

1. 文件操作不能在 UI 线程上跑（会卡死窗口）；
2. `page->send` 又必须在 UI 线程上跑（WebView2 的要求）。

而"页面还在不在"这件事**必须在 UI 线程上查**：页面销毁（`WM_DESTROY`
→ `Async::removeByPage`）同样发生在 UI 线程，两者互斥。放在后台线程里查，
就会有"刚查完、还没发，窗口没了"的窗口期。

`readFile` 的 `binary` 走共享内存（`Page::postSharedBuffer`，跟 `display.getImage`
一条路），先 Post 再 Close。
