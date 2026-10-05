// test_ws_tls.cpp —— TLS 诊断（对比 443 与 2245）
#include "ws_socket.h"
#include <cstdio>

int main() {
    printf("[t] connecting api.bilibili.com:443 ...\n");
    {
        WsSocket ws;
        bool ok = ws.TlsConnect("api.bilibili.com", 443);
        printf("[t] 443: %s\n", ok ? "OK" : "FAIL");
    }
    printf("[t] connecting zj-cn-live-comet.chat.bilibili.com:2245 ...\n");
    {
        WsSocket ws;
        bool ok = ws.TlsConnect("zj-cn-live-comet.chat.bilibili.com", 2245);
        printf("[t] 2245: %s\n", ok ? "OK" : "FAIL");
    }
    return 0;
}
