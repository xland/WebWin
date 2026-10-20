# res —— exe 资源与版本信息

读写 **exe 里的资源**，以及读 exe 文件的版本信息。

```ts
// 版本信息 —— exe 属性窗口「详细信息」那一栏
const v = await res.getInfo('D:\\dist\\MyApp.exe');
console.log(v.fileVersion, v.strings?.ProductName);

// 列出资源
const list = await res.list('D:\\dist\\MyApp.exe');
// → [{ type: 'RCDATA', name: 'webwin.js', size: 20789 }, ...]

// 增 / 查 / 存 / 删
await res.addText('D:\\dist\\MyApp.exe', 'app.json', '{"v":1}');
const text = await res.get('D:\\dist\\MyApp.exe', 'app.json');
await res.saveToFile('D:\\dist\\MyApp.exe', 'app.json', 'app.json.bak');
await res.remove('D:\\dist\\MyApp.exe', 'app.json');

// 一个文件整个塞进去（资源名默认取文件名）
await res.addFile('D:\\dist\\MyApp.exe', 'assets/logo.png');
```

## 为什么这样设计

**为什么每个方法都要带 exePath。** 资源操作的写方法改不了"正在运行的这个 exe"
（下一条），所以这里没有"当前 exe"这个概念 —— 操作目标必须由调用方显式指定。
这样语义也直白：你永远知道自己改的是谁。

**为什么全部异步。** 资源可能几 MB，而写资源要**重写整个 PE 文件**
（`BeginUpdateResource` 会把整份资源表重建再写回磁盘）。跑在 UI 线程上窗口直接僵住，
所以一律和 `fs` 域一样丢到后台线程。

**为什么读别的 exe 用 `LoadLibraryEx` 而不是直接 `FindResource(nullptr, ...)`。**
后者只认自己进程的模块，读不了别的 exe。所以统一走
`LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE` —— 只当数据文件映射，
既不执行 `DllMain`，也让 `FindResource` / `EnumResource*` 正常工作。

**为什么写完必须 `EndUpdateResource(FALSE)`，失败时必须 `EndUpdateResource(TRUE)`。**
那个 `FALSE` / `TRUE` 是"提交"和"丢弃"。中途 `UpdateResource` 失败却不丢弃的话，
磁盘上那个 PE 会停在半改的状态。

## 方法

| 方法 | 返回 | 说明 |
| --- | --- | --- |
| `res.getInfo(exePath)` | `ResInfo` | 版本号 + `StringFileInfo` 字符串表 |
| `res.list(exePath, options?)` | `ResListResult` | 列资源（类型 / 名字 / 字节数） |
| `res.get(exePath, name, options?)` | `string` \| `ResBinary` | 读一个资源 |
| `res.addFile(exePath, filePath, options?)` | — | 把一个文件整个塞进去 |
| `res.addText(exePath, name, text, options?)` | — | 把一段文本塞进去 |
| `res.saveToFile(exePath, name, destPath, options?)` | — | 把资源原样导出成文件 |
| `res.remove(exePath, name, options?)` | — | 删一个资源 |

`options` 只有一个字段 `type`（资源类型，默认 `RCDATA`）。`get` 额外有 `encoding`。

## 改不了正在运行的 exe

这是本域最要紧的一条限制。Windows 以映像方式映射运行中的可执行文件，那个文件句柄
只声明了 `FILE_SHARE_READ | FILE_SHARE_DELETE`，而 `BeginUpdateResource` 内部要
`GENERIC_WRITE` 打开 —— 必然撞 `ERROR_SHARING_VIOLATION`。

所以 `addFile` / `addText` / `remove` 的 `exePath` 必须指向**别的**文件：

- 安装包、待分发的目标程序 —— 这是最主要的用途
- 或者先 `fs.copyFile` 复制一份出来，改完再替换（替换运行中的 exe 要重启后才生效）

碰到目标是自身时，C++ 侧会**提前**拦下并给出能看懂的错误，而不是甩一个
sharing violation 让你猜：

```
不能修改正在运行的这个 exe 自身：Windows 以映像方式映射它，
BeginUpdateResource 打开写入会被拒（ERROR_SHARING_VIOLATION）。先复制一份到别的路径再改。
```

四个读方法（`get` / `saveToFile` / `list` / `getInfo`）不受此限 —— 读不写，
指向自己没问题。本工程 `src/Res.rc` 里嵌的 `webwin.js`、`default.html`
就是现成的例子：`res.get(自己, 'webwin.js')` 能直接读出整个 SDK。

## 资源类型

默认 `RCDATA`。传字符串类型名可以换：

```ts
await res.list(exe, { type: 'ICON' });     // 标准类型 → 映射到 RT_*
await res.list(exe, { type: 'MyAppCfg' }); // 自定义类型 → 原样当字符串传
```

认识的标准类型名：`CURSOR` `BITMAP` `ICON` `MENU` `DIALOG` `STRING` `FONTDIR`
`FONT` `ACCELERATOR` `RCDATA` `MESSAGETABLE` `GROUP_CURSOR` `GROUP_ICON`
`VERSION` `PLUGPLAY` `VXD` `ANICURSOR` `ANIICON` `HTML` `MANIFEST`。

**不认识的名字不当错误** —— PE 资源表本来就允许任意字符串类型名，原样传下去，
这样"自定义类型 + 对应资源编辑器"的工作流也能走。

`list` 的 `type` 是可选的：**不传列全部类型**，传了只列那一类。这点和本域其他方法
不同（它们默认 RCDATA），因为"列全部"才是列表的默认预期。

## 编码

`get` 的 `encoding`，跟 `fs.readFile` 一致：

- `utf8`（默认）当文本读。**内容不是合法 UTF-8 时会出乱码**
- `base64` 二进制无损，代价是字符串体积涨 1/3
- `binary` 字节走共享内存，返回 `ArrayBuffer`。零拷贝、无膨胀，
  但**用完必须 `release()`**，且 buffer 只读

`addText` 的 `encoding` 只有 `utf8` 和 `base64`（要塞二进制就用 `addFile`
或 `base64`）。

## 路径

`exePath` / `filePath` / `destPath` 的相对路径一律以 package.json 所在目录为底，
跟 `fs` 域和 `path.resolve()` 一致。

`saveToFile` 和 `fs.writeFile` 一样**不自动建父目录** —— 静默建目录会把拼错的路径
变成一堆垃圾目录。

## 错误处理

失败一律 reject。常见几类：

| 错误 | 含义 |
| --- | --- |
| 不能修改正在运行的这个 exe 自身… | 写操作指向了自己，见上面那一节 |
| 目标 exe 不存在或不是文件 | 路径不对 |
| 找不到资源（名字或资源类型不对…） | 名字 / type 不对。**先用 `list` 看看实际有哪些** |
| BeginUpdateResource 失败：…（目标正被占用） | 目标正在运行，或被别的文件占用 |
| 资源名超过 255 个字符 | PE 资源表的名字上限 |

## 已知限制

- **改资源会破坏 Authenticode 数字签名。** 资源表被重写，签名自然失效。
  签名过的 exe 要改资源，得改完重新签名。
- **改不来正在运行的 exe**（见上）。
- 资源名超过 255 字符会直接 reject —— PE 资源表的硬上限。
- 本域只读 / 写**标准语言和子语言**（`LANG_NEUTRAL, SUBLANG_NEUTRAL`）。
  多语言资源（同一资源名多个语言版本）只能碰到中性那一份。
- 枚举出来的资源类型名，标准类型是 `RCDATA` 这类大写名，
  非标准的给数字字符串（如 `24`）。

## 实现说明

- C++ 侧 `src/Res.cpp`，域类型 `res`，在 `Page::handleMessage` 的 if/else 链上分发
- 写：`BeginUpdateResourceW` → `UpdateResourceW` → `EndUpdateResourceW(FALSE)`；
  删除就是 `UpdateResourceW(..., nullptr, 0)`
- 读：`LoadLibraryExW(LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)`
  → `FindResourceW` / `LoadResource` / `LockResource` / `SizeofResource`，用完 `FreeLibrary`
- 枚举：`EnumResourceTypesW` → `EnumResourceNamesW`
- 版本：`GetFileVersionInfoSizeW` / `GetFileVersionInfoW` / `VerQueryValueW`
  （语言块由 `VarFileInfo\Translation` 现查，不写死）
- 全部走 `Async::run`，后台线程干活、UI 线程回包；`binary` 走
  `ICoreWebView2SharedBuffer`，字节不进 JSON
- 相对路径基准 `App::getPackageBaseDir()`，与 `fs` 域一致
