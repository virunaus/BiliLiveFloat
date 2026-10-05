#pragma once
// ============================================================
// search_window.h —— 主播搜索窗口
// 按名称关键词搜索主播直播间（live_user 接口）：
//   - 开播主播：金色呼吸发光边框（动画）
//   - 未开播主播：灰色边框 + 灰色文字
//   - 结果按粉丝量由低到高排序
//   - 双击结果 → 回调主窗口进入该直播间
// ============================================================

#include <windows.h>
#include <string>
#include <vector>

#include "bili_api.h"

class SearchWindow {
public:
    // 进入直播间回调（主窗口切房）
    using EnterRoomCb = void (*)(long long roomid, void* ctx);

    ~SearchWindow();

    // 打开/聚焦搜索窗口；keyword 非空时直接搜索
    void Open(HINSTANCE hInst, HWND owner, const std::wstring& keyword = L"");

    void Close();

    HWND Hwnd() const { return hwnd_; }

    void SetEnterRoomCallback(EnterRoomCb cb, void* ctx) {
        enterCb_ = cb; ctx_ = ctx;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void OnSearch();
    void PopulateList();
    void EnterSelected();

    HWND hwnd_ = nullptr;
    HWND hEdit_ = nullptr;
    HWND hBtn_ = nullptr;      // "搜索"按钮
    HWND hEnter_ = nullptr;    // "进入直播间"按钮
    HWND hList_ = nullptr;
    HINSTANCE hInst_ = nullptr;
    std::vector<bili::SearchResult> results_;
    EnterRoomCb enterCb_ = nullptr;
    void* ctx_ = nullptr;
};
