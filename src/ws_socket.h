#pragma once
// ============================================================
// ws_socket.h —— 基于 Win32 socket + schannel 的 WSS 客户端
// 解决 WinHTTP WebSocket API 与 B站弹幕服务器 TLS 不兼容问题
// （schannel TLS13 实测可连通；此处自实现 WS 握手与帧收发）。
// ============================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#define SECURITY_WIN32
#include <security.h>
#include <schannel.h>

#include <string>
#include <vector>
#include <cstdint>

// 轻量 WSS 客户端（单连接）
class WsSocket {
public:
    WsSocket() = default;
    ~WsSocket();

    // 连接并完成 TLS 握手与 WebSocket 升级（阻塞）
    bool Connect(const std::string& host, int port);

    // 仅完成 TCP+TLS（诊断用）
    bool TlsConnect(const std::string& host, int port);

    // 发送一帧二进制消息（自动掩码，客户端必须掩码）
    bool Send(const unsigned char* data, size_t len);

    // 读取一帧消息的 payload；返回 false 表示连接关闭/错误
    // 若为控制帧（ping/close），内部处理并继续读取
    bool Receive(std::vector<unsigned char>& payload);

    void Close();

private:
    // schannel
    bool TlsHandshake();
    bool TlsWrite(const unsigned char* data, size_t len);
    bool TlsDecrypt(size_t& outLen);
    void TlsCleanup();

    // ws 帧
    bool WsReadFrame(std::vector<unsigned char>& payload);

    SOCKET sock_ = INVALID_SOCKET;
    bool tlsReady_ = false;
    std::string targetHost_;
    void* pCred_ = nullptr;   // CredHandle*
    void* pCtx_ = nullptr;    // CtxtHandle*
    std::vector<unsigned char> tlsInBuf_;   // TLS 记录缓冲
    std::vector<unsigned char> frameBuf_;   // 解密后的应用数据缓冲
};
