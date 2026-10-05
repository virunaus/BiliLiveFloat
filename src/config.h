#pragma once
// ============================================================
// config.h —— 配置持久化（%APPDATA%\BiliLiveFloat\config.json）
// 保存：窗口位置/尺寸、音量、静音、置顶、穿透状态、
//       最近直播间列表、上次房间。
// ============================================================

#include <string>
#include <vector>

// 应用配置
struct AppConfig {
    int x = -1, y = -1;              // 窗口位置（-1 = 首次启动，居中）
    int w = 480, h = 270;            // 窗口尺寸
    int volume = 100;                // 音量 0-100
    bool mute = false;               // 静音
    bool topmost = true;             // 始终置顶
    bool passthrough = false;        // 上次退出时是否穿透模式
    bool danmaku_enabled = true;     // 弹幕开关
    std::vector<long long> recent_rooms;   // 最近直播间（最多 10，最新在前）
    long long last_room = 0;         // 上次房间
};

class Config {
public:
    // 配置文件路径 %APPDATA%\BiliLiveFloat\config.json
    static std::wstring GetPath();
    // 读取配置（文件不存在时返回默认值）
    AppConfig Load();
    // 保存配置
    void Save(const AppConfig& cfg);
};
