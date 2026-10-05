// test_tcp.cpp —— 原始 socket HTTP 连通测试（诊断中间层拦截）
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { printf("[t] socket fail\n"); return 1; }
    hostent* he = gethostbyname("api.bilibili.com");
    if (!he) { printf("[t] dns fail\n"); return 1; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(443);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    printf("[t] connecting %s...\n", inet_ntoa(addr.sin_addr));
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        printf("[t] connect fail %d\n", WSAGetLastError());
        return 1;
    }
    printf("[t] connected\n");
    const char* req = "GET / HTTP/1.1\r\nHost: api.bilibili.com\r\nConnection: close\r\n\r\n";
    if (send(s, req, (int)strlen(req), 0) <= 0) {
        printf("[t] send fail %d\n", WSAGetLastError());
        return 1;
    }
    printf("[t] sent %d bytes\n", (int)strlen(req));
    DWORD tv = 10000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    char buf[4096];
    int r = recv(s, buf, sizeof(buf), 0);
    if (r > 0) {
        buf[r] = 0;
        printf("[t] recv %d bytes: %.80s\n", r, buf);
    } else {
        printf("[t] recv fail/empty err=%d\n", WSAGetLastError());
    }
    closesocket(s);
    WSACleanup();
    return 0;
}
