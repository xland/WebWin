# 系统 API（`os` 域）

`window.os` 是**全局单例**——和窗口无关（不像 `win` 一个窗口一个实例）。

```ts
const ver = await os.getOsVer();
const id  = await os.getCpuId();
```

## 硬件与系统信息

| 方法 | 返回 |
| --- | --- |
| `os.getOsVer()` | 系统版本（见下） |
| `os.getCpuId()` | 机器标识字符串 |
| `os.getDiskId()` | 系统盘序列号 |
| `os.getUserLang()` | **当前用户**的语言（区域名，如 `zh-CN`） |
| `os.getOsLang()` | **系统**的语言 |

`getOsVer()` 返回值：

```ts
{
  major: number;
  minor: number;
  build: number;
  /** 只有 Windows 10 / 11 才会给；更早的系统不猜，调用方自己看 build */
  versionName?: 'win10' | 'win11';
}
```

> `getUserLang` 与 `getOsLang` 在多用户机器上可能不同。

## 系统配色

`os.getOsColor()` 返回系统当前主题的配色项（`#RRGGBB`）：

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

## 网络

`os.getIpAddr()` 返回所有已启用网卡的地址：

```ts
{ name: string; mac: string; addresses: string[] }[]
```

已过滤掉未启用的网卡和回环地址（`127.0.0.1` / `::1`）。

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
