#pragma once
// ============================================================
// player.h —— libmpv 播放器封装
// 渲染方案（已核实）：标准 libmpv 无 D3D11 render API，
// 采用官方推荐的 --wid=<窗口句柄> 嵌入方式，配合
// vo=gpu / gpu-api=d3d11 / hwdec=auto 实现 D3D11 硬件解码。
// 事件经 mpv_set_wakeup_callback + 事件线程，再投递到 UI 线程。
// ============================================================

#include <windows.h>
#include <atomic>
#include <string>
#include <thread>

struct mpv_handle;

class Player {
public:
    // 事件类型（简化抽象，供 UI 使用）
    enum class Event {
        None = 0,
        EndFile,          // 断流/切流/结束
        VolumeChanged,
        MuteChanged,
        PlaybackStarted,
        PlaybackStopped,
        Error,
    };

    using EventCallback = void (*)(Player* player, Event ev, void* ctx);

    Player();
    ~Player();

    // 初始化 mpv 核心并嵌入到指定子窗口（需在首次 LoadUrl 前调用）
    // volume 初始音量（0-100）
    bool Init(HWND videoHwnd, int volume, bool mute);
    void Shutdown();

    // 播放直播流（会替换当前流）
    bool LoadUrl(const std::string& url);

    // 音量 / 静音（属性持久化到配置）
    void SetVolume(int volume);
    int  GetVolume() const;
    void SetMute(bool mute);
    bool GetMute() const;

    bool IsValid() const { return mpv_ != nullptr; }

    void SetEventCallback(EventCallback cb, void* ctx) {
        evCb_ = cb; evCtx_ = ctx;
    }

private:
    static void WakeupCallback(void* ctx);
    void EventThreadMain();

    mpv_handle* mpv_ = nullptr;
    std::thread eventThread_;
    std::atomic<bool> stopThread_{ false };
    HWND videoHwnd_ = nullptr;

    EventCallback evCb_ = nullptr;
    void* evCtx_ = nullptr;
};
