# WebWin

一个类似 TAURI 的框架，提供的能力比 TAURI 要多，仅支持 Windows，基于 C++20 开发，底层框架：WebView2、WinRT。

# 文档

## 使用文档

| 文档                            | 内容                                  |
| ------------------------------- | ------------------------------------- |
| [配置说明](doc/config.md)       | `package.json` 里能配什么             |
| [窗口 API](doc/win.md)          | `win` / `Win`：窗口控制、窗口事件     |
| [系统 API](doc/os.md)           | `os`：系统信息、锁屏、凭据            |
| [显示设备 API](doc/display.md)  | `display`：显示器、截屏、取色         |
| [系统对话框 API](doc/dialog.md) | `dialog`：选文件、存文件、消息框      |
| [右键菜单 API](doc/menu.md)     | `Menu`：右键菜单与菜单项              |
| [应用 API](doc/app.md)          | `app`：应用信息、路径、退出           |
| [数据库 API](doc/db.md)         | `db`：使用本地SQLite数据库            |
| [托盘 API](doc/tray.md)         | `Tray`：托盘图标、右键菜单、事件      |
| [路径 API](doc/path.md)         | `path`：路径拼接、拆分、校验          |
| [HTTP API](doc/http.md)         | `Http`：发 HTTP/HTTPS 请求            |
| [文件 API](doc/fs.md)           | `fs`：文件读写、删除、复制、信息      |
| [exe 资源 API](doc/res.md)      | `res`：exe 资源读 / 写 / 删与版本信息 |
| [子进程 API](doc/process.md)    | `Process`：启动子进程、收输出与退出码 |
| [注册表 API](doc/regedit.md)    | `regedit`：注册表读 / 写 / 删键与枚举 |
| [网络 API](doc/net.md)          | `Net`：网卡信息（名字 / MAC / IP）    |
