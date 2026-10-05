// ============================================================
// search_window.cpp —— 主播搜索窗口实现
// 深色无边框弹窗：搜索框 + 自绘结果列表
//   - 开播：金色呼吸发光边框（定时器动画）+ 状态标签"● 直播中"
//   - 未开播：灰色边框/文字 + 标签"○ 未开播"
//   - 双击条目 → 回调主窗口切房
// ============================================================

#include "search_window.h"

#include <windowsx.h>
#include <cstdio>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

constexpr int kItemH = 58;         // 列表项高度
constexpr int kGap = 12;
constexpr int kTopH = 44;          // 搜索区高度
constexpr int kBottomH = 52;       // 底部按钮区高度

// 结果项格式："昵称\t粉丝\t状态"（用制表符分段，自绘时解析）
std::string FansStr(long long fans) {
    char buf[32];
    if (fans >= 100000000)
        snprintf(buf, sizeof(buf), "%.1f亿", fans / 1e8);
    else if (fans >= 10000)
        snprintf(buf, sizeof(buf), "%.1f万", fans / 1e4);
    else
        snprintf(buf, sizeof(buf), "%lld", fans);
    return buf;
}

void DrawTextClipped(HDC dc, const RECT& rc, const std::wstring& text,
                     HFONT font, COLORREF color) {
    HFONT old = static_cast<HFONT>(SelectObject(dc, font));
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    RECT r = rc;
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
}

} // namespace

SearchWindow::~SearchWindow() {
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

void SearchWindow::Open(HINSTANCE hInst, HWND owner, const std::wstring& keyword) {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        if (!keyword.empty()) {
            SetWindowTextW(hEdit_, keyword.c_str());
            OnSearch();
        }
        return;
    }
    hInst_ = hInst;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &SearchWindow::WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"BiliLiveFloatSearch";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassExW(&wc);

    // 深色无边框顶层窗口
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    hwnd_ = CreateWindowExW(ex, L"BiliLiveFloatSearch", L"搜索直播间",
                            WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, 460, 620,
                            owner, nullptr, hInst, this);
    if (!hwnd_) return;
    RECT wr{};
    GetWindowRect(hwnd_, &wr);
    MoveWindow(hwnd_, wr.left, wr.top, 460, 620, TRUE);

    // 搜索框
    hEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                             kGap, kGap, 460 - kGap * 2 - 80 - 8, kTopH - kGap * 2,
                             hwnd_, nullptr, hInst, nullptr);
    // 搜索按钮
    hBtn_ = CreateWindowW(L"BUTTON", L"搜索",
                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                          460 - kGap - 80, kGap, 80, kTopH - kGap * 2,
                          hwnd_, reinterpret_cast<HMENU>(1), hInst, nullptr);
    // 结果列表（自绘）
    hList_ = CreateWindowExW(0, L"LISTBOX", L"",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER |
                             LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                             kGap, kTopH, 460 - kGap * 2, 620 - kTopH - kBottomH - 4,
                             hwnd_, nullptr, hInst, nullptr);
    // 底部"进入直播间"按钮
    hEnter_ = CreateWindowW(L"BUTTON", L"进入直播间",
                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                            kGap, 620 - kBottomH + 10, 460 - kGap * 2, 32,
                            hwnd_, reinterpret_cast<HMENU>(3), hInst, nullptr);
    SendMessageW(hList_, LB_SETITEMHEIGHT, 0, kItemH);

    SetWindowLongPtrW(hEdit_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(hList_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(hEnter_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetFocus(hEdit_);

    if (!keyword.empty()) {
        SetWindowTextW(hEdit_, keyword.c_str());
        OnSearch();
    }
    // 发光呼吸动画（30fps 重绘列表）
    SetTimer(hwnd_, 2, 33, nullptr);
}

void SearchWindow::Close() {
    if (hwnd_) { ShowWindow(hwnd_, SW_HIDE); }
}

void SearchWindow::OnSearch() {
    wchar_t kw[128];
    GetWindowTextW(hEdit_, kw, 128);
    std::wstring keyword(kw);
    // 去掉首尾空白
    while (!keyword.empty() && (keyword.front() == L' ' || keyword.front() == L'\t'))
        keyword.erase(keyword.begin());
    while (!keyword.empty() && (keyword.back() == L' ' || keyword.back() == L'\t'))
        keyword.pop_back();
    if (keyword.empty()) return;

    results_ = bili::SearchLiveUsers(keyword);
    PopulateList();
}

void SearchWindow::PopulateList() {
    SendMessageW(hList_, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < results_.size(); ++i) {
        // 文本行：昵称\t粉丝\t状态
        char buf[512];
        const auto& r = results_[i];
        std::string fans = FansStr(r.fans);
        std::string status = (r.live_status == 1) ? "直播中" :
                             (r.live_status == 2) ? "轮播" : "未开播";
        snprintf(buf, sizeof(buf), "%s\t%s\t%s",
                 r.name.c_str(), fans.c_str(), status.c_str());
        // UTF-8 → wide（LB_ADDSTRING）
        int n = MultiByteToWideChar(CP_UTF8, 0, buf, -1, nullptr, 0);
        std::wstring w(n ? n - 1 : 0, L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, buf, -1, &w[0], n);
        int idx = static_cast<int>(SendMessageW(hList_, LB_ADDSTRING, 0,
                                                reinterpret_cast<LPARAM>(w.c_str())));
        if (idx != LB_ERR) SendMessageW(hList_, LB_SETITEMDATA, idx, i);
    }
    // 状态提示（标题栏文字）
    wchar_t title[128];
    swprintf_s(title, L"搜索直播间 - 共 %d 位主播（按粉丝量由高到低）", (int)results_.size());
    SetWindowTextW(hwnd_, title);
}

void SearchWindow::EnterSelected() {
    int sel = static_cast<int>(SendMessageW(hList_, LB_GETCURSEL, 0, 0));
    if (sel == LB_ERR) return;
    size_t idx = static_cast<size_t>(SendMessageW(hList_, LB_GETITEMDATA, sel, 0));
    if (idx >= results_.size()) return;
    long long room = results_[idx].roomid;
    Close();
    if (enterCb_) enterCb_(room, ctx_);
}

// ---- 自绘列表项 ------------------------------------------------

static void DrawItemContent(HDC dc, const RECT& rc, const std::wstring& line,
                            bool live, int phase) {
    // 拆三段：昵称 \t 粉丝 \t 状态
    std::wstring name, fans, status;
    size_t p1 = line.find(L'\t');
    if (p1 != std::wstring::npos) {
        name = line.substr(0, p1);
        size_t p2 = line.find(L'\t', p1 + 1);
        if (p2 != std::wstring::npos) {
            fans = line.substr(p1 + 1, p2 - p1 - 1);
            status = line.substr(p2 + 1);
        }
    } else {
        name = line;
    }

    // 字体：名称 13pt，粉丝/状态 10pt
    HFONT nameFont = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
    HFONT subFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");

    COLORREF nameColor = live ? RGB(240, 240, 240) : RGB(130, 130, 130);
    COLORREF subColor = live ? RGB(255, 200, 80) : RGB(110, 110, 110);

    RECT rName = rc; rName.left += 18; rName.right = rc.right - 140;
    RECT rFans = rc; rFans.left = rc.right - 135; rFans.right = rc.right - 14;
    RECT rSt = rc; rSt.left = rc.right - 170; rSt.right = rc.right - 135;

    DrawTextClipped(dc, rName, name, nameFont, nameColor);
    DrawTextClipped(dc, rFans, fans, subFont, subColor);

    // 状态标签：开播金色圆点+文字；未开播灰色
    if (live) {
        SetBkMode(dc, TRANSPARENT);
        // 发光圆点（呼吸）
        int glow = 140 + 90 * static_cast<int>(sin(phase * 3.14159f / 30.0f));
        if (glow < 120) glow = 120;
        HBRUSH dot = CreateSolidBrush(RGB(255, 180, 40));
        HBRUSH halo = CreateSolidBrush(RGB(255, 180, 40));
        HBRUSH oldBr = static_cast<HBRUSH>(SelectObject(dc, halo));
        Ellipse(dc, rSt.left - 2, rSt.top + 4, rSt.left + 12, rSt.top + 18);
        SelectObject(dc, dot);
        Ellipse(dc, rSt.left, rSt.top + 6, rSt.left + 10, rSt.top + 16);
        SelectObject(dc, oldBr);
        DeleteObject(dot); DeleteObject(halo);
        // 文字（呼吸透明度无法直接设置，用亮度变化）
        int lum = 170 + 60 * static_cast<int>(sin((phase + 15) * 3.14159f / 30.0f));
        if (lum < 170) lum = 170;
        DrawTextClipped(dc, rSt, status, subFont, RGB(lum, lum - 60 > 0 ? lum - 60 : 40, 40));
    } else {
        DrawTextClipped(dc, rSt, status, subFont, RGB(100, 100, 100));
    }

    DeleteObject(nameFont); DeleteObject(subFont);
}

LRESULT CALLBACK SearchWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SearchWindow* self = reinterpret_cast<SearchWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self && msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<SearchWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }

    switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == 1 && HIWORD(wParam) == BN_CLICKED) {
                self->OnSearch();
                return 0;
            }
            if (id == 3 && HIWORD(wParam) == BN_CLICKED) {
                // "进入直播间"按钮：进入选中项
                self->EnterSelected();
                return 0;
            }
            if (id == 2) {
                // ListBox 通知
                if (HIWORD(wParam) == LBN_DBLCLK) {
                    self->EnterSelected();
                }
                return 0;
            }
            break;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (dis->CtlType != ODT_LISTBOX || !self) break;
            // 背景
            bool selected = (dis->itemState & ODS_SELECTED) != 0;
            bool live = true; // 从 itemdata 查状态
            size_t idx = static_cast<size_t>(SendMessageW(dis->hwndItem, LB_GETITEMDATA,
                                                          dis->itemID, 0));
            if (idx < self->results_.size())
                live = (self->results_[idx].live_status != 0);
            (void)selected;

            HDC dc = dis->hDC;
            RECT rc = dis->rcItem;
            // 呼吸相位（用窗口定时器计数）
            static int phase = 0;
            phase++;

            if (live) {
                // 开播：金色发光边框（三层，呼吸动画）
                int a = 30 + 25 * static_cast<int>(sin(phase * 3.14159f / 30.0f));
                if (a < 30) a = 30;
                HBRUSH bg = CreateSolidBrush(RGB(28, 24, 16));
                FillRect(dc, &rc, bg);
                DeleteObject(bg);
                HPEN penOuter = CreatePen(PS_SOLID, 2, RGB(80 + a, 60, 10));
                HPEN penMid = CreatePen(PS_SOLID, 1, RGB(200 + a / 2, 140, 30));
                HPEN penIn = CreatePen(PS_SOLID, 2, RGB(255, 200, 60));
                HGDIOBJ old = SelectObject(dc, penOuter);
                FrameRect(dc, &rc, (HBRUSH)GetStockObject(NULL_BRUSH));
                // 用矩形描边模拟发光
                RECT r1 = rc; InflateRect(&r1, -2, -2);
                SelectObject(dc, penMid);
                FrameRect(dc, &r1, (HBRUSH)GetStockObject(NULL_BRUSH));
                RECT r2 = rc; InflateRect(&r2, -4, -4);
                SelectObject(dc, penIn);
                FrameRect(dc, &r2, (HBRUSH)GetStockObject(NULL_BRUSH));
                SelectObject(dc, old);
                DeleteObject(penOuter); DeleteObject(penMid); DeleteObject(penIn);
            } else {
                // 未开播：灰色边框
                HBRUSH bg = CreateSolidBrush(RGB(30, 30, 32));
                FillRect(dc, &rc, bg);
                DeleteObject(bg);
                HBRUSH gray = CreateSolidBrush(RGB(90, 90, 95));
                FrameRect(dc, &rc, gray);
                DeleteObject(gray);
            }

            // 文字
            wchar_t line[512];
            int ln = SendMessageW(dis->hwndItem, LB_GETTEXT, dis->itemID,
                                  reinterpret_cast<LPARAM>(line));
            if (ln != LB_ERR) {
                DrawItemContent(dc, rc, std::wstring(line, line + ln), live, phase);
            }
            return TRUE;
        }
        case WM_TIMER:
            if (wParam == 2) {
                // 发光呼吸动画：重绘列表
                InvalidateRect(self->hList_, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) { self->Close(); return 0; }
            if (wParam == VK_RETURN && GetFocus() == self->hEdit_) {
                self->OnSearch(); return 0;
            }
            if (wParam == VK_RETURN && GetFocus() == self->hList_) {
                self->EnterSelected(); return 0;
            }
            break;
        case WM_SETFOCUS:
            if (self && self->hEdit_) SetFocus(self->hEdit_);
            return 0;
        case WM_CLOSE:
            self->Close();
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
