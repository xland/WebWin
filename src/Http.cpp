#include "Http.h"

#include <algorithm>
#include <cwctype>
#include <thread>

#include "App.h"
#include "Json.h"
#include "Util.h"

/**
 * http 域的实现。
 *
 * 数据流向：
 *   JS Http.fetch → handleApi（UI 线程）→ 登记 pending → std::thread
 *                                                        ↓
 *                                            doFetch（后台线程，WinHTTP 同步 API）
 *                                                        ↓
 *                            App::runLater → finish（回到 UI 线程）→ page->send
 *
 * 两条硬约束，顺序不能反：
 * 1. 网络请求绝不能在 UI 线程上跑（会卡死窗口），所以必须开线程；
 * 2. page->send 必须在 UI 线程上跑（WebView2 的要求），所以回来要走 runLater。
 *
 * 而"页面还在不在"这件事**必须在 UI 线程上查**：Page 的销毁（Win 的 WM_DESTROY
 * → Http::removeByPage）也发生在 UI 线程上，两者互斥。因此在 finish() 里查到的
 * Page* 一定是有效的 —— 要是在后台线程里查，查完到 send 之间窗口可能就没了。
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

	/** 一个"正在等回包"的请求 */
	struct Pending
	{
		Page* page{ nullptr };
		ComPtr<ICoreWebView2Frame2> frame;
		JsonObject result{ nullptr };
	};

	std::mutex mtx;
	int lastId{ 0 };
	std::map<int, Pending> pendings;

	/** 掐掉首尾空白和 CR */
	std::wstring trim(const std::wstring& s)
	{
		size_t b = 0, e = s.size();
		while (b < e && (s[b] == L' ' || s[b] == L'\t')) ++b;
		while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r')) --e;
		return s.substr(b, e - b);
	}

	/**
	 * 错误码转成能看的消息。
	 * WinHTTP 自己的错误码不在系统消息表里，FormatMessage 常常查不出来，
	 * 所以**一定把数字带上** —— 那才是能拿去查的东西
	 */
	std::wstring winErr(DWORD code)
	{
		LPWSTR buf = nullptr;
		auto n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
		auto msg = (n && buf) ? trim(std::wstring{ buf, n }) : std::wstring{};
		if (buf) LocalFree(buf);
		if (msg.empty()) msg = L"未知错误";
		return msg + L"（WinHTTP 错误码 " + std::to_wstring(code) + L"）";
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
			out.error = what + L"：" + winErr(GetLastError());
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

	/**
	 * 回到 UI 线程后把结果装进信封发出去。**只**由 App::runLater 排下的任务调用。
	 * 必须在这里（UI 线程上）查 pending 还在不在 —— 见本文件开头的说明
	 */
	void finish(int id, const FetchResult& r)
	{
		Pending p;
		{
			std::lock_guard<std::mutex> lock(mtx);
			auto it = pendings.find(id);
			if (it == pendings.end()) return; // 页面已销毁：被 removeByPage 清掉了
			p = std::move(it->second);
			pendings.erase(it);
		} // 锁要在 send 之前放掉：send 可能连锁触发新请求，别自己把自己锁住

		if (!r.error.empty())
		{
			Json::setError(p.result, r.error);
			p.page->send(p.result, p.frame.Get());
			return;
		}

		JsonObject headers;
		for (const auto& [k, v] : r.headers)
			headers.SetNamedValue(k.c_str(), JsonValue::CreateStringValue(v.c_str()));

		JsonObject data;
		data.SetNamedValue(L"status", JsonValue::CreateNumberValue(r.status));
		data.SetNamedValue(L"statusText", JsonValue::CreateStringValue(r.statusText));
		data.SetNamedValue(L"headers", headers);
		// 响应体按 UTF-8 解：非 UTF-8 的内容（比如 GBK 页面）会出乱码，v1 不管
		data.SetNamedValue(L"body", JsonValue::CreateStringValue(Util::utf8ToWide(r.body)));
		// ok = 2xx / 3xx，跟 web fetch 一致；4xx / 5xx 也 resolve，不 reject
		data.SetNamedValue(L"ok", JsonValue::CreateBooleanValue(r.status >= 200 && r.status < 400));

		Json::setOk(p.result);
		p.result.SetNamedValue(L"data", data);
		p.page->send(p.result, p.frame.Get());
	}
}

bool Http::handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	if (method != L"fetch") return false; // 交给调用方回 unknown method（同 Dialog）
	return fetch(params, page, result, frame);
}

void Http::removeByPage(Page* page)
{
	std::lock_guard<std::mutex> lock(mtx);
	std::erase_if(pendings, [page](const auto& kv) { return kv.second.page == page; });
}

bool Http::fetch(const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	auto url = Json::getString(params, L"url", L"");
	if (url.empty())
	{
		Json::setError(result, L"url 不能为空");
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

	int id = 0;
	{
		std::lock_guard<std::mutex> lock(mtx);
		id = ++lastId;
		Pending p;
		p.page = page;
		p.frame = frame;
		p.result = result; // JsonObject 是引用语义：这就是外面那个信封
		pendings.emplace(id, std::move(p));
	}

	try
	{
		// detach 是因为没人等它 —— 结果靠 runLater 送回 UI 线程
		std::thread([id, url, method, headersText, body = std::move(body)] {
			// shared_ptr 是因为 std::function 要求可拷贝，而 FetchResult 搬进去就出不来了
			auto r = std::make_shared<FetchResult>(doFetch(url, method, headersText, body));
			if (!App::runLater([id, r] { finish(id, *r); }))
			{
				// 排不进去（应用正在退）：把这条撤了，别让它占着表
				std::lock_guard<std::mutex> lock(mtx);
				pendings.erase(id);
			}
		}).detach();
	}
	catch (...)
	{
		std::lock_guard<std::mutex> lock(mtx);
		pendings.erase(id);
		Json::setError(result, L"创建请求线程失败");
		return false;
	}
	return true; // 已在后台跑起来：响应稍后由 finish() 发回
}
