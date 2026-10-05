// ============================================================
// player.cpp —— libmpv 播放器实现
// 关键选项：wid 嵌入、d3d11 硬件解码、反爬请求头、断流事件。
// ============================================================

#include "player.h"

#include <mpv/client.h>

#include <cstdio>

namespace {

// 把 UTF-8 字符串转成 wide（用于 SetWindowText 等）
// mpv 的事件字符串大多是 ASCII/UTF-8，这里仅做必要转换
const char* kUA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";

} // namespace

Player::Player() = default;
Player::~Player() { Shutdown(); }

bool Player::Init(HWND videoHwnd, int volume, bool mute) {
    videoHwnd_ = videoHwnd;

    mpv_ = mpv_create();
    if (!mpv_) return false;

    // ---- 选项设置（嵌入窗口必须在 initialize 前）----
    // 视频输出：gpu 后端 + D3D11，硬件解码自动
    mpv_set_option_string(mpv_, "vo", "gpu");
    mpv_set_option_string(mpv_, "gpu-api", "d3d11");
    mpv_set_option_string(mpv_, "hwdec", "auto");
    // 直播流缓冲
    mpv_set_option_string(mpv_, "cache", "yes");
    mpv_set_option_string(mpv_, "demuxer-max-bytes", "256MB");
    mpv_set_option_string(mpv_, "demuxer-max-back-bytes", "64MB");
    // 嵌入窗口
    char widStr[32];
    snprintf(widStr, sizeof(widStr), "%lld", static_cast<long long>(
        reinterpret_cast<INT_PTR>(videoHwnd_)));
    mpv_set_option_string(mpv_, "wid", widStr);
    // 禁止 mpv 创建自己的窗口/控制台
    mpv_set_option_string(mpv_, "force-window", "no");
    // 反爬请求头（否则 403）：Referer + UA
    mpv_set_option_string(mpv_, "http-header-fields",
        "Referer: https://live.bilibili.com, "
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    // B站 CDN 的 HTTP/2 与 curl 兼容性差（用户机器实测报
    // "Stream error in the HTTP/2 framing layer"），强制 HTTP/1.1
    mpv_set_option_string(mpv_, "http-version", "1.1");
    // 直播流没有暂停意义，但保留默认行为即可
    mpv_set_option_string(mpv_, "keep-open", "no");
    // 音频输出自动
    mpv_set_option_string(mpv_, "ao", "wasapi");
    // 诊断日志：写到 D:\bilibilibilil\build\mpv.log（排查播放/解码问题）
    mpv_set_option_string(mpv_, "log-file", "D:\\bilibilibilil\\build\\mpv.log");
    mpv_request_log_messages(mpv_, "debug");

    // 初始音量/静音
    char volStr[16];
    snprintf(volStr, sizeof(volStr), "%d", volume);
    mpv_set_option_string(mpv_, "volume", volStr);
    mpv_set_option_string(mpv_, "mute", mute ? "yes" : "no");

    if (mpv_initialize(mpv_) < 0) {
        mpv_destroy(mpv_);
        mpv_ = nullptr;
        return false;
    }

    // 事件唤醒 + 事件线程
    mpv_set_wakeup_callback(mpv_, &Player::WakeupCallback, this);
    stopThread_ = false;
    eventThread_ = std::thread(&Player::EventThreadMain, this);
    return true;
}

void Player::Shutdown() {
    if (!mpv_) return;
    stopThread_ = true;
    // 唤醒事件线程退出
    if (eventThread_.joinable()) {
        mpv_wakeup(mpv_);
        eventThread_.join();
    }
    mpv_terminate_destroy(mpv_);
    mpv_ = nullptr;
}

void Player::WakeupCallback(void* ctx) {
    // 由事件线程处理即可，无需额外动作
    (void)ctx;
}

void Player::EventThreadMain() {
    while (!stopThread_.load()) {
        mpv_event* event = mpv_wait_event(mpv_, 0.4);
        if (event->event_id == MPV_EVENT_NONE) {
            if (stopThread_.load()) break;
            continue;
        }
        Event ev = Event::None;
        switch (event->event_id) {
            case MPV_EVENT_END_FILE: {
                auto* end = static_cast<mpv_event_end_file*>(event->data);
                if (end && end->reason == MPV_END_FILE_REASON_EOF) {
                    ev = Event::EndFile;      // 正常结束/断流
                } else if (end && end->reason == MPV_END_FILE_REASON_ERROR) {
                    ev = Event::Error;
                } else {
                    ev = Event::EndFile;
                }
                break;
            }
            case MPV_EVENT_PROPERTY_CHANGE: {
                auto* prop = static_cast<mpv_event_property*>(event->data);
                if (!prop || !prop->name) break;
                if (strcmp(prop->name, "volume") == 0) ev = Event::VolumeChanged;
                else if (strcmp(prop->name, "mute") == 0) ev = Event::MuteChanged;
                break;
            }
            case MPV_EVENT_START_FILE: {
                ev = Event::PlaybackStarted;
                break;
            }
            default:
                break;
        }
        if (ev != Event::None && evCb_) {
            evCb_(this, ev, evCtx_);
        }
    }
}

bool Player::LoadUrl(const std::string& url) {
    if (!mpv_) return false;
    const char* args[] = { "loadfile", url.c_str(), "replace", nullptr };
    return mpv_command(mpv_, args) >= 0;
}

void Player::SetVolume(int volume) {
    if (!mpv_) return;
    char val[16];
    snprintf(val, sizeof(val), "%d", volume);
    const char* args[] = { "set", "volume", val, nullptr };
    mpv_command(mpv_, args);
}

int Player::GetVolume() const {
    if (!mpv_) return 100;
    double vol = 100.0;
    mpv_get_property(mpv_, "volume", MPV_FORMAT_DOUBLE, &vol);
    return static_cast<int>(vol + 0.5);
}

void Player::SetMute(bool mute) {
    if (!mpv_) return;
    mpv_set_property_string(mpv_, "mute", mute ? "yes" : "no");
}

bool Player::GetMute() const {
    if (!mpv_) return false;
    int m = 0;
    mpv_get_property(mpv_, "mute", MPV_FORMAT_FLAG, &m);
    return m != 0;
}
