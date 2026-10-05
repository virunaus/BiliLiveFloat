// ============================================================
// config.cpp —— 配置读写实现（nlohmann-json）
// ============================================================

#include "config.h"

#include <windows.h>
#include <shlobj.h>
#include <nlohmann/json.hpp>

#include <fstream>

using nlohmann::json;

std::wstring Config::GetPath() {
    wchar_t buf[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf))) {
        std::wstring dir = std::wstring(buf) + L"\\BiliLiveFloat";
        // 确保目录存在
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\config.json";
    }
    return L"config.json";
}

AppConfig Config::Load() {
    AppConfig cfg;
    std::ifstream in(GetPath(), std::ios::binary);
    if (!in) return cfg;
    try {
        json j = json::parse(in);
        cfg.x = j.value("window_x", -1);
        cfg.y = j.value("window_y", -1);
        cfg.w = j.value("window_w", 480);
        cfg.h = j.value("window_h", 270);
        cfg.volume = j.value("volume", 100);
        cfg.mute = j.value("mute", false);
        cfg.topmost = j.value("topmost", true);
        cfg.passthrough = j.value("passthrough", false);
        cfg.danmaku_enabled = j.value("danmaku_enabled", true);
        cfg.last_room = j.value("last_room", 0LL);
        if (j.contains("recent_rooms") && j["recent_rooms"].is_array()) {
            for (auto& r : j["recent_rooms"]) {
                if (r.is_number_integer()) cfg.recent_rooms.push_back(r.get<long long>());
            }
        }
    } catch (...) {
        // 配置损坏时回退默认值
        cfg = AppConfig{};
    }
    return cfg;
}

void Config::Save(const AppConfig& cfg) {
    json j;
    j["window_x"] = cfg.x;
    j["window_y"] = cfg.y;
    j["window_w"] = cfg.w;
    j["window_h"] = cfg.h;
    j["volume"] = cfg.volume;
    j["mute"] = cfg.mute;
    j["topmost"] = cfg.topmost;
    j["passthrough"] = cfg.passthrough;
    j["danmaku_enabled"] = cfg.danmaku_enabled;
    j["last_room"] = cfg.last_room;
    j["recent_rooms"] = json::array();
    for (auto r : cfg.recent_rooms) j["recent_rooms"].push_back(r);

    std::ofstream out(GetPath(), std::ios::binary);
    if (out) out << j.dump(2);
}
