#include "Async.h"

#include <cstring>

// resume_background / fire_and_forget 在 base.h；
// resume_foreground(DispatcherQueue) 那个重载要靠 Windows.System.h
#include <winrt/base.h>
#include <winrt/Windows.System.h>

#include "App.h"
#include "Env.h"
#include "Json.h"
#include "Util.h"

// 说明见 Async.h 头部的流程图，这里只讲实现细节
namespace
{
	struct Pending
	{
		Page* page{ nullptr };
		ComPtr<ICoreWebView2Frame2> frame;
		JsonObject result{ nullptr }; // 引用语义：就是外面那个信封
	};

	std::mutex mtx;
	int lastId{ 0 };
	std::map<int, Pending> pendings;

	/**
	 * 大块数据走共享内存（照 Display::getImage 的顺序）。
	 * result 里已经是完整的 { id, ok, data } 信封，当 additionalData 一起带过去，
	 * 页面靠其中的 id 把 buffer 关联回这一次调用。
	 */
	bool sendBuffer(Page* page, ICoreWebView2Frame2* frame, JsonObject& result, const std::vector<uint8_t>& bytes)
	{
		ComPtr<ICoreWebView2Environment12> env12;
		auto env = Env::getEnv();
		if (!env || FAILED(env->QueryInterface(IID_PPV_ARGS(&env12)))) return false;

		ComPtr<ICoreWebView2SharedBuffer> buffer;
		if (FAILED(env12->CreateSharedBuffer(bytes.size(), &buffer))) return false;
		BYTE* data = nullptr;
		if (FAILED(buffer->get_Buffer(&data)) || !data) return false;
		std::memcpy(data, bytes.data(), bytes.size());

		auto json = result.Stringify();
		auto hr = page->postSharedBuffer(buffer.Get(), json.c_str(), frame);
		// 顺序不能反：先 Post 再 Close。已经 Close 过的 buffer 再 Post 会直接返回 RO_E_CLOSED
		buffer->Close();
		return SUCCEEDED(hr);
	}

	/**
	 * 把结果装进信封发回页面。**只**在 UI 线程上调用 ——
	 * 这也是"页面还在不在"能在这里安全判定的原因
	 */
	void finish(int id, AsyncResult r)
	{
		Pending p;
		{
			std::lock_guard<std::mutex> lock(mtx);
			auto it = pendings.find(id);
			if (it == pendings.end()) return; // 页面已销毁：被 removeByPage 清掉了
			p = std::move(it->second);
			pendings.erase(it);
		} // 锁要在发回之前放掉：发回可能连锁触发新任务，别自己把自己锁住

		if (!r.error.empty())
		{
			Json::setError(p.result, r.error);
			p.page->send(p.result, p.frame.Get());
			return;
		}

		Json::setOk(p.result);
		if (r.data) p.result.SetNamedValue(L"data", r.data());

		// 走共享内存；发不出去要转成 error，不能退化成普通响应 ——
		// 那样页面拿到的是一个没有 buffer 字段的对象，调用方无从判断。
		// 判据是 viaBuffer 而不是 buffer 是否为空：0 字节也要给一个空 ArrayBuffer
		if (r.viaBuffer)
		{
			if (sendBuffer(p.page, p.frame.Get(), p.result, r.buffer)) return;
			Json::setError(p.result, L"发共享内存失败（数据过大或 WebView2 版本不支持）");
		}
		p.page->send(p.result, p.frame.Get());
	}

	/**
	 * 一个异步任务的完整一生：去线程池干活 → 回到 UI 线程回包。
	 *
	 * 用 fire_and_forget 而不是 std::thread，有两个实打实的好处：
	 * 后台那段交给 **WinRT 线程池**（线程复用，不必每个请求开一条 OS 线程），
	 * 而且不用 detach —— 没有"进程在退、线程还在跑"那一档事。
	 *
	 * **异常绝不能逃出这里**：fire_and_forget 里没被接住的异常会直接 terminate()，
	 * 整个进程崩掉。所以每一步都兜住，把异常转成 error 发回页面 ——
	 * 页面里那个 Promise 总归要有个交代
	 */
	winrt::fire_and_forget runTask(int id, std::function<AsyncResult()> work)
	{
		AsyncResult r;
		try
		{
			co_await winrt::resume_background(); // 切到线程池：work 在那儿跑
			r = work();
		}
		catch (const std::exception& e)
		{
			r = AsyncResult::fail(L"后台任务抛异常：" + Util::utf8ToWide(e.what()));
		}
		catch (...)
		{
			r = AsyncResult::fail(L"后台任务抛出未知异常");
		}

		// 回到 UI 线程：finish 要在那儿查"页面还在不在"，page->send 也必须在那儿调。
		// 明确指名 DispatcherQueue，不用 apartment_context —— 理由见 App::dispatcherQueue
		try
		{
			co_await winrt::resume_foreground(App::dispatcherQueue());
			finish(id, std::move(r));
		}
		catch (...)
		{
			// 排不进队列（应用正在退）：已经没人会收到回包了，撤掉这条免得占着表
			std::lock_guard<std::mutex> lock(mtx);
			pendings.erase(id);
		}
	}
}

AsyncResult AsyncResult::fail(std::wstring message)
{
	AsyncResult r;
	r.error = std::move(message);
	return r;
}

AsyncResult AsyncResult::ok(std::function<IJsonValue()> data)
{
	AsyncResult r;
	r.data = std::move(data);
	return r;
}

AsyncResult AsyncResult::binary(std::vector<uint8_t> bytes, std::function<IJsonValue()> data)
{
	AsyncResult r;
	r.buffer = std::move(bytes);
	r.viaBuffer = true;
	r.data = std::move(data);
	return r;
}

bool Async::run(Page* page, JsonObject& result, ICoreWebView2Frame2* frame, std::function<AsyncResult()> work)
{
	// 队列都没建好就别开工：任务跑完了回不来，页面里那个 Promise 会永远挂着且没报错
	if (!App::dispatcherQueue()) return false;

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

	// fire_and_forget：自己跑到完，不用 join。参数会被拷进协程帧，所以 move 进去是安全的
	runTask(id, std::move(work));
	return true; // 已排上：响应稍后由 finish() 发回
}

void Async::removeByPage(Page* page)
{
	std::lock_guard<std::mutex> lock(mtx);
	std::erase_if(pendings, [page](const auto& kv) { return kv.second.page == page; });
}
