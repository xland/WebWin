# 别在电话里开会：WebView2 里弹模态对话框的三种改法

> 本文与具体项目无关。你只需要知道 Win32 和 WebView2 就能读完。

---

## 序：一句话版本

WebView2 给你的 C++ 代码**打电话**（事件回调）时，你不能在电话里再**开一个会**（模态循环）。

先挂电话，再去开会。

"挂电话"的意思是：把要做的事记下来，等这次回调返回之后、回到消息循环里，再做。

## 一、你会遇到什么

你写了个 WebView2 应用，网页点了按钮，JS 给 C++ 发消息。C++ 收到后弹个消息框：

```cpp
// 注册 WebMessageReceived 回调
webview->add_WebMessageReceived(
    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
        [hwnd](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*) -> HRESULT
        {
            MessageBox(hwnd, L"你好", L"标题", MB_OK);   // ← 出事的就是这行
            return S_OK;
        }).Get(), nullptr);
```

一运行，程序直接断进调试器：

```
Running a message loop synchronously in an event handler in Webview can cause
reentrancy issue. Please refer to ...#re-entrancy for more information ...
已在 xxx.exe 中执行断点指令(__debugbreak()语句或类似调用)。
```

这不是崩溃，也不是你写错了，是 **WebView2 主动把你叫停的**：
它检测到你做了一件它不支持的事，宁可当场断下来，也不让你带着隐患继续跑。

> 如果日志里在它之前还有几条 `80004001 尚未实现`，
> 那是 SHELL32 自己的常规噪音，跟这件事无关，别被它带跑。

## 二、为什么

### 一次调用走过的路

```
网页 JS  postMessage
   ↓
WebView2 在 UI 线程上派发 WebMessageReceived
   ↓
你的 C++ 回调（事件处理器）
   ↓
MessageBox(...)                    ← 出事
```

### 问题出在 MessageBox 内部

`MessageBox` 不只是"画个框"。它要处理按钮点击、绘制、响应用户操作，
所以它**内部会跑一个消息循环**。

于是你这一行 `MessageBox` 之下，栈变成了这样：

```
WebView2 的回调      ──→      里面又开了一个消息循环
```

这叫**重入**。WebView2 要求它的回调**一进一出**：
回调还没返回，就不许在里面再起一个循环去调度消息。

微软把这个规则写成了明文：

> "Callbacks, including event handlers and completion handlers, run serially.
> After you run an event handler and begin a message loop, an event handler or
> completion callback cannot be run in a re-entrant manner."

### 哪些调用会踩

判据只有一条：**内部会起模态循环的调用，都不能在回调里直接用。**

| 会踩的 | 共同点 |
| --- | --- |
| `MessageBox` | 内部跑模态循环，等用户点按钮 |
| `IFileDialog::Show`（打开 / 保存文件） | 同上 |
| `TrackPopupMenu`（右键菜单） | 同上 |
| `GetOpenFileName` / `SHBrowseForFolder` | 同上 |
| 打印对话框 | 同上 |

**不会踩的**：`ShellExecute`（比如"在文件夹里显示该文件"）、普通的窗口操作。
它们不起循环，当场调用没问题。

### 顺带一提：这不只是 WebView2 的问题

任何"你被别人调用、且对方希望你尽快返回"的场合都可能有这条约束。
换成别的内嵌浏览器内核（CEF 等）也是同样的道理。

## 三、三种改法

下面三种都能让程序不崩。区别在于**代价**和**可维护性**。

### 第一种：给自己投一条消息，等到消息循环里再弹

思路：回调里**不弹**，只往自己的窗口投一条消息；
真正的弹框动作放到窗口过程里 —— 那里早已回到消息循环。

```cpp
#define WM_MY_SHOW_DIALOG (WM_APP + 1)

// 回调里：不弹，只投一条消息
webview->add_WebMessageReceived(
    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
        [hwnd](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*) -> HRESULT
        {
            PostMessage(hwnd, WM_MY_SHOW_DIALOG, 0, 0);
            return S_OK;
        }).Get(), nullptr);

// 窗口过程里：这里是正常消息流，可以弹
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_MY_SHOW_DIALOG:
        MessageBox(hwnd, L"你好", L"标题", MB_OK);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
```

**为什么这样就够了**：消息被 `GetMessage` 取走时，那个 WebView2 回调早就返回了，栈也退干净了。

**代价**：
- 每个要推迟的动作，都要一个 `WM_APP + N` 常量 + WndProc 里一个 case
- 参数只能塞进 `WPARAM` / `LPARAM`（64 位下各 8 字节）

### 第二种：消息只当通知，参数另找地方放

要传的东西塞不进那 8 个字节时（一堆字符串、整个 JSON），
就在自己那边摆个队列，消息只当"有活儿了"的门铃：

```cpp
struct Task { /* 一堆参数 */ };
std::deque<Task> g_pending;

// 回调里：参数进队列，消息只报个到
g_pending.push_back(task);
PostMessage(hwnd, WM_MY_SHOW_DIALOG, 0, 0);

// 窗口过程里
case WM_MY_SHOW_DIALOG:
{
    auto t = std::move(g_pending.front());
    g_pending.pop_front();
    DoTheRealWork(t);   // 这里才真的弹
    return 0;
}
```

**一个白捡的好处**：`PostMessage` 的消息是**挂在窗口上**的。
窗口在消息被取走之前销毁了，Windows 会把这条消息直接清掉，什么都不会发生。

### 第三种：DispatcherQueue（微软推荐的做法）

前两种都能用。真正的问题是：**每加一个要推迟的动作，都要去 WndProc 里插一个 case**，
而且每个都得自己发明一套传参方式。WndProc 渐渐变成公共垃圾桶。

微软文档在给出那个报错之后，紧接着写的就是：

> "Instead, **schedule the appropriate work to take place after completion of
> the event handler.**"

（它的示例用 `SynchronizationContext.Post`，那是 .NET 的说法；
在纯 Win32 里对应的东西就是 `DispatcherQueue`。）

#### 装

```cpp
#include <DispatcherQueue.h>        // um/ 的 Win32 API：CreateDispatcherQueueController
#include <winrt/Windows.System.h>   // C++/WinRT 投影：DispatcherQueueController

DispatcherQueueOptions options
{
    sizeof(DispatcherQueueOptions),
    DQTYPE_THREAD_CURRENT,   // 给**当前**线程装，不新开线程
    DQTAT_COM_NONE           // 对 DQTYPE_THREAD_CURRENT 无效，详见下方说明
};

winrt::Windows::System::DispatcherQueueController g_controller{ nullptr };

CreateDispatcherQueueController(
    options,
    reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(
        winrt::put_abi(g_controller)));
```

#### 用

```cpp
webview->add_WebMessageReceived(
    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*) -> HRESULT
        {
            // 只是"排进去"，不当场执行
            g_controller.DispatcherQueue().TryEnqueue([]
            {
                MessageBox(g_hwnd, L"你好", L"标题", MB_OK);   // 安全了
            });
            return S_OK;
        }).Get(), nullptr);
```

**为什么这样就不会重入** —— 决定性的一句在 Windows SDK 的
`um/DispatcherQueue.h` 里：

> "It is the responsibility of the caller to **run a NTUSER Message pump loop**
> so as to dispatch DispatcherQueue's tasks onto this thread."

翻译：**`TryEnqueue` 只排队，不当场执行**；
任务是由你这个线程的消息泵（就是 `GetMessage` 那个循环）分派的。
所以任务跑起来时，WebView2 的回调栈早就退干净了。

可以看出，它跟前两种**本质上是同一件事**，
只是"回到消息循环"这一步不用你手写 `PostMessage` 了。

**收益**：
- 不用定义 `WM_APP + N`
- 不用在 WndProc 里加 case
- 参数用 lambda 捕获，类型交给编译器检查

## 四、并排对比

| | 第一种 | 第二种 | 第三种 |
| --- | --- | --- | --- |
| 能解决重入 | 能 | 能 | 能 |
| WndProc 里要加 case | 每个动作一个 | 每个动作一个 | **不用** |
| 传参方式 | 塞进 WPARAM/LPARAM | 自建队列 | lambda 捕获 |
| 窗口关了，任务会自动消失 | **会**（系统保证） | **会** | **不会**（要自己补，见下） |
| 支持取消已排队任务 | 间接支持 | 自己清队列 | **不支持**（没有移除接口） |
| 系统要求 | 无 | 无 | Windows 10+ |
| 微软推荐 | — | — | **是** |

## 五、六个要留意的细节

**1. `TryEnqueue` 会返回 `false`，必须处理。**

队列还没建，或者已经关了。不处理的话，请求永远不回包，
页面里那个 Promise 会一直挂着，**而且没有任何报错**。

```cpp
if (!g_controller.DispatcherQueue().TryEnqueue([]
    {
        MessageBox(g_hwnd, L"你好", L"标题", MB_OK);
    }))
{
    // 别漏了这里
}
```

**2. DispatcherQueue 删不掉已排队的任务。**

没有 `TryDequeue` 之类的接口。所以窗口关了，任务照样跑，然后摸到已销毁的对象。
解决办法只有一条：**任务里先自己查一下依赖还在不在**。

```cpp
g_controller.DispatcherQueue().TryEnqueue([]
{
    if (!g_alive) return;              // 或 if (g_windows.count(id) == 0) return;
    MessageBox(g_hwnd, L"你好", L"标题", MB_OK);
});
```

注意这是从第一种的"系统保证"退化来的（消息挂在窗口上，窗口没了消息自动消失），
换成 DispatcherQueue 后**这层保护没了，得自己补上**。

**3. controller 必须活过所有排队任务。**

别把它做成某个函数里的局部对象，出了作用域任务就没地方跑了。
放在全局或长期存活的对象成员里。

**4. 同一线程上只能有一个 controller。**

SDK 头文件原话：*"API will throw if there already exists a
DispatcherQueueController on this thread."*

如果你的 WebView2 项目已经在别处（比如 Windows App SDK、或者为了别的用途）装过一个，
这里会失败。**先查一下你项目里是不是已经有了** —— 很多 WebView2 项目其实早就装了，
只是没想起来用，那就直接白捡。

**5. `apartmentType` 对 `DQTYPE_THREAD_CURRENT` 是无效的。**

头文件写得很清楚：*"Only relevant if threadType is DQTYPE_THREAD_DEDICATED"*。
所以别照抄别人代码里的 `DQTAT_COM_STA` —— 它只在专用线程模式下有意义。

**6. 它不帮你识别哪些 API 需要推迟。**

`DispatcherQueue` 只是让"推迟"这件事变便宜了。
**哪些 Win32 API 内部会起模态循环，仍然得你自己知道。**

建议把这张表贴在代码里，这样下一个人不用靠 `__debugbreak()` 重新发现一遍。

## 六、还有一条弯路：别顺手扔到后台线程

有人第一反应是：起个 `std::thread` 去弹不就行了？

不太行，两个理由：

- **WebView2 的接口只能在创建它的那个线程上调。**
  你在后台线程弹完了，结果还得 `PostMessage` 送回来 —— 横竖都要回来，绕了一圈。
- **跨线程传 `HWND` 当属主**，模态层级和父窗口禁用关系都不如同线程可靠。

既然最后总要回到 UI 线程，不如一开始就原地推迟。

## 七、怎么选

- **Windows 10 以上，且还没装 DispatcherQueue** → 直接第三种，记得补上第 1、2 条。
- **你项目里其实已经装了** → 白捡，直接用。
- **第一种不是错**：只有一个动作要推迟时，一个 `PostMessage` 加一个 case 也很清楚。
  只是别让它发展成"每加一个功能就插一个 case"。

## 八、一句总结

**回调是用来尽快返回的，不是用来干重活的。**
要干重活，就把它排到下一次消息循环去。

---

## 附录：本文做法在 WebWin 里的落点（仅本项目）

| 本文 | 本项目 |
| --- | --- |
| 第三种（DispatcherQueue） | `App::runLater(fn)`，底层是 `App::initDispatcherQueueCtrl()` 建的 controller |
| 右键菜单 | `Menu` 域，`TrackPopupMenu` 走 `App::runLater` |
| 文件框 / 消息框 | `Dialog` 域，走 `App::runLater` + 自己的 `pending` 队列 |

`Dialog` 之所以在 `App::runLater` 之外还保留自己的队列，
就是为了拿到 DispatcherQueue 给不了的两样东西：**持有载荷**和**能被提前撤销**。
