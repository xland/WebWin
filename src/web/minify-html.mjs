// 把 default.html 压缩后输出到 dist/default.html
// 该页面会内嵌进 exe（见 src/Res.rc），属于最终分发物，所以要压
import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { minify } from 'html-minifier-terser';

const source = await readFile('default.html', 'utf8');
const result = await minify(source, {
	collapseWhitespace: true,
	removeComments: true,
	removeRedundantAttributes: true,
	removeScriptTypeAttributes: true,
	removeStyleLinkTypeAttributes: true,
	minifyCSS: true,
	minifyJS: true
});

await mkdir('dist', { recursive: true });
await writeFile('dist/default.html', result);
console.log(`default.html -> dist/default.html (${source.length} -> ${result.length} bytes)`);
