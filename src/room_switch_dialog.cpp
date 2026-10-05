// ============================================================
// room_switch_dialog.cpp —— 切房输入对话框实现
// 动态创建：编辑框 + 确定/取消，模态消息循环。
// ============================================================

#include "room_switch_dialog.h"

namespace {

constexpr wchar_t kDlgClass[] = L"BiliRoomSwitchDlg";
constexpr int IDC_ROOM_EDIT = 1001;
constexpr int IDC_ROOM_OK = 1002;
constexpr int IDC_ROOM_CANCEL = 1003;

std::wstring g_result;
bool g_done = false;
bool g_ok = false;

void CloseDialog(HWND hDlg) {
    g_done = true;
    DestroyWindow(hDlg);
}

LRESULT CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_ROOM_OK) {
                wchar_t buf[512] = {0};
                GetDlgItemTextW(hDlg, IDC_ROOM_EDIT, buf, 511);
                g_result = buf;
                g_ok = true;
                CloseDialog(hDlg);
                return TRUE;
            }
            if (id == IDC_ROOM_CANCEL) {
                CloseDialog(hDlg);
                return TRUE;
            }
            return TRUE;
        }
        case WM_CLOSE:
            CloseDialog(hDlg);
            return TRUE;
        default:
            break;
    }
    return FALSE;
}

} // namespace

bool ShowRoomSwitchDialog(HWND parent, std::wstring& result) {
    g_result.clear();
    g_done = false;
    g_ok = false;

    // 注册窗口类（仅一次）
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kDlgClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    HWND hDlg = CreateWindowExW(0, kDlgClass, L"切换直播间",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 300, 130,
                                parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hDlg) return false;

    // 子控件
    CreateWindowExW(0, L"STATIC", L"输入房间号 / 短链 / 完整 URL：",
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    14, 12, 270, 18, hDlg, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                 14, 38, 260, 22, hDlg, reinterpret_cast<HMENU>(IDC_ROOM_EDIT),
                                 GetModuleHandleW(nullptr), nullptr);
    CreateWindowExW(0, L"BUTTON", L"确定",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                    90, 78, 70, 26, hDlg, reinterpret_cast<HMENU>(IDC_ROOM_OK),
                    GetModuleHandleW(nullptr), nullptr);
    CreateWindowExW(0, L"BUTTON", L"取消",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    170, 78, 70, 26, hDlg, reinterpret_cast<HMENU>(IDC_ROOM_CANCEL),
                    GetModuleHandleW(nullptr), nullptr);

    // 居中于父窗口
    RECT pr{};
    if (parent && GetWindowRect(parent, &pr)) {
        RECT dr{};
        GetWindowRect(hDlg, &dr);
        int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
        int x = pr.left + (pr.right - pr.left - dw) / 2;
        int y = pr.top + (pr.bottom - pr.top - dh) / 2;
        SetWindowPos(hDlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }

    // 模态循环
    EnableWindow(parent, FALSE);
    ShowWindow(hDlg, SW_SHOW);
    SetFocus(hEdit);
    MSG msg{};
    while (!g_done && GetMessageW(&msg, nullptr, 0, 0)) {
        // Enter / Esc 快速处理
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_RETURN) {
                wchar_t buf[512] = {0};
                GetDlgItemTextW(hDlg, IDC_ROOM_EDIT, buf, 511);
                g_result = buf;
                g_ok = true;
                CloseDialog(hDlg);
                continue;
            }
            if (msg.wParam == VK_ESCAPE) {
                CloseDialog(hDlg);
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    result = g_result;
    return g_ok;
}
