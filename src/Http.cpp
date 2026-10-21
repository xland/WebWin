#include "Http.h"

#include <algorithm>
#include <cwctype>

#include "Async.h"
#include "Json.h"
#include "Util.h"

/**
 * http 域的实现。
 *
 * 发请求跑在后台线程上，但**线程和页面生命周期不归本文件管** ——
 * 那是 Async 的活（见 Async.h 头部的流程图）。这里只负责两件事：
 * 把 JS 给的参数整理成 WinHTTP 要的形状，以及把 doFetch 的结果转成 JSON。
 */
namespace
{
	/** 后台线程产出的结果。纯数据，方便跨线程搬运 */
	struct FetchResult
	{
		int status{ 0 };
		std::wstring statusText;
		std::map<std::wstring, std::wstring> headers; // key 统一小写
		std::string body;      // 响应体原始字节（JS 侧按 UTF-8 解成文本）
		std::wstring error;    // 非空 = 网络层就失败了，上面那些都没意义
	};

	/**
	 * 装 status / statusText / headers / ok —— 三种 encoding 都要这几项，抽出来免得抄三遍
	 */
	void setMeta(JsonObject& data, int status, const std::wstring& statusText,
		const std::map<std::wstring, std::wstring>& headers, bool ok)
	{
		JsonObject hs;
		for (const auto& [k, v] : headers)
			hs.SetNamedValue(k.c_str(), JsonValue::CreateStringValue(v.c_str()));
		data.SetNamedValue(L"status", JsonValue::CreateNumberValue(status));
		data.SetNamedValue(L"statusText", JsonValue::CreateStringValue(statusText));
		data.SetNamedValue(L"headers", hs);
		data.SetNamedValue(L"ok", JsonValue::CreateBooleanValue(ok));
	}

	/** 掐掉首尾空白和 CR */
	std::wstring trim(const std::wstring& s)
	{
		size_t b = 0, e = s.size();
		while (b < e && (s[b] == L' ' || s[b] == L'\t')) ++b;
		while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r')) --e;
		return s.substr(b, e - b);
	}

	/** 把 RAW_HEADERS_CRLF 那一大块按行拆成 key/value；同名头用 ", " 接起来（HTTP 的规矩） */
	void parseHeaders(const std::wstring& raw, std::map<std::wstring, std::wstring>& out)
	{
		size_t pos = 0;
		while (pos < raw.size())
		{
			auto end = raw.find(L"\r\n", pos);
			if (end == std::wstring::npos) end = raw.size();
			auto line = raw.substr(pos, end - pos);
			pos = end + 2;

			auto colon = line.find(L':');
			if (colon == std::wstring::npos) continue; // 状态行 / 续行，跳过

			auto key = trim(line.substr(0, colon));
			auto val = trim(line.substr(colon + 1));
			if (key.empty()) continue;

			// 统一小写：跟 web 的 Headers 一样按小写 key 取，省得大小写不对就取不到
			for (auto& c : key) c = static_cast<WCHAR>(std::towlower(c));

			auto it = out.find(key);
			if (it == out.end()) out.emplace(key, val);
			else { it->second += L", "; it->second += val; }
		}
	}

	/** 后台线程上跑：WinHTTP 的同步 API 发一次请求 */
	FetchResult doFetch(const std::wstring& url, const std::wstring& method,
		const std::wstring& headersText, const std::string& body)
	{
		FetchResult out;

		// ---- 1. 拆 URL ----
		URL_COMPONENTS uc{};
		uc.dwStructSize = sizeof(uc);
		WCHAR host[256]{};
		WCHAR path[4096]{};
		WCHAR extra[2048]{};
		uc.lpszHostName = host;   uc.dwHostNameLength = 256;
		uc.lpszUrlPath = path;    uc.dwUrlPathLength = 4096;
		uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
		if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc))
		{
			out.error = L"URL 解析失败（要写全，比如 https://example.com/a）";
			return out;
		}
		if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS)
		{
			out.error = L"只支持 http / https";
			return out;
		}
		const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

		HINTERNET session = nullptr;
		HINTERNET connect = nullptr;
		HINTERNET request = nullptr;
		auto cleanup = [&] {
			if (request) WinHttpCloseHandle(request);
			if (connect) WinHttpCloseHandle(connect);
			if (session) WinHttpCloseHandle(session);
		};
		auto fail = [&](const std::wstring& what) {
			out.error = what + L"：" + Util::winErr(GetLastError());
			cleanup();
			return out;
		};

		// AUTOMATIC_PROXY：走系统代理设置（公司网络里常常是必需的）
		session = WinHttpOpen(L"WebWin", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session) return fail(L"WinHttpOpen 失败");
		// 显式超时：WinHTTP 默认有几项是无限等待，卡住的请求会让线程一直挂着
		WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000);

		connect = WinHttpConnect(session, host, uc.nPort, 0);
		if (!connect) return fail(L"WinHttpConnect 失败");

		// UrlPath 不含 query，ExtraInfo 才是 ?a=1 —— 要拼回去
		auto object = std::wstring{ path } + extra;
		if (object.empty()) object = L"/";
		request = WinHttpOpenRequest(connect, method.c_str(), object.c_str(), nullptr,
			WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
		if (!request) return fail(L"WinHttpOpenRequest 失败");
		// 重定向用 WinHTTP 默认的 DISALLOW_HTTPS_TO_HTTP：会自动跟随，
		// 唯独拒绝 https 降级成 http —— 那会悄悄把加密连接变成明文

		// ---- 2. 发请求 ----
		// 请求头整块给 WinHttpSendRequest，长度传 -1 让它自己算（字符串以 \0 结尾）
		if (!WinHttpSendRequest(request,
			headersText.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headersText.c_str(),
			headersText.empty() ? 0 : static_cast<DWORD>(-1),
			body.empty() ? WINHTTP_NO_REQUEST_DATA : reinterpret_cast<LPVOID>(const_cast<char*>(body.data())),
			static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0))
			return fail(L"WinHttpSendRequest 失败");
		if (!WinHttpReceiveResponse(request, nullptr)) return fail(L"WinHttpReceiveResponse 失败");

		// ---- 3. 状态码 ----
		DWORD status = 0;
		DWORD len = sizeof(status);
		WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
		out.status = static_cast<int>(status);

		WCHAR statusText[512]{};
		len = sizeof(statusText);
		if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_TEXT, WINHTTP_HEADER_NAME_BY_INDEX,
			statusText, &len, WINHTTP_NO_HEADER_INDEX))
			out.statusText = statusText;

		// ---- 4. 响应头：先问一次长度（WinHTTP 固定返回 0 + ERROR_INSUFFICIENT_BUFFER） ----
		DWORD hdrLen = 0;
		WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
			WINHTTP_NO_OUTPUT_BUFFER, &hdrLen, WINHTTP_NO_HEADER_INDEX);
		if (hdrLen > 0)
		{
			std::wstring raw(static_cast<size_t>(hdrLen) / sizeof(WCHAR), L'\0');
			if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
				&raw[0], &hdrLen, WINHTTP_NO_HEADER_INDEX))
			{
				raw.resize(static_cast<size_t>(hdrLen) / sizeof(WCHAR));
				parseHeaders(raw, out.headers);
			}
		}

		// ---- 5. 响应体：QueryDataAvailable + ReadData 循环 ----
		for (;;)
		{
			DWORD available = 0;
			if (!WinHttpQueryDataAvailable(request, &available)) break;
			if (available == 0) break;
			std::vector<char> buf(available);
			DWORD read = 0;
			if (!WinHttpReadData(request, buf.data(), available, &read) || read == 0) break;
			out.body.append(buf.data(), read);
		}

		cleanup();
		return out;
	}

}

bool Http::handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	if (method != L"fetch") return false; // 交给调用方回 unknown method（同 Dialog）
	return fetch(params, page, result, frame);
}

bool Http::fetch(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto url = Json::getString(params, L"url", L"");
	if (url.empty())
	{
		Json::setError(result, L"http.invalidParam", L"url 不能为空");
		return false; // 当场就错：让调用方把这个 error 发回去
	}

	auto method = Json::getString(params, L"method", L"GET");
	if (method.empty()) method = L"GET";
	for (auto& c : method) c = static_cast<wchar_t>(std::towupper(c)); // WinHTTP 认大写

	// 请求头：{ k: v } → "k: v\r\n" 整块（WinHTTP 要 CRLF 分隔的一整块）
	std::wstring headersText;
	if (auto h = Json::getObject(params, L"headers"))
	{
		for (auto&& pair : h)
		{
			if (pair.Value().ValueType() != JsonValueType::String) continue; // 非字符串的跳过
			headersText += std::wstring{ pair.Key().c_str() } + L": "
				+ std::wstring{ pair.Value().GetString().c_str() } + L"\r\n";
		}
	}

	// 请求体按 UTF-8 编码送出去
	auto body = Util::wideToUtf8(Json::getString(params, L"body", L""));

	// 响应体怎么给：默认按 UTF-8 解成文本；base64 无损；binary 走共享内存
	auto encoding = Json::getString(params, L"encoding", L"utf8");

	// 交给 Async：doFetch 跑在后台线程上，data 回调回到 UI 线程才组装 JSON
	auto started = Async::run(page, result, frame,
		[url, method, headersText, body = std::move(body), encoding] {
		auto r = doFetch(url, method, headersText, body);
		if (!r.error.empty()) return AsyncResult::fail(r.error);
		// ok = 2xx / 3xx，跟 web fetch 一致；4xx / 5xx 也 resolve，不 reject
		auto ok = r.status >= 200 && r.status < 400;

		// binary：字节走共享内存，页面拿到 ArrayBuffer。不进 JSON，
		// 既省掉 base64 那 1/3 膨胀，也避免大响应体在 JSON 里被整块拷一次
		if (encoding == L"binary")
		{
			auto size = r.body.size();
			// 只捕获元数据、不捕获 body：大响应体不该在 UI 线程回调里再多留一份
			return AsyncResult::binary(std::vector<uint8_t>(r.body.begin(), r.body.end()),
				[status = r.status, statusText = r.statusText, headers = r.headers, ok, size] {
				JsonObject data;
				setMeta(data, status, statusText, headers, ok);
				data.SetNamedValue(L"size", JsonValue::CreateNumberValue(static_cast<double>(size)));
				return data;
			});
		}

		// base64 把原始字节无损编成字符串；utf8 直接解成文本（非 UTF-8 的内容会出乱码）
		auto text = Util::utf8ToWide(encoding == L"base64" ? Util::toBase64(r.body) : r.body);
		// 按拷贝捕获：std::function 要求可拷贝，搬进去就出不来了
		return AsyncResult::ok([status = r.status, statusText = r.statusText,
			headers = r.headers, ok, text = std::move(text)] {
			JsonObject data;
			setMeta(data, status, statusText, headers, ok);
			data.SetNamedValue(L"body", JsonValue::CreateStringValue(text));
			return data;
		});
	});
	if (!started)
	{
		Json::setError(result, L"http.createFailed", L"创建请求线程失败");
		return false; // 当场就错：让调用方把这个 error 发回去
	}
	return true; // 已在后台跑起来：响应稍后由 Async 发回，调用方不要再发一次
}
