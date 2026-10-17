/**
 * WebWin JS SDK 入口（打包成 dist/webwin.js，内嵌进 exe）
 *
 * 安装成功后，页面里可以直接使用：
 *   win            —— 当前窗口对象
 *   Win            —— 窗口类型，Win.create() 创建新窗口
 *   os / app / display / dialog —— 与窗口无关的全局单例能力
 *   （dialog 虽是单例，但绑在当前窗口上：弹出来的框模态到当前窗口）
 *   Menu / Db   —— 需要自己建实例的类型（Menu.create() / Db.open()）
 */

import { App } from './App';
import { Db } from './Db';
import { Dialog } from './Dialog';
import { Display } from './Display';
import { installChannel } from './Ipc';
import { Menu } from './Menu';
import { Os } from './Os';
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
		/** 显示设备能力（全局单例，与窗口无关） */
		display: Display;
		/** 系统对话框能力（全局单例，但绑在当前窗口上） */
		dialog: Dialog;
		/** 菜单类型 */
		Menu: typeof Menu;
		/** 数据库类型：Db.open() 建连接，实例上再跑 SQL */
		Db: typeof Db;
	}
}

const config = installChannel();
if (config) {
	window.win = Win.fromId(config.winId ?? 0);
	window.Win = Win;
	window.os = new Os();
	window.app = new App();
	window.display = new Display();
	window.dialog = new Dialog(config.winId ?? null); // 对话框模态到当前窗口
	window.Menu = Menu; // 菜单点击由 Menu 域自己在通道上登记钩子认领，这里不必接
	window.Db = Db; // 数据库没有默认实例：必须 Db.open() 自己建
}
