// test_api_console.cpp —— C++ 接口层自检（console，诊断/验证用）
// 验证：房间信息 / 流地址 / 弹幕配置(wbi签名) / 弹幕WS认证+心跳
#include <cstdio>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>

#include "bili_api.h"
#include "danmaku_client.h"

static int g_msgs = 0;
static std::atomic<bool> g_connected{false};

static void OnMsg(const DanmakuMessage& m, void*) {
    ++g_msgs;
    if (g_msgs <= 3)
        printf("  [danmaku] %s: %s\n", m.user.c_str(), m.text.c_str());
}
static void OnSt(DanmakuStatus st, void*) {
    const char* s = st == DanmakuStatus::Connected ? "connected" :
                    st == DanmakuStatus::Reconnecting ? "reconnecting" : "auth-failed";
    printf("  [status] %s\n", s);
    if (st == DanmakuStatus::Connected) g_connected = true;
}

int main(int argc, char** argv) {
    long long room = argc > 1 ? atoll(argv[1]) : 7734200;
    printf("== C++ API self-test, room=%lld ==\n", room);

    auto info = bili::GetRoomInfo(room);
    printf("[1] GetRoomInfo: %s", info ? "ok" : "FAIL");
    if (info) printf(" room_id=%lld live=%d title=%s", info->room_id, info->live_status, info->title.c_str());
    printf("\n");
    if (!info) return 1;
    if (info->live_status == 0) { printf("room not live, skipping stream/danmaku\n"); return 2; }

    auto stream = bili::GetStreamUrl(info->room_id);
    printf("[2] GetStreamUrl: %s", stream ? "ok" : "FAIL");
    if (stream) printf(" qn=%d fmt=%s len=%zu", stream->qn, stream->format.c_str(), stream->url.size());
    printf("\n");
    if (!stream) return 3;

    bili::Session session;
    auto danmu = bili::GetDanmakuConf(info->room_id, session);
    printf("[3] GetDanmakuConf: %s", danmu ? "ok" : "FAIL");
    if (danmu) printf(" token_len=%zu servers=%zu", danmu->token.size(), danmu->servers.size());
    printf("\n");
    if (!danmu) return 4;

    std::string buvid3;
    {
        std::string c = session.cookie_header;
        size_t pos = c.find("buvid3=");
        if (pos != std::string::npos) {
            size_t start = pos + 7;
            size_t end = c.find(';', start);
            buvid3 = c.substr(start, end == std::string::npos ? std::string::npos : end - start);
        }
    }
    printf("  buvid3=%s\n", buvid3.c_str());

    DanmakuClient client;
    client.SetCallbacks(OnMsg, OnSt, nullptr);
    client.Start(info->room_id, *danmu, buvid3);
    printf("[4] Danmaku WS connecting...\n");
    for (int i = 0; i < 40; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        // 跑满 20s，验证弹幕间隔期（>1s 无数据）不断开
    }
    printf("[5] danmaku connected=%d msgs=%d\n", (int)g_connected.load(), g_msgs);
    client.Stop();
    printf("== done ==\n");
    return 0;
}
