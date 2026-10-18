/**
 * WebWin JS SDK 入口（打包成 dist/webwin.js，内嵌进 exe）
 *
 * 安装成功后，页面里可以直接使用：
 *   win            —— 当前窗口对象
 *   Win            —— 窗口类型，Win.create() 创建新窗口
 *   os / app / display / dialog / path —— 与窗口无关的全局单例能力
 *   （dialog 虽是单例，但绑在当前窗口上：弹出来的框模态到当前窗口）
 *   path 是纯 JS 的字符串工具，不经过 C++
 *   Menu / Db / Tray —— 需要自己建实例的类型（Menu.create() / Db.open() / Tray.create()）
 *   Http —— 只有静态方法的类型（Http.fetch() 发请求，走 C++ 所以没有 CORS 限制）
 */

import { App } from './App';
import { Db } from './Db';
import { Dialog } from './Dialog';
import { Display } from './Display';
import { Http } from './Http';
import { installChannel } from './Ipc';
import { Menu } from './Menu';
import { Os } from './Os';
import { Path } from './Path';
import { Tray } from './Tray';
import { Win } from './Win';

declare global {
	interface Window {
		/** 当前窗口 */
		win: Win;
		/** 窗口类型 */
		Win: typeof Win;
		/** 系统级能力（全局单例，与窗口无关） */
		os: Os;
		/** 应用级能力（全局单例，与窗口无关） */
		app: App;
		/** 路径字符串工具（纯 JS，同步；不经过 C++） */
		path: Path;
		/** 显示设备能力（全局单例，与窗口无关） */
		display: Display;
		/** 系统对话框能力（全局单例，但绑在当前窗口上） */
		dialog: Dialog;
		/** 菜单类型 */
		Menu: typeof Menu;
		/** 数据库类型：Db.open() 建连接，实例上再跑 SQL */
		Db: typeof Db;
		/** 托盘类型：Tray.create() 建图标，实例上改提示、销毁、听事件 */
		Tray: typeof Tray;
		/** HTTP 请求类型：Http.fetch() 发请求（走 C++，不受 CORS 限制） */
		Http: typeof Http;
	}
}

const config = installChannel();
if (config) {
	window.win = Win.fromId(config.winId ?? 0);
	window.Win = Win;
	window.os = new Os();
	window.app = new App();
	// 唯一的纯 JS 能力：不发消息，只从注入的配置里取基准目录
	window.path = new Path(config.appPath ?? '');
	window.display = new Display();
	window.dialog = new Dialog(config.winId ?? null); // 对话框模态到当前窗口
	window.Menu = Menu; // 菜单点击由 Menu 域自己在通道上登记钩子认领，这里不必接
	window.Db = Db; // 数据库没有默认实例：必须 Db.open() 自己建
	window.Tray = Tray; // 托盘同理：图标要自己建，一个进程允许建多个
	window.Http = Http; // HTTP 只有静态方法，不用建实例
}
