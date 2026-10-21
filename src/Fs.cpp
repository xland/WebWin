#include "Fs.h"

#include <filesystem>
#include <fstream>
#include <iterator>

#include "App.h"
#include "Win.h"
#include "Async.h"
#include "Json.h"
#include "Util.h"

// 说明见 Fs.h 头部：本域只管文件，全部异步（跑在 Async 的后台线程上）
namespace
{
	/**
	 * 相对路径 → 绝对路径。基准是 package.json 所在目录，跟 JS 的 path.resolve() 一致。
	 * 带盘符（C:\）或 UNC（\\server\share）的一律原样用。
	 *
	 * 注意 `\a` 这种只有根分隔符、没有盘符的仍然依赖"当前盘"，属于 Win32 的既有行为，
	 * 文档里写明了 —— 想可预测就写全盘符。
	 */
	std::filesystem::path resolvePath(const std::wstring& p)
	{
		auto path = std::filesystem::path{ p };
		if (!path.has_root_name() && !path.has_root_directory()) return App::getPackageBaseDir() / path;
		return path;
	}

	/** FILETIME → Unix 毫秒。FILETIME 是 1601-01-01 起的 100ns 计数 */
	long long fileTimeToMs(const FILETIME& ft)
	{
		auto v = (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
		return static_cast<long long>(v / 10000) - 11644473600000LL;
	}

	/** 读全部字节。失败时 error 非空 */
	std::string readAll(const std::filesystem::path& path, std::wstring& error)
	{
		std::error_code ec;
		if (std::filesystem::is_directory(path, ec)) { error = L"是目录，不是文件"; return {}; }

		std::ifstream f(path, std::ios::binary);
		if (!f) { error = L"读不到文件（不存在或没权限）"; return {}; }
		return std::string{ std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
	}

	/** 写字节。append=false 时覆盖。失败时 error 非空 */
	bool writeAll(const std::filesystem::path& path, const std::string& bytes, bool append, std::wstring& error)
	{
		// 不自动建父目录：Node 的 writeFile 也不建，静默建目录会把拼错的路径变成一堆垃圾目录
		auto dir = path.parent_path();
		std::error_code ec;
		if (!dir.empty() && !std::filesystem::exists(dir, ec))
		{
			error = L"所在目录不存在（不会自动创建）：" + dir.wstring();
			return false;
		}

		auto mode = std::ios::binary | (append ? std::ios::app : std::ios::trunc);
		std::ofstream f(path, mode);
		if (!f) { error = L"打不开文件写入（没权限或已被别的程序占用）"; return false; }
		if (!bytes.empty()) f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		f.flush();
		if (!f) { error = L"写入失败（磁盘满了？）"; return false; }
		return true;
	}

	/** writeFile / appendFile 共用的那条：把活丢给 Async，回包没有 data */
	bool runWrite(const std::filesystem::path& path, std::string bytes, bool append,
		Page* page, JsonObject& result, ICoreWebView2Frame2* frame, const wchar_t* failMsg)
	{
		auto started = Async::run(page, result, frame, [path, bytes = std::move(bytes), append] {
			std::wstring error;
			if (!writeAll(path, bytes, append, error))
				return AsyncResult::fail(error + L"：" + path.wstring());
			return AsyncResult::ok(nullptr); // 没返回值：resolve(undefined)
		});
		if (!started) { Json::setError(result, L"fs.error", failMsg); return false; }
		return true;
	}
}

bool Fs::handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	if (method == L"readFile") return readFile(params, page, result, frame);
	if (method == L"readFileChunk") return readFileChunk(params, page, result, frame);
	// fs 域的事件订阅：委托到 NO_WINDOW（与窗口无关，同 app 域的做法）。
	// 分块读取的"隐藏事件" fs-chunk 就靠它下发到 JS
	if (method == L"on") { Win::subscribeEvent(Win::NO_WINDOW, Json::getString(params, L"name", L""), page, frame); Json::setOk(result); return true; }
	if (method == L"off") { Win::unsubscribeEvent(Win::NO_WINDOW, Json::getString(params, L"name", L""), page, frame); Json::setOk(result); return true; }
	if (method == L"writeFile") return writeFile(params, page, result, frame);
	if (method == L"appendFile") return appendFile(params, page, result, frame);
	if (method == L"unlink") return unlink(params, page, result, frame);
	if (method == L"copyFile") return copyFile(params, page, result, frame);
	if (method == L"rename") return rename(params, page, result, frame);
	if (method == L"stat") return stat(params, page, result, frame);
	if (method == L"access") return access(params, page, result, frame);
	return false; // 交给调用方回 unknown method
}

bool Fs::readFile(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto raw = Json::getString(params, L"path", L"");
	if (raw.empty()) { Json::setError(result, L"fs.invalidParam", L"path 不能为空"); return false; }
	auto path = resolvePath(raw);
	auto encoding = Json::getString(params, L"encoding", L"utf8");
	auto chunkSize = Json::getInt(params, L"chunkSize", 0);

	// 分块流式：chunkSize>0 时读全文件后在 UI 线程逐块下行（配合 JS 侧 callback）。
	// 块走"隐藏事件"通道（fireEvent 'fs-chunk' 带 streamId），不污染请求-响应协议：
	// JS 用 fs.on('fs-chunk') 按 streamId 路由到本次的 callback
	if (chunkSize > 0)
	{
		auto streamId = Json::getInt(params, L"streamId", 0);
		auto started = Async::run(page, result, frame, [path, encoding, chunkSize, streamId] {
			std::wstring error;
			auto bytes = readAll(path, error);
			if (!error.empty()) return AsyncResult::fail(error + L"：" + path.wstring());

			// utf8 切块时把边界对齐到"完整字符之后"，把跨边界的那一个多字节字符
			// 整个留给下一块，避免块尾巴截出半个字符；base64 / binary 没这问题
			auto align = [](const std::string& b, size_t off, size_t len) -> size_t {
				size_t end = off + len;
				if (end >= b.size()) return len; // 到 EOF 不调整
				size_t scan = end;
				while (scan > off) // 往前找到 end 之前最后一个非续字节（完整字符首字节）
				{
					unsigned char c = static_cast<unsigned char>(b[scan - 1]);
					if ((c & 0xC0) != 0x80) break;
					--scan;
				}
				return scan - off; // [scan, end) 是不完整的字符，留给下一块
			};

			size_t total = bytes.size();
			size_t cs = static_cast<size_t>(chunkSize);
			AsyncResult r;
			for (size_t off = 0; off < total; )
			{
				size_t len = (std::min)(cs, total - off);
				bool isLast = (off + cs >= total);
				if (encoding == L"utf8" && !isLast) len = align(bytes, off, len);
				std::vector<uint8_t> chunkBytes(bytes.begin() + off, bytes.begin() + off + len);
				size_t index = r.chunks.size();
				// 块走事件通道：finish 会用 fireEvent(NO_WINDOW, 'fs-chunk', ...) 下发。
				// 回调只在 UI 线程上调（finish 里）：闭包只捕获字节，不在此构造 winrt 对象
				r.chunks.push_back(AsyncResult::StreamChunk{ L"fs-chunk", [encoding, chunkBytes = std::move(chunkBytes), index, isLast, streamId]() -> JsonObject {
					JsonObject o{ nullptr };
					std::string s = (encoding == L"base64" || encoding == L"binary")
						? Util::toBase64(std::string(chunkBytes.begin(), chunkBytes.end()))
						: std::string(chunkBytes.begin(), chunkBytes.end()); // utf8 原始字节
					o.SetNamedValue(L"content", JsonValue::CreateStringValue(Util::utf8ToWide(s)));
					o.SetNamedValue(L"index", JsonValue::CreateNumberValue(static_cast<double>(index)));
					o.SetNamedValue(L"isLast", JsonValue::CreateBooleanValue(isLast));
					o.SetNamedValue(L"streamId", JsonValue::CreateNumberValue(static_cast<double>(streamId)));
					return o;
				} });
				off += len;
			}
			// 最终响应带的元数据：页面据此知道总量，不必靠数回调次数
			size_t totalChunks = r.chunks.size();
			r.data = [total, totalChunks]() -> IJsonValue {
				JsonObject o{ nullptr };
				o.SetNamedValue(L"totalBytes", JsonValue::CreateNumberValue(static_cast<double>(total)));
				o.SetNamedValue(L"totalChunks", JsonValue::CreateNumberValue(static_cast<double>(totalChunks)));
				return o;
			};
			return r;
		});
		if (!started) { Json::setError(result, L"fs.error", L"创建文件分块读取任务失败"); return false; }
		return true;
	}

	// ---- 原有的一次性读取（chunkSize<=0）----
	auto started = Async::run(page, result, frame, [path, encoding] {
		std::wstring error;
		auto bytes = readAll(path, error);
		if (!error.empty()) return AsyncResult::fail(error + L"：" + path.wstring());

		// binary：字节走共享内存，页面拿到 ArrayBuffer —— 不进 JSON，省掉 base64 那 1/3 膨胀
		if (encoding == L"binary")
		{
			auto size = bytes.size();
			return AsyncResult::binary(std::vector<uint8_t>(bytes.begin(), bytes.end()), [size] {
				JsonObject data;
				data.SetNamedValue(L"size", JsonValue::CreateNumberValue(static_cast<double>(size)));
				return data;
			});
		}

		// base64：二进制内容无损，代价是字符串体积涨 1/3。读图片、zip 用它
		if (encoding == L"base64")
			return AsyncResult::ok([text = Util::toBase64(bytes)] {
				return JsonValue::CreateStringValue(Util::utf8ToWide(text));
			});

		// 默认 utf8：非 UTF-8 的内容（比如 GBK 文本）会出乱码，要原样字节就用 base64 / binary
		return AsyncResult::ok([text = Util::utf8ToWide(bytes)] {
			return JsonValue::CreateStringValue(text);
		});
	});
	if (!started) { Json::setError(result, L"fs.error", L"创建文件读取任务失败"); return false; }
	return true;
}

bool Fs::readFileChunk(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto raw = Json::getString(params, L"path", L"");
	if (raw.empty()) { Json::setError(result, L"fs.invalidParam", L"path 不能为空"); return false; }
	auto path = resolvePath(raw);
	auto encoding = Json::getString(params, L"encoding", L"utf8");
	auto start = Json::getInt(params, L"start", 0);
	auto size = Json::getInt(params, L"size", 0);
	if (start < 0) start = 0;
	if (size <= 0) { Json::setError(result, L"fs.invalidParam", L"size 必须为正数"); return false; }

	auto started = Async::run(page, result, frame, [path, encoding, start, size] {
		std::ifstream f(path, std::ios::binary);
		if (!f) return AsyncResult::fail(L"读不到文件（不存在或没权限）：" + path.wstring());
		f.seekg(start, std::ios::beg);
		std::vector<char> buf(static_cast<size_t>(size));
		f.read(buf.data(), size);
		std::streamsize got = f.gcount();
		if (got <= 0) return AsyncResult::ok([]() -> IJsonValue { return JsonValue::CreateStringValue(L""); });
		std::vector<uint8_t> bytes(buf.begin(), buf.begin() + static_cast<size_t>(got));

		if (encoding == L"binary")
		{
			return AsyncResult::binary(std::move(bytes), [got]() {
				JsonObject data;
				data.SetNamedValue(L"size", JsonValue::CreateNumberValue(static_cast<double>(got)));
				return data;
			});
		}
		if (encoding == L"base64")
			return AsyncResult::ok([text = Util::toBase64(std::string(bytes.begin(), bytes.end()))]() {
				return JsonValue::CreateStringValue(Util::utf8ToWide(text));
			});
		return AsyncResult::ok([text = Util::utf8ToWide(std::string(bytes.begin(), bytes.end()))]() {
			return JsonValue::CreateStringValue(text);
		});
	});
	if (!started) { Json::setError(result, L"fs.error", L"创建文件分段读取任务失败"); return false; }
	return true;
}

bool Fs::writeFile(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto raw = Json::getString(params, L"path", L"");
	if (raw.empty()) { Json::setError(result, L"fs.invalidParam", L"path 不能为空"); return false; }
	auto path = resolvePath(raw);

	auto text = Json::getString(params, L"data", L"");
	auto encoding = Json::getString(params, L"encoding", L"utf8");
	std::string bytes;
	if (encoding == L"base64")
	{
		bytes = Util::fromBase64(Util::wideToUtf8(text));
		if (!text.empty() && bytes.empty()) { Json::setError(result, L"fs.encodeInvalid", L"data 不是合法的 base64"); return false; }
	}
	else bytes = Util::wideToUtf8(text);

	return runWrite(path, std::move(bytes), false, page, result, frame, L"创建文件写入任务失败");
}

bool Fs::appendFile(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto raw = Json::getString(params, L"path", L"");
	if (raw.empty()) { Json::setError(result, L"fs.invalidParam", L"path 不能为空"); return false; }
	auto path = resolvePath(raw);

	auto text = Json::getString(params, L"data", L"");
	auto encoding = Json::getString(params, L"encoding", L"utf8");
	std::string bytes;
	if (encoding == L"base64")
	{
		bytes = Util::fromBase64(Util::wideToUtf8(text));
		if (!text.empty() && bytes.empty()) { Json::setError(result, L"fs.encodeInvalid", L"data 不是合法的 base64"); return false; }
	}
	else bytes = Util::wideToUtf8(text);

	return runWrite(path, std::move(bytes), true, page, result, frame, L"创建文件追加任务失败");
}

bool Fs::unlink(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto raw = Json::getString(params, L"path", L"");
	if (raw.empty()) { Json::setError(result, L"fs.invalidParam", L"path 不能为空"); return false; }
	auto path = resolvePath(raw);

	auto started = Async::run(page, result, frame, [path] {
		if (DeleteFileW(path.c_str())) return AsyncResult::ok(nullptr);
		return AsyncResult::fail(L"删除失败：" + Util::winErr(GetLastError()) + L"：" + path.wstring());
	});
	if (!started) { Json::setError(result, L"fs.error", L"创建文件删除任务失败"); return false; }
	return true;
}

bool Fs::copyFile(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto rawSrc = Json::getString(params, L"src", L"");
	auto rawDest = Json::getString(params, L"dest", L"");
	if (rawSrc.empty() || rawDest.empty()) { Json::setError(result, L"fs.invalidParam", L"src 和 dest 都不能为空"); return false; }
	auto src = resolvePath(rawSrc);
	auto dest = resolvePath(rawDest);

	auto started = Async::run(page, result, frame, [src, dest] {
		// 第三参数 FALSE = 目标已存在就覆盖（Node 的 copyFile 默认也是覆盖）
		if (CopyFileW(src.c_str(), dest.c_str(), FALSE)) return AsyncResult::ok(nullptr);
		// 两端都报出来。只报 src 的话，目标目录没写权限这种常见情况
		// 看起来就像"源文件读不了"，排查方向直接跑偏
		return AsyncResult::fail(L"复制失败：" + Util::winErr(GetLastError())
			+ L"：" + src.wstring() + L" → " + dest.wstring());
	});
	if (!started) { Json::setError(result, L"fs.error", L"创建文件复制任务失败"); return false; }
	return true;
}

bool Fs::rename(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto rawOld = Json::getString(params, L"oldPath", L"");
	auto rawNew = Json::getString(params, L"newPath", L"");
	if (rawOld.empty() || rawNew.empty()) { Json::setError(result, L"fs.invalidParam", L"oldPath 和 newPath 都不能为空"); return false; }
	auto from = resolvePath(rawOld);
	auto to = resolvePath(rawNew);

	auto started = Async::run(page, result, frame, [from, to] {
		// REPLACE_EXISTING：目标已存在就覆盖
		// COPY_ALLOWED：跨盘移动时允许"复制 + 删源"，否则跨盘 rename 会直接失败
		if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
			return AsyncResult::ok(nullptr);
		return AsyncResult::fail(L"移动失败：" + Util::winErr(GetLastError()) + L"：" + from.wstring());
	});
	if (!started) { Json::setError(result, L"fs.error", L"创建文件移动任务失败"); return false; }
	return true;
}

bool Fs::stat(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto raw = Json::getString(params, L"path", L"");
	if (raw.empty()) { Json::setError(result, L"fs.invalidParam", L"path 不能为空"); return false; }
	auto path = resolvePath(raw);

	auto started = Async::run(page, result, frame, [path] {
		WIN32_FILE_ATTRIBUTE_DATA d{};
		if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d))
			return AsyncResult::fail(L"取不到文件信息：" + Util::winErr(GetLastError()) + L"：" + path.wstring());
		return AsyncResult::ok([d] {
			auto attrs = d.dwFileAttributes;
			auto isDir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
			// size 走 double：JSON 只有一种数字，超过 2^53 会掉精度（8PB 以上才碰到）
			auto size = (static_cast<unsigned long long>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;

			JsonObject data;
			data.SetNamedValue(L"size", JsonValue::CreateNumberValue(static_cast<double>(size)));
			data.SetNamedValue(L"isFile", JsonValue::CreateBooleanValue(!isDir));
			data.SetNamedValue(L"isDirectory", JsonValue::CreateBooleanValue(isDir));
			data.SetNamedValue(L"isSymbolicLink", JsonValue::CreateBooleanValue((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0));
			data.SetNamedValue(L"readonly", JsonValue::CreateBooleanValue((attrs & FILE_ATTRIBUTE_READONLY) != 0));
			data.SetNamedValue(L"hidden", JsonValue::CreateBooleanValue((attrs & FILE_ATTRIBUTE_HIDDEN) != 0));
			data.SetNamedValue(L"mtimeMs", JsonValue::CreateNumberValue(static_cast<double>(fileTimeToMs(d.ftLastWriteTime))));
			data.SetNamedValue(L"atimeMs", JsonValue::CreateNumberValue(static_cast<double>(fileTimeToMs(d.ftLastAccessTime))));
			data.SetNamedValue(L"birthtimeMs", JsonValue::CreateNumberValue(static_cast<double>(fileTimeToMs(d.ftCreationTime))));
			// Windows 没有 POSIX 权限位：只读给 0o444，否则 0o666
			data.SetNamedValue(L"mode", JsonValue::CreateNumberValue(
				static_cast<double>((attrs & FILE_ATTRIBUTE_READONLY) ? 0444 : 0666)));
			// 也没有 ctime（inode 变更时间）这个东西：不给 ctimeMs，免得给个假数字
			return data;
		});
	});
	if (!started) { Json::setError(result, L"fs.error", L"创建信息查询任务失败"); return false; }
	return true;
}

bool Fs::access(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto raw = Json::getString(params, L"path", L"");
	if (raw.empty()) { Json::setError(result, L"fs.invalidParam", L"path 不能为空"); return false; }
	auto path = resolvePath(raw);
	auto mode = Json::getInt(params, L"mode", 0); // 0 = F_OK，只查存在

	auto started = Async::run(page, result, frame, [path, mode] {
		auto attrs = GetFileAttributesW(path.c_str());
		if (attrs == INVALID_FILE_ATTRIBUTES)
			return AsyncResult::fail(L"不存在或无法访问：" + Util::winErr(GetLastError()) + L"：" + path.wstring());
		auto isDir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;

		// R_OK：真的开一次句柄才知道能不能读 —— 只读属性挡的是写，挡不住读
		if (mode & 4)
		{
			// 目录不开句柄（CreateFile 开目录要 FILE_FLAG_BACKUP_SEMANTICS），能取到属性就算可读
			if (!isDir)
			{
				auto h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
					nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (h == INVALID_HANDLE_VALUE)
					return AsyncResult::fail(L"不可读：" + Util::winErr(GetLastError()) + L"：" + path.wstring());
				CloseHandle(h);
			}
		}
		// W_OK：看只读属性。这是近似 —— 真正的写权限还受 ACL 管辖，这里不查
		if ((mode & 2) && (attrs & FILE_ATTRIBUTE_READONLY))
			return AsyncResult::fail(L"只读，不可写：" + path.wstring());
		// X_OK（1）在 Windows 上没有对应概念：能取到属性就算可执行，不额外检查
		return AsyncResult::ok(nullptr);
	});
	if (!started) { Json::setError(result, L"fs.error", L"创建访问检查任务失败"); return false; }
	return true;
}
