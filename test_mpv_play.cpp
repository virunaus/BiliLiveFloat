// test_mpv_play.cpp —— 控制台验证 mpv wid 嵌入播放 B站直播流
// 用法：test_mpv_play.exe <房号> [秒数]
// 说明：mpv 日志（warn 级）会直接打印到 stderr，可用重定向捕获
#include <windows.h>
#include <cstdio>
#include <string>
#include <atomic>

#include "mpv/client.h"
#include "bili_api.h"

int main(int argc, char** argv) {
    long long room = 6;
    if (argc > 1) room = atoll(argv[1]);
    int waitSec = 12;
    if (argc > 2) waitSec = atoi(argv[2]);

    auto info = bili::GetRoomInfo(room);
    if (!info) { printf("GetRoomInfo FAIL\n"); return 1; }
    printf("room=%lld live=%d title=%s\n", info->room_id, info->live_status, info->title.c_str());
    auto stream = bili::GetStreamUrl(info->room_id);
    if (!stream) { printf("GetStreamUrl FAIL\n"); return 1; }
    printf("stream url len=%zu\n", stream->url.size());

    // 创建窗口（console 程序也能创建窗口）
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = L"T_MPV";
    RegisterClassExW(&wc);
    HWND video = CreateWindowExW(0, L"T_MPV", L"v", WS_POPUP | WS_VISIBLE, 100, 100, 640, 360,
                                 nullptr, nullptr, hInst, nullptr);
    printf("video hwnd=%p\n", (void*)video);

    mpv_handle* mpv = mpv_create();
    if (!mpv) { printf("mpv_create FAIL\n"); return 1; }
    mpv_request_log_messages(mpv, "warn");
    mpv_set_option_string(mpv, "vo", "gpu");
    mpv_set_option_string(mpv, "gpu-api", "d3d11");
    mpv_set_option_string(mpv, "hwdec", "auto");
    char wid[32];
    snprintf(wid, sizeof(wid), "%lld", (long long)(INT_PTR)video);
    mpv_set_option_string(mpv, "wid", wid);
    mpv_set_option_string(mpv, "http-header-fields",
        "Referer: https://live.bilibili.com, User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    // B站 CDN 的 HTTP/2 与 curl 兼容性差（HTTP/2 framing layer error），强制 HTTP/1.1
    mpv_set_option_string(mpv, "http-version", "1.1");
    mpv_set_option_string(mpv, "cache", "yes");
    if (mpv_initialize(mpv) < 0) { printf("mpv_initialize FAIL\n"); return 1; }
    printf("mpv initialized\n");

    const char* args[] = { "loadfile", stream->url.c_str(), "replace", nullptr };
    int rc = mpv_command(mpv, args);
    printf("loadfile rc=%d\n", rc);

    // 轮询事件
    time_t t0 = time(nullptr);
    while (time(nullptr) - t0 < waitSec) {
        mpv_event* ev = mpv_wait_event(mpv, 0.2);
        if (ev->event_id == MPV_EVENT_FILE_LOADED) {
            printf("EVENT FILE_LOADED\n");
            break;
        } else if (ev->event_id == MPV_EVENT_END_FILE) {
            auto* end = static_cast<mpv_event_end_file*>(ev->data);
            printf("EVENT END_FILE reason=%d\n", end ? end->reason : -1);
            break;
        } else if (ev->event_id == MPV_EVENT_PLAYBACK_RESTART) {
            printf("EVENT PLAYBACK_RESTART\n");
            break;
        } else if (ev->event_id == MPV_EVENT_START_FILE) {
            printf("EVENT START_FILE\n");
        } else if (ev->event_id != MPV_EVENT_NONE) {
            printf("EVENT id=%d\n", (int)ev->event_id);
        }
    }
    // 播放状态查询
    int paused = -1;
    mpv_get_property(mpv, "pause", MPV_FORMAT_FLAG, &paused);
    printf("pause=%d\n", paused);
    mpv_destroy(mpv);
    DestroyWindow(video);
    printf("done\n");
    return 0;
}
