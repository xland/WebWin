/**
 * WebWin JS SDK —— 通信基类
 *
 * 开发者不会直接操作本类：通道方法是 protected，也不出现在 .d.ts 的公开面上。
 * 各能力域（Win / Clipboard / Dialog ...）都继承它，对外只暴露语义化的方法。
 *
 * 消息协议（见 arch/02-architecture.md）：
 *   上行请求 { id, type, method, params, winId? }
 *   下行响应 { id, ok, data | error }
 *   下行事件 { name, payload, winId? }
 *
 * type 是能力域（`win` / `dialog` ...），C++ 按它把消息路由到对应的域处理器。
 * winId 是消息的路由凭据：一条消息从 A 页面发出时，C++ 靠它知道要操作哪个窗口。
 */
/** 框架注入 SDK 前写入 window 的配置 */
export interface WebWinConfig {
    allowRemote: boolean;
    origin: string;
    /** 当前窗口的 id，由 C++ 注入；SDK 据此构造全局 win 对象 */
    winId?: number;
}
export type Payload = unknown;
export type Handler<T = unknown> = (payload: T) => void;
/**
 * C++ 侧下行的 error 字段。
 * 目前是字符串（错误码表尚未定义），将来会改成 { code, message }，故两种都兼容。
 */
export type WebWinError = string | {
    code?: string;
    message?: string;
};
/**
 * 建立通道并注册接收。成功返回注入的配置，失败返回 null（此时不应安装 SDK）。
 * @internal
 */
export declare function installChannel(): WebWinConfig | null;
export declare class Ipc {
    /**
     * 能力域标识（`win` / `dialog` / `clipboard` ...），子类覆盖。
     * C++ 侧按它把消息路由到对应的域处理器；事件订阅也靠它带上域信息。
     */
    protected type: string;
    /** 关联的窗口 id；null 表示与窗口无关（clipboard / dialog 这类单例能力） */
    protected winId: number | null;
    /**
     * 调用 C++ 暴露的能力：type 指能力域，method 指该域下的具体动作。
     * additionalObjects 用于携带 File 等无法 JSON 序列化的对象
     */
    protected invoke<T = unknown>(type: string, method: string, params?: Payload, additionalObjects?: unknown[]): Promise<T>;
    /**
     * 调本域的方法：type 取 `this.type`，不必在每个方法里重复写域名。
     * 能力域里绝大多数调用都走这里；`invoke` 留给需要显式指定域的场合。
     */
    protected call<T = unknown>(method: string, params?: Payload, additionalObjects?: unknown[]): Promise<T>;
    /** 订阅事件；返回取消订阅的函数 */
    protected on<T = unknown>(name: string, handler: Handler<T>): () => void;
    /** 取消订阅 */
    protected off<T = unknown>(name: string, handler: Handler<T>): void;
    /** 供子类在还没有实例时发请求（如 Win.create 要先拿到窗口 id） */
    protected static request<T = unknown>(type: string, method: string, params?: Payload, winId?: number | null, additionalObjects?: unknown[]): Promise<T>;
}
