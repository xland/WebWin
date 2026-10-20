# API 能力清单

框架对外以 JS API 形态提供能力（经 `02-architecture.md` 所述的消息通道调用）。
本清单是框架的能力边界，**会持续扩充**。

## 基础能力（SDK 提供）

| # | 能力 | 备注 |
| --- | --- | --- |
| 1 | 文件读写、删除、创建 | **已落地**：`fs.readFile()` / `writeFile()` / `appendFile()` / `unlink()` / `copyFile()` / `rename()` / `stat()` / `access()`，**全部异步**（跑在后台线程），相对路径以应用基准目录为底（见 `../doc/fs.md`）。**只管文件** —— 目录操作见第 12 项；流式读写、文件句柄、`watch()` 仍待做 |
| 2 | 剪贴板访问 | |
| 3 | 托盘图标 | **已落地**：`Tray.create({ tooltip?, menu? })` + `setToolTip()` / `destroy()` + `click` / `rightClick` / `doubleClick` 事件（见 `../doc/tray.md`）。图标一律用 exe 自己的（自定义图标待做，T013）；**有托盘图标时最后一个窗口关掉不退出应用** |
| 4 | 应用程序多开通知机制 | **应用被多开时**（第二个实例启动）通知已有实例。第二个实例自身的处置（退出 / 继续运行）待定 |
| 5 | 使用系统对话框 | **已落地**：`dialog.openPath()` / `savePath()` / `msgBox()` / `itemInFolder()`（见 `../doc/dialog.md`）。对话框模态到当前窗口 |
| 6 | 阻止应用锁屏 | 阻止系统进入锁屏/休眠。**已落地**：`os.preventSleep()` / `os.stopPreventSleep()` |
| 7 | 访问系统进程 | |
| 8 | 启动子进程（与子进程通信） | **部分落地**：`Process.spawn(file, args?, options?)` 启动子进程，返回进程对象；stdout / stderr / exit 三个事件推回**发起 spawn 的那个文档**（见 `../doc/process.md`）。**stdin 仍待做** —— 目前子进程的 stdin 是空设备（NUL）；`env` 选项、`proc.kill()` 也待做 |
| 9 | 启动 HTTP 服务 | **发请求已落地**：`Http.fetch()`（走 C++ WinHTTP，**无 CORS 限制**，见 `../doc/http.md`）。**起服务待做**：`Http.createServer()` 已登记为 T014 |
| 10 | 访问屏幕信息 | 显示器、分辨率、DPI 等。**已落地**：`display.getAll()` / `getPrimary()` / `getByPos()` / `getByRect()` / `getColor()` / `getImage()` + `display-added` / `display-removed`（见 `../doc/display.md`）。DPI 变化事件待定（T012） |
| 11 | 访问版本信息 | **系统版本 / 框架版本 / 应用版本** 三类。**系统版本已落地**：`os.getVersion()` |
| 12 | 目录操作 API | 创建、遍历、删除等。**路径字符串运算已落地**：`path.join()` / `resolve()` / `normalize()` / `dirname()` / `basename()` / `extname()` / `parse()` / `format()` / `isAbsolute()` / `relative()` + Windows 专属的 `isValidName()` / `isValidPath()`（见 `../doc/path.md`）。**纯 JS、同步**，不经过 C++；目录的创建/遍历/删除仍待做 |
| 13 | 访问常用目录 | 音乐、文档、数据目录等 |
| 14 | 访问窗口 | 枚举窗口、向窗口发送消息 |
| 15 | 右键菜单 | **已落地**：窗口的 `Menu.create()` / `win.setContextMenu()`（见 `../doc/menu.md`）；托盘的 `Tray.create({ menu })`（见 `../doc/tray.md`） |
| 16 | 本地数据库（SQLite） | **已落地**：`Db.open(name?)` 返回实例，实例上 `exec()` / `run()` / `get()` / `all()` / `close()`（见 `../doc/db.md`）。库文件默认在数据目录 `db/` 下，也可给完整路径自己指定。安全上**禁用 `ATTACH`**（否则任意 SQL 能绕过 `open` 的路径约束） |
| 17 | 系统凭据（凭据管理器） | **已落地**：`os.credWrite(key, value)` / `os.credRead(key)`（不存在返回 `null`）/ `os.credDelete(key)`（幂等）/ `os.credList(prefix?)`（只给 key，见 `../doc/os.md`）。一律"普通凭据"，存在**本机**这一栏：不漫游、只对当前用户可见 |
| 18 | 注册表 | **已落地**：`regedit.get(key, name?)`（不存在返回 `null`）/ `regedit.set(key, name, value)`（键不存在时自动逐层建）/ `regedit.deleteKey(key)`（递归）/ `regedit.listKeys(key)` / `regedit.listValues(key)`（见 `../doc/regedit.md`）。键路径写成一整条，跟 reg.exe 一样；根键简写与全名都认。**写 HKLM 需要管理员** |
| 19 | 网络 | **部分落地**：`Net.getAddress()` 取本机各网卡的名字 / MAC / IP 地址（静态方法，原 `os.getIpAddr()` 迁来并改名，见 `../doc/net.md`）。已过滤未启用网卡与回环地址。**待实现**：联网状态变化事件、本机端口占用查询 |

## 规划中的能力（已列入待办）

| 能力 | 待办 ID |
| --- | --- |
| 加解密 API | T005 |
| 启动 WebSocket 服务 | T006 |
| 菜单建好后再改：`menu.setLabel(id)` / `setChecked(id)` / `setDisabled(id)` | T007 |

## 待确认

- 各能力的 API 命名与参数：待逐项设计时确定。
- SQLite 的整数精度：INTEGER 是 64 位的，转到 JS number（double）后超过 2^53 会丢精度。
  目前一律转 number；若实际场景用得到大整数，再考虑给 `Db.open()` 加开关。
- SQLite 的查询目前**同步跑在 UI 线程**（与 dialog / menu 一致的取舍）。
  若出现慢查询拖住界面，再引入后台线程 + `App::runLater` 回抛结果。
- 需要权限/安全管控的能力（文件、进程、窗口操作等）是否设白名单：
  与 `02-architecture.md` 的"权限/安全"待定项联动。
