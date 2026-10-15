/**
 * WebWin JS SDK 入口（打包成 dist/webwin.js，内嵌进 exe）
 *
 * 安装成功后，页面里可以直接使用：
 *   win            —— 当前窗口对象
 *   Win            —— 窗口类型，Win.create() 创建新窗口
 */
import { Win } from './Win';
declare global {
    interface Window {
        /** 当前窗口 */
        win: Win;
        /** 窗口类型 */
        Win: typeof Win;
    }
}
