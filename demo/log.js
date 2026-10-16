/**
 * 演示用日志（win.html / os.html / index.html 三页共用）。
 *
 * 三栏布局下日志统一写在**外壳页面**（index.html）右侧那一栏：
 * win.html 和 os.html 都跑在中间的 iframe 里，它们调 log() 时会转交给外壳。
 *
 * 没有外壳时（比如用 Win.create 新开的窗口直接加载 win.html），
 * 退回本页面打印 —— 页面里没有日志区就临时补一块，免得调用结果看不见。
 */
(function () {
	/** 写进当前页面的 #log；没有就临时建一块 */
	function writeLocal(text, kind) {
		let box = document.getElementById('log');
		if (!box) {
			box = document.createElement('div');
			box.id = 'log';
			box.style.cssText = 'background:#1e1e1e;color:#d4d4d4;'
				+ 'font:12px/1.6 Consolas,"Courier New",monospace;'
				+ 'padding:12px;border-radius:6px;white-space:pre-wrap';
			document.body.appendChild(box);
		}
		const line = document.createElement('div');
		line.className = kind || '';
		line.textContent = `[${new Date().toLocaleTimeString('zh-CN', { hour12: false })}] ${text}`;
		box.appendChild(line);
		box.scrollTop = box.scrollHeight; // 滚到底部，最新一条始终可见
	}

	window.log = function (text, kind) {
		// 有外壳就交给外壳，所有页面的日志集中在一处
		if (window.parent !== window && typeof window.parent.log === 'function') {
			window.parent.log(text, kind);
			return;
		}
		writeLocal(text, kind);
	};
})();
