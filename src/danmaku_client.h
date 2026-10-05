#pragma once
// ============================================================
// danmaku_client.h —— B站直播弹幕客户端
// 使用 WinHTTP 原生 WebSocket API（wss，TLS 由系统处理），
// 协议：16 字节头 + body；op=7 认证、op=2 心跳、op=5 推送；
// brotli/zlib 解压；断线指数退避重连（1s→2s→4s→…→30s 封顶）。
// 独立线程运行，消息经回调交给 UI 线程。
// ============================================================

#include <windows.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace bili { struct DanmakuConf; struct DanmakuServer; }

// 单条弹幕消息
struct DanmakuMessage {
    std::wstring text;   // 弹幕内容
    std::wstring user;   // 用户名
    long long uid = 0;
    ULONGLONG ts = 0;    // 到达时间（GetTickCount64），用于限流
};

// 连接状态回调
enum class DanmakuStatus {
    Connected,      // 已连接并认证成功
    Reconnecting,   // 断开，正在重连（指数退避中）
    AuthFailed,     // 认证失败（token 无效）
    Stopped,        // 已停止
};

class DanmakuClient {
public:
    using MsgCallback  = void (*)(const DanmakuMessage& msg, void* ctx);
    using StatusCallback = void (*)(DanmakuStatus st, void* ctx);

    DanmakuClient();
    ~DanmakuClient();

    // 启动：连接指定房间的弹幕服务器（conf 需已通过 GetDanmakuConf 获取）
    void Start(long long realRoomId,
               const bili::DanmakuConf& conf,
               const std::string& buvid3);

    // 停止并等待线程退出
    void Stop();

    bool IsRunning() const { return running_.load(); }

    void SetCallbacks(MsgCallback msg, StatusCallback st, void* ctx) {
        std::lock_guard<std::mutex> lk(cbMutex_);
        msgCb_ = msg; stCb_ = st; ctx_ = ctx;
    }

private:
    void ThreadMain();
    void ConnectOnce(const std::string& host, int wssPort);
    void NotifyStatus(DanmakuStatus st);

    std::thread thread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> stopRequested_{ false };

    long long roomId_ = 0;
    std::string token_;
    std::string buvid3_;
    std::vector<bili::DanmakuServer> servers_;  // 弹幕服务器（host+wss_port，故障转移轮询）

    std::mutex cbMutex_;
    MsgCallback msgCb_ = nullptr;
    StatusCallback stCb_ = nullptr;
    void* ctx_ = nullptr;
};
