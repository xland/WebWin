/**
 * WebWin JS SDK 入口（打包成 dist/webwin.js，内嵌进 exe）
 *
 * 安装成功后，页面里可以直接使用：
 *   win            —— 当前窗口对象
 *   Win            —— 窗口类型，Win.create() 创建新窗口
 *   os / app / display —— 与窗口无关的全局单例能力
 */

import { App } from './App';
import { Display } from './Display';
import { installChannel, setMenuClickDispatcher } from './Ipc';
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
		/** 菜单类型 */
		Menu: typeof Menu;
	}
}

const config = installChannel();
if (config) {
	window.win = Win.fromId(config.winId ?? 0);
	window.Win = Win;
	window.os = new Os();
	window.app = new App();
	window.display = new Display();
	window.Menu = Menu;
	// 菜单点击不走事件通道（它不是"某个窗口的事件"），单独接一条
	setMenuClickDispatcher((msg) => Menu.dispatch(msg));
}
