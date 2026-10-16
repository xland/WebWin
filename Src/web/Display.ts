import { Ipc } from './Ipc';
import type { SharedBufferData } from './Ipc';

/** 矩形。坐标与尺寸都是**物理像素**（不是 CSS 像素） */
export interface Rect {
	x: number;
	y: number;
	width: number;
	height: number;
}

/** 一块显示器 */
export interface DisplayInfo {
	/** 设备名，形如 `\\.\DISPLAY1`；可以当标识用 */
	id: string;
	/** 是否主显示器 */
	primary: boolean;
	/** 缩放百分比：100 / 125 / 150 ... */
	scaleFactor: number;
	/** 屏幕矩形（物理像素） */
	bounds: Rect;
	/** 工作区矩形：屏幕矩形扣掉任务栏等 */
	workArea: Rect;
}

/** `display.getImage` 的返回值：PNG 字节走共享内存，不进 JSON */
export interface CapturedImage extends SharedBufferData {
	/** 截图宽度（物理像素，等于传入的 width） */
	width: number;
	/** 截图高度 */
	height: number;
}

/**
 * 显示设备能力（display 域）。
 *
 * display 与窗口无关，所以它是**全局单例** `window.display` ——
 * 不像 `win` 那样一个窗口一个实例，事件也是全局订阅。
 *
 * 所有坐标 / 尺寸都是**物理像素**：150% 缩放的 4K 屏，bounds.width 是 3840。
 */
export class Display extends Ipc {
	private static readonly domain = 'display'; // 对应 C++ 侧的 Display 域处理器

	protected type = Display.domain;

	/** 所有显示器 */
	getAll(): Promise<DisplayInfo[]> { return this.call('getAll'); }

	/** 主显示器 */
	getPrimary(): Promise<DisplayInfo> { return this.call('getPrimary'); }

	/** 包含该点的显示器；点落在多屏之间的空隙时给最近的一块 */
	getByPos(x: number, y: number): Promise<DisplayInfo> { return this.call('getByPos', { x, y }); }

	/** 与该矩形重叠面积最大的显示器；一块都不重叠时给中心点最近的那块 */
	getByRect(x: number, y: number, width: number, height: number): Promise<DisplayInfo> {
		return this.call('getByRect', { x, y, width, height });
	}

	/** 截屏幕的一块区域，返回 PNG 字节。用完记得 release() */
	getImage(x: number, y: number, width: number, height: number): Promise<CapturedImage> {
		return this.call('getImage', { x, y, width, height });
	}

	/**
	 * 取屏幕上某一点的颜色（`#RRGGBB`）。
	 * 不传坐标就取**鼠标当前位置**的颜色 —— 做取色器时不用自己跟踪鼠标
	 */
	getColor(x?: number, y?: number): Promise<string> {
		const params = (x === undefined || y === undefined) ? {} : { x, y };
		return this.call('getColor', params);
	}
}
