// winsock2 必须排在 Windows.h 之前（Net.h 会带上 Windows.h），否则两个版本的
// socket 定义会打架。iphlpapi 依赖它，所以整组都放在最前面（同 Os.cpp 当年的做法）
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <cstring>
#include <vector>

// JsonArray::Append 是 IVector 的方法，要显式带上集合头文件（Os.cpp / Json.cpp 同理）
#include <winrt/Windows.Foundation.Collections.h>

#include "Net.h"
#include "Json.h"
#include "Util.h"

void Net::handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	if (method == L"getAddress") getAddress(result);
	else Json::setError(result, L"unknown method: " + method);
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
