// winsock2 必须排在 Windows.h 之前（Net.h 会带上 Windows.h），否则两个版本的
// socket 定义会打架。iphlpapi 依赖它，所以整组都放在最前面（同 Os.cpp 当年的做法）
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <cstring>
#include <vector>
#include <string>
#include <map>
#include <mutex>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>

// JsonArray::Append 是 IVector 的方法，要显式带上集合头文件（Os.cpp / Json.cpp 同理）
#include <winrt/Windows.Foundation.Collections.h>

#include "Net.h"
#include "Json.h"
#include "Util.h"
#include "App.h"   // App::runLater：把事件下行排到 UI 线程（fireEvent 必须在 UI 线程）
#include "Win.h"   // Win::fireEvent / subscribeEvent / NO_WINDOW

void Net::handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	if (method == L"getAddress") { getAddress(result); return; }

	// ---- 命名管道 ----
	if (method == L"createNamedPipeServer") { createNamedPipeServer(params, page, result); return; }
	if (method == L"createNamedPipeClient") { createNamedPipeClient(params, page, result); return; }
	if (method == L"send") { sendData(params, result); return; }
	if (method == L"close") { closeConn(params, result); return; }
	// net 域的事件订阅：委托到 NO_WINDOW（与窗口无关，同 app / fs 域的做法）。
	// 隐藏事件 data / connect / close / error 就靠它下发到 JS
	if (method == L"on") { Win::subscribeEvent(Win::NO_WINDOW, Json::getString(params, L"name", L""), page, frame); Json::setOk(result); return; }
	if (method == L"off") { Win::unsubscribeEvent(Win::NO_WINDOW, Json::getString(params, L"name", L""), page, frame); Json::setOk(result); return; }

	Json::setError(result, L"unknown method: " + method);
}

void Net::getAddress(JsonObject& result)
{
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { Json::setError(result, L"初始化 Winsock 失败"); return; }

	// 先按经验值申请，不够时 GetAdaptersAddresses 会回填所需大小，再取一次
	ULONG size = 15000;
	std::vector<char> buffer(size);
	auto adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
	auto code = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &size);
	if (code == ERROR_BUFFER_OVERFLOW)
	{
		buffer.resize(size);
		adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
		code = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &size);
	}
	if (code != ERROR_SUCCESS)
	{
		WSACleanup();
		Json::setError(result, L"取网卡信息失败");
		return;
	}

	JsonArray list;
	for (auto adapter = adapters; adapter; adapter = adapter->Next)
	{
		if (adapter->OperStatus != IfOperStatusUp) continue; // 未启用的网卡
		if (adapter->PhysicalAddressLength == 0) continue;   // 没有 MAC：回环、虚拟网卡之类

		JsonObject item;
		item.SetNamedValue(L"name", JsonValue::CreateStringValue(adapter->FriendlyName));
		wchar_t mac[18];
		swprintf_s(mac, L"%02X-%02X-%02X-%02X-%02X-%02X",
			adapter->PhysicalAddress[0], adapter->PhysicalAddress[1], adapter->PhysicalAddress[2],
			adapter->PhysicalAddress[3], adapter->PhysicalAddress[4], adapter->PhysicalAddress[5]);
		item.SetNamedValue(L"mac", JsonValue::CreateStringValue(mac));

		JsonArray addresses;
		for (auto unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next)
		{
			auto addr = unicast->Address.lpSockaddr;
			char text[INET6_ADDRSTRLEN];
			if (addr->sa_family == AF_INET)
					{
				auto ipv4 = reinterpret_cast<sockaddr_in*>(addr);
				if (!inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text))) continue;
				if (std::strcmp(text, "127.0.0.1") == 0) continue; // 回环地址不算
			}
			else if (addr->sa_family == AF_INET6)
			{
				auto ipv6 = reinterpret_cast<sockaddr_in6*>(addr);
				if (!inet_ntop(AF_INET6, &ipv6->sin6_addr, text, sizeof(text))) continue;
				if (std::strcmp(text, "::1") == 0) continue;
			}
			else continue;
			addresses.Append(JsonValue::CreateStringValue(Util::utf8ToWide(text)));
		}
		item.SetNamedValue(L"addresses", addresses);
		list.Append(item);
	}

	WSACleanup();
	Json::setOk(result);
	result.SetNamedValue(L"data", list);
}

// =============================================================================
// 命名管道（pipe）实现 —— 与将来的 socket 逻辑隔离：这里只管 pipe。
// 复用本文件外层的 connId 路由 + 事件下行（net 域隐藏事件）；socket 将来加一块，
// 走同样的 connId / 事件，不动这里的代码。
//
// 协议：每条消息 = 4 字节小端长度前缀 + payload 字节（send 端按 encoding 决定 payload）。
// 多客户端 server：用重叠 IO 的监听实例，每接受一个客户端就连一个独立读线程。
// =============================================================================
namespace {

	struct Conn;
	using ConnPtr = std::shared_ptr<Conn>;
	struct ServerState;
	using ServerPtr = std::shared_ptr<ServerState>;

	struct Conn
	{
		int id = 0;
		Page* page = nullptr;
		std::wstring recvEncoding = L"utf8";
		HANDLE handle = INVALID_HANDLE_VALUE; // 已连接的管道句柄（重叠）
		HANDLE stopEvent = nullptr;           // 唤醒读线程，使其退出
		std::mutex writeMutex;                // 写串行化
		std::vector<uint8_t> readBuf;         // 帧重组缓冲（单读线程访问）
		std::atomic<bool> closing{ false };
		std::thread reader;
		ServerPtr server;                     // 若是 server 接受的客户端连接，指向所属 server
	};

	struct ServerState
	{
		int id = 0;
		Page* page = nullptr;
		std::wstring pipeName;                // 完整 \\\\.\\pipe\\name
		std::wstring recvEncoding = L"utf8";
		DWORD bufSize = 4096;
		HANDLE stopEvent = nullptr;           // 唤醒 acceptor
		std::atomic<bool> closing{ false };
		std::thread acceptor;
		std::mutex clientsMutex;
		std::vector<ConnPtr> clients;         // 已接受的客户端连接
	};

	std::mutex g_mux;
	std::map<int, ConnPtr> g_conns;     // client 实例 / server 上客户端连接的 connId -> Conn
	std::map<int, ServerPtr> g_servers; // server connId -> ServerState
	int g_connSeq = 0;

	// ---- 事件下行（统一走 net 域隐藏事件，必须在 UI 线程 fire）----
	JsonObject makeConnId(int id)
	{
		JsonObject o{ nullptr };
		o.SetNamedValue(L"connId", JsonValue::CreateNumberValue(static_cast<double>(id)));
		return o;
	}
	void fireData(int id, const std::wstring& data)
	{
		auto o = makeConnId(id);
		o.SetNamedValue(L"data", JsonValue::CreateStringValue(data));
		App::runLater([o]() { Win::fireEvent(Win::NO_WINDOW, L"data", o); });
	}
	void fireConnect(int serverId, int clientId)
	{
		auto o = makeConnId(serverId);
		o.SetNamedValue(L"clientConnId", JsonValue::CreateNumberValue(static_cast<double>(clientId)));
		App::runLater([o]() { Win::fireEvent(Win::NO_WINDOW, L"connect", o); });
	}
	void fireClose(int id)
	{
		auto o = makeConnId(id);
		App::runLater([o]() { Win::fireEvent(Win::NO_WINDOW, L"close", o); });
	}
	void fireError(int id, const std::wstring& msg)
	{
		auto o = makeConnId(id);
		o.SetNamedValue(L"message", JsonValue::CreateStringValue(msg));
		App::runLater([o]() { Win::fireEvent(Win::NO_WINDOW, L"error", o); });
	}

	// 把 readBuf 里能拼出的完整帧逐个下发（单读线程调用，无需锁）
	void parseFrames(ConnPtr c)
	{
		auto& b = c->readBuf;
		while (b.size() >= 4)
		{
			DWORD len = b[0] | (static_cast<DWORD>(b[1]) << 8)
				| (static_cast<DWORD>(b[2]) << 16) | (static_cast<DWORD>(b[3]) << 24);
			if (b.size() < 4 + static_cast<size_t>(len)) break; // 帧还没收全
			std::vector<uint8_t> payload(b.begin() + 4, b.begin() + 4 + len);
			b.erase(b.begin(), b.begin() + 4 + len);

			// 按接收编码把 payload 字节呈现成字符串（binary 当前降级为 base64 字符串）
			std::string out;
			if (c->recvEncoding == L"utf8")
				out.assign(payload.begin(), payload.end());
			else
				out = Util::toBase64(std::string(payload.begin(), payload.end()));
			fireData(c->id, Util::utf8ToWide(out));
		}
	}

	void connReader(ConnPtr c)
	{
		uint8_t buf[8192];
		OVERLAPPED ol{};
		ol.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		while (!c->closing.load())
		{
			DWORD got = 0;
			ResetEvent(ol.hEvent);
			if (!ReadFile(c->handle, buf, sizeof(buf), &got, &ol))
			{
				if (GetLastError() != ERROR_IO_PENDING) break;
				HANDLE waits[] = { c->stopEvent, ol.hEvent };
				DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
				if (r != WAIT_OBJECT_0 + 1) break; // stopEvent 或异常
				if (!GetOverlappedResult(c->handle, &ol, &got, FALSE)) break;
			}
			if (got == 0) break;
			c->readBuf.insert(c->readBuf.end(), buf, buf + got);
			parseFrames(c);
		}
		CloseHandle(ol.hEvent);
		bool wasClosing = c->closing.exchange(true);
		if (!wasClosing) // 被动断开：先 error 再 close
			fireError(c->id, L"连接已断开");
		fireClose(c->id);
	}

	// 关闭一个连接（不碰 g_mux 之外的锁；调用方负责从 g_conns 删除）
	void closeConnPtr(const ConnPtr& c)
	{
		if (c->closing.exchange(true)) return; // 已关
		if (c->stopEvent) SetEvent(c->stopEvent);
		CancelIo(c->handle);
		if (c->reader.joinable()) c->reader.join();
		CloseHandle(c->handle);
		if (c->server)
		{
			std::lock_guard<std::mutex> lk(c->server->clientsMutex);
			auto& v = c->server->clients;
			v.erase(std::remove(v.begin(), v.end(), c), v.end());
		}
	}

	void closeServer(const ServerPtr& s)
	{
		if (s->closing.exchange(true)) return;
		if (s->stopEvent) SetEvent(s->stopEvent);
		// 先关所有客户端连接
		std::vector<ConnPtr> clients;
		{
			std::lock_guard<std::mutex> lk(s->clientsMutex);
			clients = s->clients;
		}
		for (auto& c : clients) closeConnPtr(c);
		if (s->acceptor.joinable()) s->acceptor.join();
		if (s->stopEvent) CloseHandle(s->stopEvent);
	}

	void serverAcceptLoop(ServerPtr s)
	{
		while (!s->closing.load())
		{
			HANDLE h = CreateNamedPipe(
				s->pipeName.c_str(),
				PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
				PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
				PIPE_UNLIMITED_INSTANCES, s->bufSize, s->bufSize, 0, nullptr);
			if (h == INVALID_HANDLE_VALUE) break;

			OVERLAPPED ol{};
			ol.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
			if (!ConnectNamedPipe(h, &ol))
			{
				DWORD e = GetLastError();
				if (e != ERROR_IO_PENDING && e != ERROR_PIPE_CONNECTED)
				{
					CloseHandle(ol.hEvent); CloseHandle(h);
					if (s->closing.load()) break;
					continue; // 罕见错误，重试
				}
			}
			HANDLE waits[] = { s->stopEvent, ol.hEvent };
			DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
			if (r == WAIT_OBJECT_0) // 被要求停止
			{
				CancelIo(h); CloseHandle(ol.hEvent); CloseHandle(h);
				break;
			}
			// 有客户端连上：h 成为已连接实例（重叠句柄），交给一个客户端连接
			CloseHandle(ol.hEvent);

			ConnPtr c;
			int cid;
			{
				std::lock_guard<std::mutex> lk(g_mux);
				cid = ++g_connSeq;
				c = std::make_shared<Conn>();
				c->id = cid;
				c->page = s->page;
				c->recvEncoding = s->recvEncoding;
				c->handle = h;
				c->stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
				c->server = s;
				g_conns[cid] = c;
				std::lock_guard<std::mutex> slk(s->clientsMutex);
				s->clients.push_back(c);
			}
			c->reader = std::thread(connReader, c);
			fireConnect(s->id, cid);
		}
	}

} // namespace

void Net::createNamedPipeServer(const JsonObject& params, Page* page, JsonObject& result)
{
	auto name = Json::getString(params, L"name", L"");
	if (name.empty()) { Json::setError(result, L"name 不能为空"); return; }
	auto encoding = Json::getString(params, L"encoding", L"utf8");
	auto bufSize = Json::getInt(params, L"bufferSize", 4096);

	ServerPtr s = std::make_shared<ServerState>();
	s->page = page;
	s->pipeName = L"\\\\.\\pipe\\" + name;
	s->recvEncoding = encoding;
	s->bufSize = bufSize > 0 ? static_cast<DWORD>(bufSize) : 4096;
	s->stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

	int sid;
	{
		std::lock_guard<std::mutex> lk(g_mux);
		sid = ++g_connSeq;
		s->id = sid;
		g_servers[sid] = s;
	}
	s->acceptor = std::thread(serverAcceptLoop, s);

	Json::setOk(result);
	JsonObject data;
	data.SetNamedValue(L"connId", JsonValue::CreateNumberValue(static_cast<double>(sid)));
	result.SetNamedValue(L"data", data);
}

void Net::createNamedPipeClient(const JsonObject& params, Page* page, JsonObject& result)
{
	auto name = Json::getString(params, L"name", L"");
	if (name.empty()) { Json::setError(result, L"name 不能为空"); return; }
	auto encoding = Json::getString(params, L"encoding", L"utf8");

	std::wstring pipeName = L"\\\\.\\pipe\\" + name;
	HANDLE h = CreateFile(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
		nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
	if (h == INVALID_HANDLE_VALUE)
	{
		Json::setError(result, L"连不上命名管道（服务端没起？）：" + name);
		return;
	}

	ConnPtr c;
	int cid;
	{
		std::lock_guard<std::mutex> lk(g_mux);
		cid = ++g_connSeq;
		c = std::make_shared<Conn>();
		c->id = cid;
		c->page = page;
		c->recvEncoding = encoding;
		c->handle = h;
		c->stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		g_conns[cid] = c;
	}
	c->reader = std::thread(connReader, c);

	Json::setOk(result);
	JsonObject data;
	data.SetNamedValue(L"connId", JsonValue::CreateNumberValue(static_cast<double>(cid)));
	result.SetNamedValue(L"data", data);
	fireConnect(cid, cid); // 客户端实例：自身已连接
}

void Net::sendData(const JsonObject& params, JsonObject& result)
{
	auto connId = static_cast<int>(Json::getInt(params, L"connId", 0));
	auto data = Json::getString(params, L"data", L"");
	auto encoding = Json::getString(params, L"encoding", L"utf8");

	ConnPtr c;
	{
		std::lock_guard<std::mutex> lk(g_mux);
		auto it = g_conns.find(connId);
		if (it == g_conns.end()) { Json::setError(result, L"连接不存在或已关闭"); return; }
		c = it->second;
	}

	// 按发送编码构造 payload 字节
	std::vector<uint8_t> payload;
	if (encoding == L"base64")
	{
		auto bytes = Util::fromBase64(Util::wideToUtf8(data));
		payload.assign(bytes.begin(), bytes.end());
	}
	else // utf8（默认）
	{
		auto u = Util::wideToUtf8(data);
		payload.assign(u.begin(), u.end());
	}

	// 帧 = 4 字节小端长度 + payload
	std::vector<uint8_t> frame;
	DWORD len = static_cast<DWORD>(payload.size());
	frame.push_back(static_cast<uint8_t>(len & 0xFF));
	frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
	frame.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
	frame.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
	frame.insert(frame.end(), payload.begin(), payload.end());

	std::lock_guard<std::mutex> wl(c->writeMutex);
	DWORD wrote = 0;
	if (!WriteFile(c->handle, frame.data(), static_cast<DWORD>(frame.size()), &wrote, nullptr))
	{
		Json::setError(result, L"发送失败");
		return;
	}
	Json::setOk(result);
}

void Net::closeConn(const JsonObject& params, JsonObject& result)
{
	auto connId = static_cast<int>(Json::getInt(params, L"connId", 0));
	ConnPtr c;
	ServerPtr s;
	{
		std::lock_guard<std::mutex> lk(g_mux);
		auto it = g_conns.find(connId);
		if (it != g_conns.end()) c = it->second;
		auto sit = g_servers.find(connId);
		if (sit != g_servers.end()) s = sit->second;
	}
	if (c)
	{
		closeConnPtr(c);
		std::lock_guard<std::mutex> lk(g_mux);
		g_conns.erase(connId);
		Json::setOk(result);
		return;
	}
	if (s)
	{
		closeServer(s);
		std::lock_guard<std::mutex> lk(g_mux);
		g_servers.erase(connId);
		Json::setOk(result);
		return;
	}
	Json::setError(result, L"连接不存在或已关闭");
}

void Net::removeByPage(Page* page)
{
	std::vector<ConnPtr> toClose;
	std::vector<ServerPtr> toCloseS;
	{
		std::lock_guard<std::mutex> lk(g_mux);
		for (auto& kv : g_conns)
			if (kv.second->page == page && !kv.second->server) toClose.push_back(kv.second);
		for (auto& kv : g_servers)
			if (kv.second->page == page) toCloseS.push_back(kv.second);
	}
	for (auto& c : toClose) closeConnPtr(c);
	for (auto& s : toCloseS) closeServer(s);
	std::lock_guard<std::mutex> lk(g_mux);
	for (auto& c : toClose) g_conns.erase(c->id);
	for (auto& s : toCloseS) g_servers.erase(s->id);
}
