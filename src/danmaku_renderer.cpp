// ============================================================
// danmaku_renderer.cpp —— 弹幕渲染器实现
// 使用 ID2D1DCRenderTarget 绘制到 32bpp 内存位图，
// 再通过 UpdateLayeredWindow 合成到分层弹幕窗口。
// ============================================================

#include "danmaku_renderer.h"

#include <d2d1helper.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

const D2D1_COLOR_F DanmakuRenderer::kPalette[] = {
    { 1.0f, 1.0f, 1.0f, 1.0f },   // 白
    { 1.0f, 0.85f, 0.20f, 1.0f }, // 金黄
    { 0.55f, 0.90f, 1.0f, 1.0f }, // 天蓝
    { 0.75f, 1.0f, 0.65f, 1.0f }, // 浅绿
    { 1.0f, 0.60f, 0.70f, 1.0f }, // 粉
    { 1.0f, 0.65f, 0.40f, 1.0f }, // 橙
    { 0.80f, 0.70f, 1.0f, 1.0f }, // 淡紫
    { 0.95f, 0.95f, 0.85f, 1.0f },// 米白
};

DanmakuRenderer::DanmakuRenderer() = default;
DanmakuRenderer::~DanmakuRenderer() {
    for (auto* b : paletteBrushes_) if (b) b->Release();
    if (textFormat_) textFormat_->Release();
    if (strokeBrush_) strokeBrush_->Release();
    if (shadowBrush_) shadowBrush_->Release();
    if (dcTarget_) dcTarget_->Release();
    if (dwriteFactory_) dwriteFactory_->Release();
    if (d2dFactory_) d2dFactory_->Release();
}

bool DanmakuRenderer::Init(HWND hwnd) {
    hwnd_ = hwnd;

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_);
    if (FAILED(hr)) return false;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(&dwriteFactory_));
    if (FAILED(hr)) return false;

    // DCRenderTarget 属性（32bpp premultiplied alpha）
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    hr = d2dFactory_->CreateDCRenderTarget(&props, &dcTarget_);
    if (FAILED(hr)) return false;

    dcTarget_->CreateSolidColorBrush(D2D1::ColorF(0.05f, 0.05f, 0.05f, 1.0f), &strokeBrush_);
    dcTarget_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f), &shadowBrush_);
    // 调色板画刷（每条弹幕按索引取色）
    for (int i = 0; i < 8; ++i) {
        dcTarget_->CreateSolidColorBrush(kPalette[i], &paletteBrushes_[i]);
    }
    if (!strokeBrush_ || !shadowBrush_) return false;

    RecreateTextFormat();

    // 初始尺寸
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    width_ = static_cast<UINT>(rc.right - rc.left);
    height_ = static_cast<UINT>(rc.bottom - rc.top);
    SetSize(width_, height_);
    ready_ = true;
    return true;
}

void DanmakuRenderer::RecreateTextFormat() {
    if (textFormat_) {
        textFormat_->Release();
        textFormat_ = nullptr;
    }
    // 微软雅黑，字号随窗口缩放
    if (!dwriteFactory_) return;
    HRESULT hr = dwriteFactory_->CreateTextFormat(
        L"Microsoft YaHei", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        fontSize_, L"zh-CN", &textFormat_);
    if (FAILED(hr)) return;
    textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
}

void DanmakuRenderer::SetSize(UINT width, UINT height) {
    width_ = width ? width : 1;
    height_ = height ? height : 1;
    // 字号随窗口缩放：窗口高度 270 时约 16px
    fontSize_ = 10.0f + 6.0f * (static_cast<float>(height_) / 270.0f);
    RecreateTextFormat();
    laneCount_ = std::max(1, static_cast<int>(static_cast<float>(height_) /
                                              (fontSize_ * 1.6f)));
    laneTail_.assign(static_cast<size_t>(laneCount_), -1.0f);
}

void DanmakuRenderer::Clear() {
    std::lock_guard<std::mutex> lk(queueMutex_);
    pending_.clear();
    active_.clear();
    if (laneTail_.size() == static_cast<size_t>(laneCount_)) {
        std::fill(laneTail_.begin(), laneTail_.end(), -1.0f);
    }
}

void DanmakuRenderer::PushMessage(const DanmakuMessage& msg) {
    if (!ready_) return;
    std::lock_guard<std::mutex> lk(queueMutex_);
    // 弹幕密度限流：队列最多保留 500 条，超出丢弃最旧
    if (pending_.size() >= 500) pending_.pop_front();
    pending_.push_back(msg);
}

float DanmakuRenderer::MeasureText(const std::wstring& text, float& height) {
    if (!dwriteFactory_ || !textFormat_) return 0;
    IDWriteTextLayout* layout = nullptr;
    dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                     textFormat_, 4096.0f, fontSize_ * 1.4f, &layout);
    if (!layout) return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    height = m.height;
    float w = m.width;
    layout->Release();
    return w;
}

bool DanmakuRenderer::TrySpawn(const DanmakuMessage& msg) {
    if (active_.size() > 600) return false;  // 总弹幕上限，防内存膨胀
    float textHeight = 0;
    float w = MeasureText(msg.text, textHeight);
    if (w <= 0 || w > static_cast<float>(width_) * 0.95f) {
        // 超长弹幕：限制宽度（丢弃或截断），此处直接丢弃过长弹幕
        return false;
    }
    float speed = 90.0f + static_cast<float>(rand() % 50) *
                         (static_cast<float>(width_) / 480.0f);
    // 选择泳道：优先选择"上一条弹幕已完全进入窗口"的空闲泳道
    for (int lane = 0; lane < laneCount_; ++lane) {
        float tail = laneTail_[static_cast<size_t>(lane)];
        float required = 8.0f; // 与上一条的间距
        if (tail < 0 || tail <= static_cast<float>(width_) - required) {
            Item it;
            it.text = msg.text;
            it.colorIdx = rand() % 8;
            it.x = static_cast<float>(width_);
            it.width = w;
            it.speed = speed;
            it.lane = lane;
            it.birth = GetTickCount64();
            // 若上一条还未完全进入，则紧跟其后
            if (tail > 0) it.x = std::max(it.x, tail + required);
            active_.push_back(std::move(it));
            laneTail_[static_cast<size_t>(lane)] = it.x + w;
            return true;
        }
    }
    return false;  // 所有泳道都忙，丢弃这条（限流）
}

void DanmakuRenderer::Advance(float dtMs) {
    float dt = dtMs / 1000.0f;
    for (auto& it : active_) {
        it.x -= it.speed * dt;
    }
    // 移除完全滚出屏幕的弹幕，并更新泳道尾随位置
    laneTail_.assign(static_cast<size_t>(laneCount_), -1.0f);
    std::vector<Item> kept;
    kept.reserve(active_.size());
    for (auto& it : active_) {
        if (it.x + it.width > -4.0f) {
            kept.push_back(std::move(it));
        }
    }
    active_ = std::move(kept);
    for (auto& it : active_) {
        float tail = it.x + it.width;
        laneTail_[static_cast<size_t>(it.lane)] = std::max(laneTail_[static_cast<size_t>(it.lane)], tail);
    }
}

void DanmakuRenderer::Draw() {
    if (!dcTarget_ || width_ == 0 || height_ == 0) return;

    // 创建 32bpp 内存位图（premultiplied alpha）
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(width_);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(height_);  // 自上而下
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(hdcMem, hbm);
    ReleaseDC(nullptr, hdcScreen);
    if (!hbm) return;

    RECT rc{ 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
    if (SUCCEEDED(dcTarget_->BindDC(hdcMem, &rc))) {
        dcTarget_->BeginDraw();
        dcTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
        // 清为全透明
        dcTarget_->Clear(D2D1::ColorF(0, 0, 0, 0));

        for (auto& it : active_) {
            float y = it.lane * fontSize_ * 1.6f + 2.0f;
            // 阴影（右下偏移 1.5px）
            dcTarget_->DrawText(it.text.c_str(), static_cast<UINT32>(it.text.size()),
                                textFormat_, D2D1::RectF(it.x + 1.5f, y + 1.5f,
                                                         it.x + 1.5f + width_, y + 1.5f + fontSize_ * 1.4f),
                                shadowBrush_,
                                D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            // 四向描边（黑边）
            for (int dy = -1; dy <= 1; dy += 2) {
                dcTarget_->DrawText(it.text.c_str(), static_cast<UINT32>(it.text.size()),
                                    textFormat_, D2D1::RectF(it.x, y + dy,
                                                             it.x + width_, y + dy + fontSize_ * 1.4f),
                                    strokeBrush_,
                                    D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            }
            for (int dx = -1; dx <= 1; dx += 2) {
                dcTarget_->DrawText(it.text.c_str(), static_cast<UINT32>(it.text.size()),
                                    textFormat_, D2D1::RectF(it.x + dx, y,
                                                             it.x + dx + width_, y + fontSize_ * 1.4f),
                                    strokeBrush_,
                                    D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            }
            // 本体（随机颜色）
            if (paletteBrushes_[it.colorIdx]) {
                dcTarget_->DrawText(it.text.c_str(), static_cast<UINT32>(it.text.size()),
                                    textFormat_, D2D1::RectF(it.x, y,
                                                             it.x + width_, y + fontSize_ * 1.4f),
                                    paletteBrushes_[it.colorIdx],
                                    D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            }
        }
        dcTarget_->EndDraw();

        // 合成到分层窗口
        POINT ptSrc = { 0, 0 };
        SIZE sz = { static_cast<LONG>(width_), static_cast<LONG>(height_) };
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        // 重要：ptDst 必须传 NULL（不改变窗口屏幕位置）。
        // UpdateLayeredWindow 的 ptDst 是"目标屏幕坐标"，若传 (0,0)，
        // 每次渲染都会把弹幕窗口钉回屏幕左上角，导致拖动停止后弹幕与窗口画面分离。
        UpdateLayeredWindow(hwnd_, hdcMem, nullptr, &sz, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
    }

    SelectObject(hdcMem, old);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
}

void DanmakuRenderer::Tick() {
    if (!ready_) return;
    // 消费队列（每帧最多 6 条，弹幕密度限流）
    std::deque<DanmakuMessage> batch;
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        size_t n = std::min<size_t>(pending_.size(), 15);
        for (size_t i = 0; i < n; ++i) {
            batch.push_back(pending_.front());
            pending_.pop_front();
        }
    }
    for (auto& m : batch) {
        TrySpawn(m);
    }
    if (paused_.load()) return;
    Advance(33.0f);  // 约 30fps 推进
    Draw();
}
