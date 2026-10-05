// ============================================================
// main.cpp —— 程序入口
// 用法：BiliLiveFloat.exe [房间号|短链|URL]
// 无参数时恢复上次房间（配置里有 last_room）。
// ============================================================

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdarg>
#include <string>

#include "app_window.h"
#include "config.h"

static void MLog(const char* fmt, ...) {
    FILE* f = nullptr;
    _wfopen_s(&f, L"D:\\bilibilibilil\\build\\main.log", L"a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    MLog("[main] WinMain entered");
    // 解析命令行参数（可选：房间号/短链/完整 URL）
    std::wstring argRoom;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        if (argc > 1) argRoom = argv[1];
        LocalFree(argv);
    }
    MLog("[main] argc=%d room=%ls", argc, argRoom.c_str());

    AppConfig cfg = Config().Load();
    MLog("[main] Config.Load done w=%d h=%d last=%lld", cfg.w, cfg.h, cfg.last_room);
    AppWindow window;
    MLog("[main] before Create");
    bool created = window.Create(hInstance, cfg);
    MLog("[main] after Create ok=%d", (int)created);
    if (!created) {
        // 诊断：记录失败原因到 exe 同目录 create_fail.log
        DWORD err = GetLastError();
        FILE* f = nullptr;
        _wfopen_s(&f, L"create_fail.log", L"w");
        if (f) {
            fprintf(f, "Create failed, GetLastError=0x%08X\n", err);
            fclose(f);
        }
        MessageBoxW(nullptr, L"窗口创建失败", L"BiliLiveFloat", MB_ICONERROR);
        return 1;
    }

    // 启动播放：命令行优先，其次恢复上次房间；都没有则播默认直播间
    // （首次运行；房号 6 为 B站英雄联盟赛事直播等热门，未开播时窗口会给出提示）
    if (!argRoom.empty()) {
        window.SwitchToRoom(argRoom);
    } else if (cfg.last_room != 0) {
        window.SwitchToRoom(cfg.last_room);
    } else {
        // 默认测试直播间：CS-advent（CS2 解说，房号 1883358196）
        window.SwitchToRoom(1883358196);
    }

    // 主消息循环
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
