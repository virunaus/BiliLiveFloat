#pragma once
// ============================================================
// danmaku_renderer.h —— 弹幕渲染器（Direct2D + DirectWrite）
// 渲染到弹幕层窗口（WS_EX_LAYERED 透明窗口，盖在视频窗口之上），
// 右→左匀速滚动、多泳道防碰撞、随机颜色、黑描边+阴影。
// 线程安全：PushMessage 可跨线程调用；Tick 只在 UI 线程调用。
// ============================================================

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "danmaku_client.h"   // DanmakuMessage

class DanmakuRenderer {
public:
    DanmakuRenderer();
    ~DanmakuRenderer();

    // 绑定弹幕层窗口并初始化 D2D/DWrite 资源（UI 线程）
    bool Init(HWND hwnd);
    // 窗口尺寸变化时调用（UI 线程）
    void SetSize(UINT width, UINT height);
    // 清空所有弹幕（切房时调用）
    void Clear();
    // 入队一条弹幕（可跨线程）
    void PushMessage(const DanmakuMessage& msg);
    // 每帧推进动画并绘制（UI 线程，建议 30-60fps 调用）
    void Tick();
    // 恢复/暂停动画推进（最小化等场景）
    void SetPaused(bool paused) { paused_ = paused; }

private:
    struct Item {
        std::wstring text;
        int colorIdx = 0;      // 调色板索引
        float x = 0;          // 当前左端 x
        float width = 0;      // 文本绘制宽度
        float speed = 0;      // 像素/秒
        int lane = 0;
        ULONGLONG birth = 0;
    };

    void RecreateTextFormat();                // 按当前字号重建文本格式
    void Advance(float dtMs);                 // 更新 x 位置
    void Draw();
    bool TrySpawn(const DanmakuMessage& msg); // 尝试入泳道（防碰撞）
    float MeasureText(const std::wstring& text, float& height);

    HWND hwnd_ = nullptr;
    bool ready_ = false;
    UINT width_ = 480, height_ = 270;

    // D2D/DWrite 资源
    ID2D1Factory* d2dFactory_ = nullptr;
    ID2D1DCRenderTarget* dcTarget_ = nullptr;
    IDWriteFactory* dwriteFactory_ = nullptr;
    IDWriteTextFormat* textFormat_ = nullptr;
    ID2D1SolidColorBrush* strokeBrush_ = nullptr;
    ID2D1SolidColorBrush* shadowBrush_ = nullptr;
    ID2D1SolidColorBrush* paletteBrushes_[8] = {};  // 对应 kPalette

    // 弹幕数据
    std::mutex queueMutex_;
    std::deque<DanmakuMessage> pending_;
    std::vector<Item> active_;
    std::vector<float> laneTail_;   // 每个泳道最后一条弹幕的尾部 x
    int laneCount_ = 0;
    float fontSize_ = 16.0f;

    // 随机调色板（常见弹幕色）
    static const D2D1_COLOR_F kPalette[];

    std::atomic<bool> paused_{ false };
};
