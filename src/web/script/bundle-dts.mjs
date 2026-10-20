// 把 tsc 生成的多个 .d.ts 合并成单个 dist/webwin.d.ts
//
// 产物是**一个全局声明文件**（合并后不含任何 import / export），
// 开发者只需 /// <reference path="./webwin.d.ts" /> 就能拿到全部类型提示，
// 不必引一堆文件 —— 交付口径见 arch/00-overview.md：SDK = exe + .d.ts
//
// 输出文件名刻意用小写：它跟 dist/webwin.js 是一对，
// TypeScript 见到同名 .d.ts 才会自动给 .js 补上类型
//
// 输入：dist/types/*.d.ts（tsconfig 的 outDir，中间产物，合并完删掉）
// 输出：dist/webwin.d.ts
import { readdir, readFile, writeFile, mkdir, rm } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..');
const srcDir = path.join(root, 'dist', 'types');
const outFile = path.join(root, 'dist', 'webwin.d.ts');

// 输出顺序：Ipc 是基类放最前，各域随后，global 块放最后。
// 新增能力域后要把它的 .d.ts 名字加到这里 —— 漏了构建会失败（见文件末尾的兜底检查）
const order = ['Ipc', 'App', 'Menu', 'Net', 'Os', 'Path', 'Win', 'Display', 'Dialog', 'Fs', 'Http', 'Process', 'Regedit', 'Db', 'Tray', 'WebWin'];

/** 去掉 declare global { ... } 这一层，让内容回到顶层（合并后大家都在同一作用域） */
function unwrapGlobal(text)
{
	const marker = 'declare global';
	const start = text.indexOf(marker);
	if (start < 0) return text;

	let i = start + marker.length;
	while (i < text.length && /\s/.test(text[i])) ++i;
	if (text[i] !== '{') return text;

	let depth = 0;
	const open = i;
	for (; i < text.length; ++i)
	{
		if (text[i] === '{') ++depth;
		else if (text[i] === '}' && --depth === 0) break;
	}
	if (depth !== 0) return text; // 括号不匹配就原样返回，宁可不改也不改坏

	return text.slice(0, start) + text.slice(open + 1, i) + text.slice(i + 1);
}

const files = await readdir(srcDir).catch(() => []);
const parts = [];
for (const name of order)
{
	const file = `${name}.d.ts`;
	if (!files.includes(file)) continue;

	const text = await readFile(path.join(srcDir, file), 'utf8')
		.then((t) => t
			// 合并后大家都在同一个文件里，模块间的 import 全部作废
			.replace(/^[ \t]*import\s+[^;]*;[ \t]*$/gm, '')
			.replace(/^([ \t]*)export\s+/gm, '$1')
		)
		.then(unwrapGlobal);

	const body = text.trim();
	if (body) parts.push(`// ---- ${name} ----\n${body}`);
}

// 兜底：order 里漏写的域会被**静默丢掉** —— 构建照常成功、exit 也是 0，
// 只是那个域的类型凭空消失（加 Db 域时就踩到了一次，很难察觉）。
// 宁可让构建失败并指名道姓，也不要悄悄少一块类型
const known = new Set(order.map((name) => `${name}.d.ts`));
const missed = files.filter((f) => f.endsWith('.d.ts') && !known.has(f));
if (missed.length)
{
	console.error(`bundle-dts: 这些 .d.ts 没写进 order，会被丢掉：${missed.join(', ')}`);
	process.exit(1);
}

const banner = [
	'/**',
	' * WebWin JS SDK —— 类型声明（自动生成，请勿手改）',
	' *',
	' * 生成方式：在 src/web 下执行 `npm run types`',
	' * 它由 tsc 输出的多个 .d.ts 合并而来，目的是让开发者只引一个文件。',
	' *',
	' * 用法（任选其一）：',
	' *   1. 在本文件旁 `/// <reference path="./webwin.d.ts" />`',
	' *   2. 把本文件加进你工程的 tsconfig 的 include / files',
	' *',
	' * 框架注入到页面里的全局对象：',
	' *   win（当前窗口）· Win（窗口类型）· os · app · display · dialog',
	' * 需要自己建实例的类型：Menu · Db · Tray · Process（都是静态工厂方法建实例）',
	' */',
	'',
].join('\n');

await mkdir(path.dirname(outFile), { recursive: true });
await writeFile(outFile, `${banner}${parts.join('\n\n')}\n`);
await rm(srcDir, { recursive: true, force: true });

console.log(`已合并 ${parts.length} 个声明 -> dist/webwin.d.ts`);
