// ============================================================
// app_window.cpp —— 主窗口实现
// 窗口结构：主窗口(无边框置顶) ─ 视频子窗口(mpv --wid)
//                       └ 弹幕透明层子窗口(D2D/DWrite)
// 穿透模式：WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_LAYERED
//           + WM_NCHITTEST 恒 HTTRANSPARENT。
// ============================================================

#include "app_window.h"

#include "room_switch_dialog.h"
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdarg>

namespace {
// 运行日志（诊断用，写在 exe 同目录 run.log）
void LogMsg(const char* fmt, ...) {
    FILE* f = nullptr;
    _wfopen_s(&f, L"D:\\bilibilibilil\\build\\run.log", L"a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputs("\n", f);
    fclose(f);
}
// 音量条 HUD 窗口：WM_PAINT 自绘进度条 + 百分比
LRESULT CALLBACK VolBarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        int vol = static_cast<int>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (vol < 0) vol = 0; if (vol > 100) vol = 100;
        HBRUSH bg = CreateSolidBrush(RGB(18, 18, 22));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        RECT bar = { 12, rc.bottom / 2 - 9, rc.right - 12, rc.bottom / 2 + 9 };
        HBRUSH track = CreateSolidBrush(RGB(58, 58, 66));
        FillRect(dc, &bar, track);
        DeleteObject(track);
        int fillW = static_cast<int>(static_cast<double>(bar.right - bar.left) * vol / 100.0);
        RECT fill = { bar.left, bar.top, bar.left + fillW, bar.bottom };
        COLORREF c = vol > 66 ? RGB(90, 200, 90) : vol > 33 ? RGB(240, 190, 60) : RGB(230, 90, 70);
        HBRUSH f = CreateSolidBrush(c);
        FillRect(dc, &fill, f);
        DeleteObject(f);
        HBRUSH bd = CreateSolidBrush(RGB(120, 120, 130));
        FrameRect(dc, &rc, bd);
        DeleteObject(bd);
        wchar_t txt[32];
        swprintf_s(txt, L"音量 %d%%", vol);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(240, 240, 240));
        HFONT font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei");
        HFONT oldF = static_cast<HFONT>(SelectObject(dc, font));
        RECT tr = rc;
        DrawTextW(dc, txt, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oldF);
        DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

constexpr wchar_t kMainClass[] = L"BiliLiveFloatMain";
constexpr int kTimerDanmaku = 1;     // 弹幕动画定时器（33ms）
constexpr int kTimerToast = 2;       // toast 隐藏定时器
constexpr int kTimerVolBar = 3;      // 音量条隐藏定时器
constexpr int kHotkeyPassthrough = 1;
constexpr int kHotkeySwitchRoom = 2;
constexpr int kHotkeySearch = 3;
constexpr int kEdgeSize = 8;         // 边缘缩放热区（像素）
constexpr int kMinWidth = 240, kMinHeight = 135;

// 菜单命令 ID
enum {
    IDM_ROOM_SWITCH = 4001,
    IDM_RECENT_BASE = 4100,   // 最近直播间：IDM_RECENT_BASE + index
    IDM_TOGGLE_PASSTHROUGH,
    IDM_TOGGLE_MUTE,
    IDM_VOLUME_UP,
    IDM_VOLUME_DOWN,
    IDM_TOGGLE_TOPMOST,
    IDM_EXIT,
    IDM_SEARCH,   // 打开主播搜索窗口
    IDM_QUALITY_BASE = 4300,  // 画质子菜单：IDM_QUALITY_BASE + index
};

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

} // namespace

AppWindow* AppWindow::s_instance = nullptr;

AppWindow::AppWindow() {
    s_instance = this;
}
AppWindow::~AppWindow() {
    if (s_instance == this) s_instance = nullptr;
}

bool AppWindow::Create(HINSTANCE hInst, const AppConfig& cfg) {
    hInst_ = hInst;
    cfg_ = cfg;
    passthrough_ = cfg_.passthrough;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &AppWindow::WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kMainClass;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    ATOM cls = RegisterClassExW(&wc);
    LogMsg("[create] RegisterClassExW atom=%u err=%lu", (unsigned)cls, GetLastError());

    DWORD exStyle = WS_EX_TOPMOST;
    if (passthrough_) exStyle |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_LAYERED;

    int x = cfg_.x >= 0 ? cfg_.x : CW_USEDEFAULT;
    int y = cfg_.y >= 0 ? cfg_.y : CW_USEDEFAULT;
    LogMsg("[create] exstyle=0x%08X style=0x%08X pos=(%d,%d) size=%dx%d",
           exStyle, (DWORD)(WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN), x, y, cfg_.w, cfg_.h);
    hwnd_ = CreateWindowExW(exStyle, kMainClass, L"BiliLiveFloat",
                            WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN,
                            x, y, cfg_.w, cfg_.h,
                            nullptr, nullptr, hInst, nullptr);
    LogMsg("[create] CreateWindowExW hwnd=%p err=%lu", (void*)hwnd_, GetLastError());
    if (!hwnd_) return false;

    // 子窗口：视频（mpv 嵌入）
    hVideo_ = CreateWindowExW(0, L"STATIC", L"",
                              WS_CHILD | WS_VISIBLE,
                              0, 0, 1, 1, hwnd_, nullptr, hInst, nullptr);
    LogMsg("[create] hVideo_=%p err=%lu", (void*)hVideo_, GetLastError());
    // 弹幕透明层：独立顶层分层窗口（WS_EX_LAYERED 不能与 WS_CHILD 组合，
    // CreateWindowEx 直接报 ERROR_INVALID_PARAMETER(87)，实测确认）
    // 用同一窗口类共享 WndProc；UpdateLayeredWindow 合成；跟随主窗口移动
    hDanmaku_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                                WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                kMainClass, L"",
                                WS_POPUP | WS_VISIBLE,
                                0, 0, 1, 1, nullptr, nullptr, hInst, nullptr);
    LogMsg("[create] hDanmaku_=%p err=%lu", (void*)hDanmaku_, GetLastError());
    if (!hVideo_ || !hDanmaku_) return false;

    UpdateChildren();
    LogMsg("[create] UpdateChildren done err=%lu", GetLastError());
    renderer_.Init(hDanmaku_);
    LogMsg("[create] renderer Init done err=%lu", GetLastError());
    if (!player_.Init(hVideo_, cfg_.volume, cfg_.mute)) {
        LogMsg("[create] player Init FAIL err=%lu", GetLastError());
        ShowToast(L"播放内核初始化失败");
        return false;
    }
    LogMsg("[create] player Init OK");

    // 事件与弹幕回调
    player_.SetEventCallback(&AppWindow::OnPlayerEvent, this);
    danmaku_.SetCallbacks(&AppWindow::OnDanmakuMessage, &AppWindow::OnDanmakuStatus, this);
    // 搜索窗口回调
    searchWnd_.SetEnterRoomCallback(&AppWindow::OnSearchEnterRoom, this);

    // 全局热键：Ctrl+Alt+X 穿透切换、Ctrl+Alt+R 切房
    RegisterHotKey(hwnd_, kHotkeyPassthrough,
                   MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'X');
    RegisterHotKey(hwnd_, kHotkeySwitchRoom,
                   MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'R');
    RegisterHotKey(hwnd_, kHotkeySearch,
                   MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'S');

    // 弹幕动画定时器
    SetTimer(hwnd_, kTimerDanmaku, 33, nullptr);

    // 应用图标（任务栏 + Alt+Tab）
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(LoadIconW(hInst_, MAKEINTRESOURCEW(100))));
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG,
                 reinterpret_cast<LPARAM>(LoadIconW(hInst_, MAKEINTRESOURCEW(100))));
    // 托盘
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP + 3;  // 托盘消息
    nid.hIcon = LoadIconW(hInst_, MAKEINTRESOURCEW(100));
    wcscpy_s(nid.szTip, L"BiliLiveFloat（右键菜单：搜索/画质/音量）");
    Shell_NotifyIconW(NIM_ADD, &nid);

    return true;
}

void AppWindow::SaveConfig() {
    RECT rc{};
    if (GetWindowRect(hwnd_, &rc)) {
        cfg_.x = rc.left;
        cfg_.y = rc.top;
        cfg_.w = rc.right - rc.left;
        cfg_.h = rc.bottom - rc.top;
    }
    cfg_.volume = player_.GetVolume();
    cfg_.mute = player_.GetMute();
    cfg_.passthrough = passthrough_;
    cfg_.last_room = currentRoom_;
    Config().Save(cfg_);
}

// ---- 消息处理 -------------------------------------------------

LRESULT CALLBACK AppWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppWindow* self = s_instance;
    if (!self && msg == WM_NCCREATE) return TRUE;
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_CREATE:
            return 0;
        case WM_NCHITTEST: {
            if (hwnd == self->hDanmaku_) return HTTRANSPARENT;
            POINT pt{ LOWORD(lParam), HIWORD(lParam) };
            return self->OnNcHitTest(pt);
        }
        case WM_MOUSEWHEEL: {
            short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            self->AdjustVolume(delta > 0 ? 5 : -5);
            return 0;
        }
        case WM_CONTEXTMENU: {
            POINT pt{ LOWORD(lParam), HIWORD(lParam) };
            self->ShowContextMenu(pt);
            return 0;
        }
        case WM_SIZE:
            if (hwnd != self->hDanmaku_) self->OnSize();
            return 0;
        case WM_MOVE:
            // 拖动主窗口时同步弹幕顶层窗口与视频子窗口重绘，避免画面分离
            if (hwnd != self->hDanmaku_) {
                self->UpdateChildren();
                if (self->hVideo_) {
                    InvalidateRect(self->hVideo_, nullptr, TRUE);
                    RedrawWindow(self->hVideo_, nullptr, nullptr,
                                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
                }
            }
            return 0;
        case WM_TIMER:
            if (wParam == kTimerDanmaku) {
                self->renderer_.Tick();
            } else if (wParam == kTimerToast && self->hToast_) {
                KillTimer(hwnd, kTimerToast);
                ShowWindow(self->hToast_, SW_HIDE);
            } else if (wParam == kTimerVolBar && self->hVolBar_) {
                KillTimer(hwnd, kTimerVolBar);
                ShowWindow(self->hVolBar_, SW_HIDE);
            }
            return 0;
        case WM_HOTKEY: {
            if (wParam == kHotkeyPassthrough) {
                self->TogglePassthrough();
            } else if (wParam == kHotkeySwitchRoom) {
                std::wstring input;
                if (ShowRoomSwitchDialog(hwnd, input) && !input.empty()) {
                    self->SwitchToRoom(input);
                }
            } else if (wParam == kHotkeySearch) {
                self->searchWnd_.Open(self->hInst_, hwnd);
            }
            return 0;
        }
        case WM_CLOSE:
            if (hwnd == self->hDanmaku_) { DestroyWindow(hwnd); return 0; }
            self->SaveConfig();
            self->danmaku_.Stop();
            self->player_.Shutdown();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY: {
            if (hwnd == self->hDanmaku_) { DestroyWindow(hwnd); return 0; }
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd;
            nid.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            PostQuitMessage(0);
            return 0;
        }
        case WM_APP_DANMAKU_STATUS:
            self->HandleDanmakuStatus(static_cast<DanmakuStatus>(wParam));
            return 0;
        case WM_APP_PLAYER_EVENT:
            self->HandlePlayerEvent(static_cast<Player::Event>(wParam));
            return 0;
        case WM_APP + 10:
            self->HandleSearchEnterRoom(static_cast<long long>(lParam));
            return 0;
        default:
            // 托盘消息
            if (msg == WM_APP + 3) {
                if (LOWORD(lParam) == WM_RBUTTONUP) {
                    POINT pt{};
                    GetCursorPos(&pt);
                    self->ShowContextMenu(pt);
                } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
                    self->RestoreFromTray();
                }
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// WM_NCHITTEST：控制模式可拖动/边缘缩放；穿透模式恒穿透
LRESULT AppWindow::OnNcHitTest(POINT pt) {
    if (passthrough_) return HTTRANSPARENT;
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int x = pt.x - rc.left, y = pt.y - rc.top;
    bool l = x < kEdgeSize, r = x >= w - kEdgeSize;
    bool t = y < kEdgeSize, b = y >= h - kEdgeSize;
    if (l && t) return HTTOPLEFT;
    if (r && t) return HTTOPRIGHT;
    if (l && b) return HTBOTTOMLEFT;
    if (r && b) return HTBOTTOMRIGHT;
    if (l) return HTLEFT;
    if (r) return HTRIGHT;
    if (t) return HTTOP;
    if (b) return HTBOTTOM;
    return HTCAPTION;  // 整窗拖动
}

void AppWindow::OnSize() {
    UpdateChildren();
}

// 子窗口跟随主窗口客户区
void AppWindow::UpdateChildren() {
    if (!hwnd_ || !hVideo_ || !hDanmaku_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    SetWindowPos(hVideo_, HWND_TOP, 0, 0, w, h, SWP_NOACTIVATE);
    // 弹幕层是顶层窗口：位置取主窗口客户区屏幕坐标，置顶保证盖在视频之上
    POINT origin{ 0, 0 };
    ClientToScreen(hwnd_, &origin);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(hDanmaku_, HWND_TOPMOST, origin.x, origin.y, w, h, SWP_NOACTIVATE);
    renderer_.SetSize(static_cast<UINT>(w), static_cast<UINT>(h));
}

// ---- 双模式 ---------------------------------------------------

void AppWindow::ApplyPassthrough(bool on) {
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (on) {
        ex |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_LAYERED;
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
        SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
        ShowToast(L"穿透模式：已开启（Ctrl+Alt+X 切回）");
    } else {
        ex &= ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_LAYERED);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
        ShowToast(L"控制模式：已恢复");
    }
    passthrough_ = on;
}

void AppWindow::TogglePassthrough() {
    ApplyPassthrough(!passthrough_);
}

void AppWindow::ApplyTopmost(bool on) {
    SetWindowPos(hwnd_, on ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

void AppWindow::ToggleTopmost() {
    cfg_.topmost = !cfg_.topmost;
    ApplyTopmost(cfg_.topmost);
    ShowToast(cfg_.topmost ? L"已置顶" : L"已取消置顶");
}

void AppWindow::ToggleMute() {
    player_.SetMute(!player_.GetMute());
}

void AppWindow::AdjustVolume(int delta) {
    int vol = std::clamp(player_.GetVolume() + delta, 0, 100);
    player_.SetVolume(vol);
    ShowVolumeBar(vol);
}

// ---- 音量条 HUD（0-100 可视化）-------------------------------

void AppWindow::ShowVolumeBar(int vol) {
    if (!hwnd_) return;
    if (!hVolBar_) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = VolBarWndProc;
        wc.hInstance = hInst_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"BiliLiveFloatVolBar";
        RegisterClassExW(&wc);
        hVolBar_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                   L"BiliLiveFloatVolBar", L"", WS_POPUP | WS_VISIBLE,
                                   0, 0, 300, 64, nullptr, nullptr, hInst_, nullptr);
        if (hVolBar_) SetLayeredWindowAttributes(hVolBar_, 0, 235, LWA_ALPHA);
    }
    if (!hVolBar_) return;
    SetWindowLongPtrW(hVolBar_, GWLP_USERDATA, vol);
    InvalidateRect(hVolBar_, nullptr, TRUE);
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    SetWindowPos(hVolBar_, HWND_TOPMOST,
                 rc.left + w / 2 - 150, rc.top + h / 2 - 32, 300, 64,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    KillTimer(hwnd_, kTimerVolBar);
    SetTimer(hwnd_, kTimerVolBar, 1500, nullptr);
}

// ---- 画质切换 ------------------------------------------------

void AppWindow::SetQuality(int qn, const wchar_t* name) {
    if (currentRoom_ == 0) { ShowToast(L"请先进入直播间"); return; }
    currentQn_ = qn;
    auto stream = bili::GetStreamUrl(currentRoom_, qn);
    if (!stream) {
        ShowToast(L"该画质暂不可用");
        return;
    }
    if (!player_.LoadUrl(stream->url)) {
        ShowToast(L"画质切换失败");
        return;
    }
    // 弹幕连接不受影响，继续
    wchar_t buf[96];
    swprintf_s(buf, L"画质：%s", name ? name : L"");
    ShowToast(buf);
}

// ---- 右键菜单 -------------------------------------------------

void AppWindow::ShowContextMenu(POINT pt) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_ROOM_SWITCH, L"切换直播间...\tCtrl+Alt+R");
    // 最近直播间子菜单
    HMENU recent = CreatePopupMenu();
    if (cfg_.recent_rooms.empty()) {
        AppendMenuW(recent, MF_STRING | MF_GRAYED, IDM_RECENT_BASE, L"（无）");
    } else {
        for (size_t i = 0; i < cfg_.recent_rooms.size() && i < 10; ++i) {
            wchar_t buf[64];
            swprintf_s(buf, L"房间 %lld", cfg_.recent_rooms[i]);
            AppendMenuW(recent, MF_STRING, IDM_RECENT_BASE + static_cast<int>(i), buf);
        }
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(recent), L"最近直播间");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TOGGLE_PASSTHROUGH,
                passthrough_ ? L"控制模式\tCtrl+Alt+X" : L"穿透模式\tCtrl+Alt+X");
    wchar_t volTxt[64];
    swprintf_s(volTxt, L"音量 %d%%（Ctrl+滚轮/↑↓）", player_.GetVolume());
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, volTxt);
    AppendMenuW(menu, MF_STRING, IDM_TOGGLE_MUTE,
                player_.GetMute() ? L"取消静音\t空格" : L"静音\t空格");
    AppendMenuW(menu, MF_STRING, IDM_VOLUME_UP, L"音量 +\tCtrl+↑");
    AppendMenuW(menu, MF_STRING, IDM_VOLUME_DOWN, L"音量 -\tCtrl+↓");
    // 画质子菜单（按房间可用画质）
    HMENU qmenu = CreatePopupMenu();
    if (currentRoom_ == 0 || acceptQn_.empty()) {
        AppendMenuW(qmenu, MF_STRING | MF_GRAYED, IDM_QUALITY_BASE, L"（未进入直播间）");
    } else {
        for (size_t i = 0; i < acceptQn_.size(); ++i) {
            UINT flag = MF_STRING;
            if (acceptQn_[i].qn == currentQn_) flag |= MF_CHECKED;
            std::wstring d = Utf8ToWide(acceptQn_[i].desc);
            AppendMenuW(qmenu, flag, IDM_QUALITY_BASE + static_cast<UINT>(i), d.c_str());
        }
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(qmenu), L"画质");
    AppendMenuW(menu, MF_STRING, IDM_TOGGLE_TOPMOST,
                cfg_.topmost ? L"取消置顶" : L"始终置顶");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_SEARCH, L"搜索直播间...\tCtrl+Alt+S");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");

    UINT id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                             pt.x, pt.y, 0, hwnd_, nullptr);
    switch (id) {
        case IDM_ROOM_SWITCH: {
            std::wstring input;
            if (ShowRoomSwitchDialog(hwnd_, input) && !input.empty()) {
                SwitchToRoom(input);
            }
            break;
        }
        case IDM_TOGGLE_PASSTHROUGH:
            TogglePassthrough();
            break;
        case IDM_TOGGLE_MUTE:
            ToggleMute();
            break;
        case IDM_VOLUME_UP:
            AdjustVolume(5);
            break;
        case IDM_VOLUME_DOWN:
            AdjustVolume(-5);
            break;
        case IDM_TOGGLE_TOPMOST:
            ToggleTopmost();
            break;
        case IDM_SEARCH:
            searchWnd_.Open(hInst_, hwnd_);
            break;
        case IDM_EXIT:
            SendMessageW(hwnd_, WM_CLOSE, 0, 0);
            break;
        default:
            if (id >= IDM_QUALITY_BASE && id < IDM_QUALITY_BASE + 16) {
                size_t qidx = static_cast<size_t>(id - IDM_QUALITY_BASE);
                if (qidx < acceptQn_.size()) {
                    const auto& q = acceptQn_[qidx];
                    SetQuality(q.qn, Utf8ToWide(q.desc).c_str());
                }
            } else if (id >= IDM_RECENT_BASE && id < IDM_RECENT_BASE + 10) {
                size_t idx = static_cast<size_t>(id - IDM_RECENT_BASE);
                if (idx < cfg_.recent_rooms.size()) {
                    SwitchToRoom(cfg_.recent_rooms[idx]);
                }
            }
            break;
    }
    DestroyMenu(menu);
    if (recent) DestroyMenu(recent);
}

// ---- Toast 提示（右上角 2 秒）-------------------------------

void AppWindow::ShowToast(const std::wstring& text) {
    if (!hwnd_) return;
    if (!hToast_) {
        hToast_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST, L"STATIC", L"",
                                  WS_POPUP | WS_VISIBLE | SS_CENTER,
                                  0, 0, 200, 28, nullptr, nullptr, hInst_, nullptr);
    }
    SetWindowTextW(hToast_, text.c_str());
    // 半透明背景
    SetLayeredWindowAttributes(hToast_, 0, 220, LWA_ALPHA);

    // 放到主窗口右上角
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    SetWindowPos(hToast_, HWND_TOPMOST,
                 rc.left + w - 210, rc.top + 8, 200, 28,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    KillTimer(hwnd_, kTimerToast);
    SetTimer(hwnd_, kTimerToast, 2000, nullptr);
}

void AppWindow::RestoreFromTray() {
    ShowWindow(hwnd_, SW_SHOW);
    SetWindowPos(hwnd_, passthrough_ ? HWND_NOTOPMOST : HWND_TOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

// ---- 弹幕回调（网络线程）→ 线程安全入队 / 投递状态 ----------

void AppWindow::OnDanmakuMessage(const DanmakuMessage& msg, void* ctx) {
    auto* self = static_cast<AppWindow*>(ctx);
    if (!self || !self->cfg_.danmaku_enabled) return;
    self->renderer_.PushMessage(msg);
}

void AppWindow::OnDanmakuStatus(DanmakuStatus st, void* ctx) {
    auto* self = static_cast<AppWindow*>(ctx);
    if (!self || !self->hwnd_) return;
    // 网络线程 → UI 线程
    PostMessageW(self->hwnd_, WM_APP_DANMAKU_STATUS,
                 static_cast<WPARAM>(st), 0);
}

void AppWindow::OnSearchEnterRoom(long long roomid, void* ctx) {
    auto* self = static_cast<AppWindow*>(ctx);
    if (!self) return;
    PostMessageW(self->hwnd_, WM_APP + 10, 0, static_cast<LPARAM>(roomid));
}

void AppWindow::HandleSearchEnterRoom(long long roomid) {
    SwitchToRoom(roomid);
}

void AppWindow::OnPlayerEvent(Player*, Player::Event ev, void* ctx) {
    auto* self = static_cast<AppWindow*>(ctx);
    if (!self || !self->hwnd_) return;
    PostMessageW(self->hwnd_, WM_APP_PLAYER_EVENT, static_cast<WPARAM>(ev), 0);
}

// ---- 切房流程 ------------------------------------------------

void AppWindow::SwitchToRoom(const std::wstring& input) {
    auto room = bili::ParseRoomInput(input);
    if (!room) {
        ShowToast(L"无法识别输入，请输入房间号/短链/完整 URL");
        return;
    }
    StartSwitchRoomFlow(*room);
}

void AppWindow::SwitchToRoom(long long roomOrShort) {
    StartSwitchRoomFlow(roomOrShort);
}

void AppWindow::StartSwitchRoomFlow(long long roomOrShort) {
    LogMsg("[switch] begin room=%lld", roomOrShort);
    ShowToast(L"正在解析直播间...");
    // 1) 房间信息（短号 → 真实房号；未开播提示）
    auto info = bili::GetRoomInfo(roomOrShort);
    LogMsg("[switch] GetRoomInfo %s", info ? "ok" : "FAIL");
    if (!info) {
        ShowToast(L"获取房间信息失败，请检查网络或房号");
        return;
    }
    LogMsg("[switch] live_status=%d room=%lld", info->live_status, info->room_id);
    if (info->live_status == 0) {
        wchar_t buf[128];
        swprintf_s(buf, L"直播间 %lld 未开播", info->room_id);
        ShowToast(buf);
        return;
    }
    // 2) 流地址（用当前选择的画质）
    auto stream = bili::GetStreamUrl(info->room_id, currentQn_);
    LogMsg("[switch] GetStreamUrl %s", stream ? "ok" : "FAIL");
    if (!stream) {
        ShowToast(L"获取播放流失败（可能未开播或受限）");
        return;
    }
    // 3) 弹幕配置
    auto danmu = bili::GetDanmakuConf(info->room_id, session_);
    LogMsg("[switch] GetDanmakuConf %s", danmu ? "ok" : "FAIL");
    LogMsg("[switch] stream=%s", stream->url.c_str());
    // 4) 播放 + 弹幕
    if (!player_.LoadUrl(stream->url)) {
        ShowToast(L"播放失败");
        return;
    }
    // 提取 buvid3（cookie 里）传给弹幕客户端
    std::string buvid3;
    {
        std::string c = session_.cookie_header;
        size_t pos = c.find("buvid3=");
        if (pos != std::string::npos) {
            size_t start = pos + 7;
            size_t end = c.find(';', start);
            buvid3 = c.substr(start, end == std::string::npos ? std::string::npos : end - start);
        }
    }
    if (danmu) {
        danmaku_.Start(info->room_id, *danmu, buvid3);
    }
    renderer_.Clear();

    // 刷新该房间可用画质列表（托盘菜单用）
    acceptQn_ = bili::GetAcceptQn(info->room_id);

    currentRoom_ = info->room_id;
    currentTitle_ = std::wstring(info->title.begin(), info->title.end());

    // 5) 更新最近直播间（去重、最新在前、最多 10）
    auto& rec = cfg_.recent_rooms;
    rec.erase(std::remove(rec.begin(), rec.end(), currentRoom_), rec.end());
    rec.insert(rec.begin(), currentRoom_);
    if (rec.size() > 10) rec.resize(10);
    SaveConfig();

    wchar_t title[256];
    swprintf_s(title, L"BiliLiveFloat - 房间 %lld", currentRoom_);
    SetWindowTextW(hwnd_, title);
    ShowToast(L"已进入直播间 " + std::to_wstring(currentRoom_));
}

// 弹幕状态（UI 线程）
void AppWindow::HandleDanmakuStatus(DanmakuStatus st) {
    switch (st) {
        case DanmakuStatus::Connected:
            break;  // 静默（避免打扰）
        case DanmakuStatus::Reconnecting:
            ShowToast(L"弹幕连接断开，正在重连...");
            break;
        case DanmakuStatus::AuthFailed:
            ShowToast(L"弹幕认证失败，将自动重试");
            break;
        default:
            break;
    }
}

void AppWindow::HandlePlayerEvent(Player::Event ev) {
    switch (ev) {
        case Player::Event::EndFile:
            ShowToast(L"直播流已结束或断开，正在重新获取...");
            if (currentRoom_ != 0) {
                // 尝试重新拉流（简单重试一次）
                auto stream = bili::GetStreamUrl(currentRoom_);
                if (stream) player_.LoadUrl(stream->url);
            }
            break;
        case Player::Event::Error:
            ShowToast(L"播放出错（断流？）");
            break;
        case Player::Event::PlaybackStarted:
            break;
        default:
            break;
    }
}
