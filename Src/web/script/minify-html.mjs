// 把 default.html 压缩后输出到 dist/default.html
// 该页面会内嵌进 exe（见 src/Res.rc），属于最终分发物，所以要压
import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { minify } from 'html-minifier-terser';

// 本脚本放在 script/ 下，但 npm 脚本的 cwd 是 src/web ——
// 两种情况都要能跑，所以路径一律相对**脚本自己**解析，不依赖 cwd
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

const source = await readFile(path.join(root, 'src', 'default.html'), 'utf8');
const result = await minify(source, {
	collapseWhitespace: true,
	removeComments: true,
	removeRedundantAttributes: true,
	removeScriptTypeAttributes: true,
	removeStyleLinkTypeAttributes: true,
	minifyCSS: true,
	minifyJS: true
});

await mkdir(path.join(root, 'dist'), { recursive: true });
await writeFile(path.join(root, 'dist', 'default.html'), result);
console.log(`default.html -> dist/default.html (${source.length} -> ${result.length} bytes)`);
