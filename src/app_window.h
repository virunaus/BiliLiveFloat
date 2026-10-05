#pragma once
// ============================================================
// app_window.h —— 主窗口管理
// 无边框置顶窗口；控制/穿透双模式；全局热键；
// 视频子窗口（libmpv 嵌入）+ 弹幕透明层子窗口（D2D）；
// 右键菜单、托盘、切房流程、配置持久化。
// ============================================================

#include <windows.h>

#include "config.h"
#include "player.h"
#include "danmaku_client.h"
#include "danmaku_renderer.h"
#include "bili_api.h"
#include "search_window.h"

#include <string>

// 自定义消息
constexpr UINT WM_APP_DANMAKU_STATUS = WM_APP + 1;  // wParam = (DanmakuStatus)
constexpr UINT WM_APP_PLAYER_EVENT   = WM_APP + 2;  // wParam = (Player::Event)

class AppWindow {
public:
    AppWindow();
    ~AppWindow();

    // 创建主窗口（应用保存的配置用于恢复位置/尺寸/音量等）
    bool Create(HINSTANCE hInst, const AppConfig& cfg);
    HWND Handle() const { return hwnd_; }

    // 切换直播间（输入：数字/短链/完整 URL 或房号）
    void SwitchToRoom(const std::wstring& input);
    void SwitchToRoom(long long roomOrShort);

    // 双模式 / 常用操作
    void TogglePassthrough();
    void ToggleMute();
    void ToggleTopmost();
    void AdjustVolume(int delta);
    // 画质切换（qn + 显示名）
    void SetQuality(int qn, const wchar_t* name);
    // 音量条 HUD（0-100 可视化）
    void ShowVolumeBar(int vol);

    void SaveConfig();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT OnNcHitTest(POINT ptScreen);

    void OnCreate();
    void OnSize();
    void ShowContextMenu(POINT ptScreen);
    void ShowToast(const std::wstring& text);
    void ApplyPassthrough(bool on);
    void ApplyTopmost(bool on);
    void UpdateChildren();
    void RestoreFromTray();
    void StartSwitchRoomFlow(long long roomOrShort);

    // 回调（网络线程 -> UI 线程）
    static void OnDanmakuMessage(const DanmakuMessage& msg, void* ctx);
    static void OnDanmakuStatus(DanmakuStatus st, void* ctx);
    static void OnPlayerEvent(Player* p, Player::Event ev, void* ctx);
    // UI 线程处理
    void HandleDanmakuStatus(DanmakuStatus st);
    void HandlePlayerEvent(Player::Event ev);

    HINSTANCE hInst_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND hVideo_ = nullptr;      // mpv 嵌入子窗口
    HWND hDanmaku_ = nullptr;    // 弹幕透明层子窗口
    HWND hToast_ = nullptr;      // toast 提示子窗口

    Player player_;
    DanmakuClient danmaku_;
    DanmakuRenderer renderer_;
    bili::Session session_;

    AppConfig cfg_;
    bool passthrough_ = false;
    bool quitting_ = false;
    long long currentRoom_ = 0;
    std::wstring currentTitle_;
    int currentQn_ = 10000;                    // 当前画质
    std::vector<bili::QualityDesc> acceptQn_;  // 房间可用画质（由高到低）
    HWND hVolBar_ = nullptr;                   // 音量条 HUD 窗口

    // 搜索窗口回调（搜索窗口线程 -> UI）
    static void OnSearchEnterRoom(long long roomid, void* ctx);
    void HandleSearchEnterRoom(long long roomid);

    SearchWindow searchWnd_;

    // 静态用于转发实例
    static AppWindow* s_instance;
};
