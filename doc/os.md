# 系统 API（`os` 域）

`window.os` 是**全局单例**——和窗口无关（不像 `win` 一个窗口一个实例）。

```ts
const ver = await os.getVersion();
const id  = await os.getCpuId();
```

## 硬件与系统信息

| 方法 | 返回 |
| --- | --- |
| `os.getVersion()` | 系统版本（见下） |
| `os.getCpuId()` | 机器标识字符串 |
| `os.getDiskId()` | 系统盘序列号 |
| `os.getLang(scope)` | 语言（区域名，如 `zh-CN`） |

`getLang(scope)` 的 `scope`：`'user'` = **当前用户**的语言，`'os'` = **系统**的语言。
两者在多用户机器上可能不同，所以这个参数不给默认值。

`getVersion()` 返回值：

```ts
{
  major: number;
  minor: number;
  build: number;
  /** 只有 Windows 10 / 11 才会给；更早的系统不猜，调用方自己看 build */
  versionName?: 'win10' | 'win11';
}
```

## 系统配色

`os.getColor()` 返回系统当前主题的配色项（`#RRGGBB`）：

```ts
{
  window:  string,  // 窗口背景
  button:  string,  // 按钮表面
  text:    string,  // 窗口文字
  caption: string,  // 标题栏文字
  border:  string,  // 活动边框
  desktop: string,  // 桌面背景
}
```

## 常用目录

`os.getPath(name)` 返回该目录的绝对路径（不带尾部分隔符）：

```ts
await os.getPath('downloads');  // "C:\Users\Alice\Downloads"
```

| `name` | 目录 |
| --- | --- |
| `home` | 用户主目录（`%USERPROFILE%`） |
| `appData` | 每用户的应用数据目录（`%APPDATA%`，**漫游**的那份，不是 Local） |
| `temp` | 临时目录 |
| `desktop` | 桌面 |
| `documents` | 我的文档 |
| `downloads` | 下载 |
| `music` | 音乐 |
| `pictures` | 图片 |
| `videos` | 视频 |
| `recent` | 最近使用的文件（仅 Windows） |

目录被用户改到别处时（比如在资源管理器里改过"下载"的位置），返回的是改后的位置。

## 阻止锁屏 / 休眠

```ts
await os.preventSleep();      // 阻止系统进入锁屏/休眠
await os.stopPreventSleep();  // 恢复
```

## 系统凭据管理器

```ts
await os.credWrite('MyApp:token', 'abc123');    // 写（key 已存在则覆盖）
const v = await os.credRead('MyApp:token');     // 读；没存过 → null
await os.credDelete('MyApp:token');             // 删（幂等）
const keys = await os.credList('MyApp');        // 列 key（前缀匹配）
```

| 方法 | 返回 | 说明 |
| --- | --- | --- |
| `os.credWrite(key, value)` | `Promise<void>` | 写一条；key 已存在则覆盖 |
| `os.credRead(key)` | `Promise<string \| null>` | 读；不存在返回 `null` |
| `os.credDelete(key)` | `Promise<void>` | 删；本来就没有也不报错 |
| `os.credList(prefix?)` | `Promise<string[]>` | 列出 key；不给 `prefix` 就列全部 |

四点：

- **值按 UTF-16 原样存**，所以任何 JS 字符串都能原样往返 ——
  转成 UTF-8 存的话，孤立的代理项（半个 emoji）会坏掉
- 凭据存在**本机**这一栏（`CRED_PERSIST_LOCAL_MACHINE`）：
  不随用户配置漫游到域里的其它机器，只对当前用户可见
- `credList` 的 `prefix` 是前缀匹配，末尾的 `*` 会自动补上
- `credList` **只给 key，不给值** —— 要值就 `credRead(key)` 逐个读，
  拿到的一定是最新的

> 凭据管理器是**系统级的共享空间**：git、远程桌面这些也在里面存东西。
> `credList` 不传 `prefix` 时会把它们一起列出来（只列 key，不含值）。

## 事件

```ts
const off = os.on('osLock', ({ locked }) => {
  console.log(locked ? '已锁' : '已解锁');
});
off();  // 取消订阅
```

| 事件 | 触发时机 | 载荷 |
| --- | --- | --- |
| `osLock` | 系统锁屏 / 解锁 | `{ locked: boolean }` |

## 错误处理

同其他域：C++ 返回字符串而不是 `Error` 对象。
