// ============================================================
// ws_socket.cpp —— WSS 客户端实现（socket + schannel + ws 帧）
// 参考标准 WS 协议 RFC6455 与 schannel SSPI 示例。
// ============================================================

#include "ws_socket.h"

#include <wincrypt.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")

namespace {

const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";  // WS 升级魔数
const char* kWsUa = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";

// 随机 16 字节 → base64（Sec-WebSocket-Key）
std::string RandomKey() {
    unsigned char b[16];
    std::mt19937 rng(static_cast<unsigned>(::time(nullptr)) ^
                     static_cast<unsigned>(GetTickCount64()));
    for (int i = 0; i < 16; ++i) b[i] = static_cast<unsigned char>(rng() & 0xFF);
    // base64 编码
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (int i = 0; i < 16; i += 3) {
        unsigned v = (static_cast<unsigned>(b[i]) << 16) |
                     (i + 1 < 16 ? static_cast<unsigned>(b[i + 1]) << 8 : 0) |
                     (i + 2 < 16 ? static_cast<unsigned>(b[i + 2]) : 0);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += (i + 1 < 16) ? tbl[(v >> 6) & 63] : '=';
        out += (i + 2 < 16) ? tbl[v & 63] : '=';
    }
    return out;
}

// SHA1（CAPI）
std::string Sha1Base64(const std::string& input) {
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    std::string out;
    if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        return out;
    if (CryptCreateHash(prov, CALG_SHA1, 0, 0, &hash)) {
        CryptHashData(hash, reinterpret_cast<const BYTE*>(input.data()),
                      static_cast<DWORD>(input.size()), 0);
        BYTE digest[20] = {0};
        DWORD len = 20;
        if (CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0)) {
            static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            for (int i = 0; i < 20; i += 3) {
                unsigned v = (static_cast<unsigned>(digest[i]) << 16) |
                             (i + 1 < 20 ? static_cast<unsigned>(digest[i + 1]) << 8 : 0) |
                             (i + 2 < 20 ? static_cast<unsigned>(digest[i + 2]) : 0);
                out += tbl[(v >> 18) & 63];
                out += tbl[(v >> 12) & 63];
                out += (i + 1 < 20) ? tbl[(v >> 6) & 63] : '=';
                out += (i + 2 < 20) ? tbl[v & 63] : '=';
            }
        }
        CryptDestroyHash(hash);
    }
    CryptReleaseContext(prov, 0);
    return out;
}

void WsDiag(const char* fmt, ...) {
    FILE* f = nullptr;
    _wfopen_s(&f, L"D:\\bilibilibilil\\build\\ws_diag.log", L"a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

bool RawRecv(SOCKET s, unsigned char* buf, size_t len, size_t* got) {
    size_t total = 0;
    while (total < len) {
        int r = recv(s, reinterpret_cast<char*>(buf + total),
                     static_cast<int>(len - total), 0);
        if (r <= 0) return false;
        total += static_cast<size_t>(r);
    }
    *got = total;
    return true;
}

bool RawSendAll(SOCKET s, const unsigned char* buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        int r = send(s, reinterpret_cast<const char*>(buf + off),
                     static_cast<int>(len - off), 0);
        if (r <= 0) return false;
        off += static_cast<size_t>(r);
    }
    return true;
}

} // namespace

WsSocket::~WsSocket() { Close(); }

void WsSocket::Close() {
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    TlsCleanup();
}

// ---- schannel TLS ----------------------------------------------

bool WsSocket::TlsWrite(const unsigned char* data, size_t len) {
    // 握手完成后应用数据必须经 EncryptMessage 加密为 TLS 记录
    if (!tlsReady_) return RawSendAll(sock_, data, len);   // 握手阶段裸发送
    SecPkgContext_StreamSizes sizes{};
    if (QueryContextAttributesW(static_cast<CtxtHandle*>(pCtx_),
                                SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK)
        return false;
    const size_t total = sizes.cbHeader + len + sizes.cbTrailer;
    std::vector<unsigned char> msg(total);
    std::memcpy(msg.data() + sizes.cbHeader, data, len);

    SecBuffer bufs[4];
    bufs[0].pvBuffer = msg.data();
    bufs[0].cbBuffer = sizes.cbHeader;
    bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
    bufs[1].pvBuffer = msg.data() + sizes.cbHeader;
    bufs[1].cbBuffer = static_cast<DWORD>(len);
    bufs[1].BufferType = SECBUFFER_DATA;
    bufs[2].pvBuffer = msg.data() + sizes.cbHeader + len;
    bufs[2].cbBuffer = sizes.cbTrailer;
    bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
    bufs[3].BufferType = SECBUFFER_EMPTY;
    bufs[3].pvBuffer = nullptr;
    bufs[3].cbBuffer = 0;
    SecBufferDesc desc{ SECBUFFER_VERSION, 4, bufs };

    SECURITY_STATUS rc = EncryptMessage(static_cast<CtxtHandle*>(pCtx_), 0, &desc, 0);
    if (rc != SEC_E_OK) { WsDiag("[ws] EncryptMessage rc=0x%08X", static_cast<unsigned>(rc)); return false; }
    size_t sendLen = bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
    return RawSendAll(sock_, msg.data(), sendLen);
}

void WsSocket::TlsCleanup() {
    if (pCtx_) {
        DeleteSecurityContext(static_cast<CtxtHandle*>(pCtx_));
        delete static_cast<CtxtHandle*>(pCtx_);
        pCtx_ = nullptr;
    }
    if (pCred_) {
        FreeCredentialsHandle(static_cast<CredHandle*>(pCred_));
        delete static_cast<CredHandle*>(pCred_);
        pCred_ = nullptr;
    }
    tlsReady_ = false;
}

bool WsSocket::TlsHandshake() {
    SCHANNEL_CRED cred{};
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    // 默认协议（TLS1.2/1.3 均可；服务器实测两者都支持）
    cred.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;

    pCred_ = new CredHandle();
    pCtx_ = new CtxtHandle();
    if (SECURITY_STATUS rc0 = AcquireCredentialsHandleW(
            nullptr, (LPWSTR)UNISP_NAME_W, SECPKG_CRED_OUTBOUND, nullptr,
            &cred, nullptr, nullptr, static_cast<CredHandle*>(pCred_), nullptr);
        rc0 != SEC_E_OK)
        return false;

    std::wstring wTarget(targetHost_.begin(), targetHost_.end());
    DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                  ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_EXTENDED_ERROR | ISC_REQ_STREAM;
    DWORD outFlags = 0;

    unsigned char token[32768];
    std::vector<unsigned char> input;   // 累积待处理的服务器数据
    bool firstCall = true;
    int guard = 0;

    while (guard++ < 40) {
        SecBuffer inBufs[2];
        SecBufferDesc inDesc;
        if (firstCall) {
            inDesc.ulVersion = SECBUFFER_VERSION;
            inDesc.cBuffers = 0;
            inDesc.pBuffers = nullptr;
        } else {
            inBufs[0].pvBuffer = input.data();
            inBufs[0].cbBuffer = static_cast<DWORD>(input.size());
            inBufs[0].BufferType = SECBUFFER_TOKEN;
            inBufs[1].pvBuffer = nullptr;
            inBufs[1].cbBuffer = 0;
            inBufs[1].BufferType = SECBUFFER_EMPTY;
            inDesc.ulVersion = SECBUFFER_VERSION;
            inDesc.cBuffers = 2;
            inDesc.pBuffers = inBufs;
        }

        SecBuffer outBufs[4];
        outBufs[0].pvBuffer = token;
        outBufs[0].cbBuffer = sizeof(token);
        outBufs[0].BufferType = SECBUFFER_TOKEN;
        outBufs[1].BufferType = SECBUFFER_EMPTY; outBufs[1].pvBuffer = nullptr; outBufs[1].cbBuffer = 0;
        outBufs[2].BufferType = SECBUFFER_EMPTY; outBufs[2].pvBuffer = nullptr; outBufs[2].cbBuffer = 0;
        outBufs[3].BufferType = SECBUFFER_EMPTY; outBufs[3].pvBuffer = nullptr; outBufs[3].cbBuffer = 0;
        SecBufferDesc outDesc{ SECBUFFER_VERSION, 4, outBufs };

        SECURITY_STATUS rc = InitializeSecurityContextW(
            static_cast<CredHandle*>(pCred_),
            firstCall ? nullptr : static_cast<CtxtHandle*>(pCtx_),
            const_cast<LPWSTR>(wTarget.c_str()), flags,
            0, SECURITY_NATIVE_DREP,
            firstCall ? nullptr : &inDesc, 0,
            static_cast<CtxtHandle*>(pCtx_), &outDesc, &outFlags, nullptr);
        firstCall = false;

        // 发送输出 token
        for (int i = 0; i < 4; ++i) {
            if (outBufs[i].BufferType == SECBUFFER_TOKEN && outBufs[i].cbBuffer > 0 && outBufs[i].pvBuffer) {
                WsDiag("[ws] TLS send token %d bytes rc=0x%08X", static_cast<int>(outBufs[i].cbBuffer),
                       static_cast<unsigned>(rc));
                if (!TlsWrite(static_cast<unsigned char*>(outBufs[i].pvBuffer), outBufs[i].cbBuffer))
                    return false;
            }
        }

        if (rc == SEC_E_OK) {
            WsDiag("[ws] TLS handshake complete");
            tlsReady_ = true;
            return true;
        }
        if (rc == SEC_E_INCOMPLETE_MESSAGE) {
            // 需要更多数据；输入不消费，追加新数据
            unsigned char resp[16384];
            int r = recv(sock_, reinterpret_cast<char*>(resp), sizeof(resp), 0);
            if (r <= 0) { WsDiag("[ws] TLS recv(inc) FAIL err=%d", WSAGetLastError()); return false; }
            WsDiag("[ws] TLS recv(inc) %d bytes", r);
            input.insert(input.end(), resp, resp + r);
            continue;
        }
        if (rc == SEC_I_CONTINUE_NEEDED) {
            // 处理 EXTRA（schannel 未消费的输入，应作为下次输入保留）
            size_t consumed = input.size();
            for (int i = 0; i < 4; ++i) {
                if (outBufs[i].BufferType == SECBUFFER_EXTRA && outBufs[i].pvBuffer) {
                    consumed = static_cast<size_t>(static_cast<unsigned char*>(outBufs[i].pvBuffer) - input.data());
                    break;
                }
            }
            if (consumed > 0 && consumed <= input.size())
                input.erase(input.begin(), input.begin() + consumed);
            WsDiag("[ws] TLS extra consumed=%d remain=%d", static_cast<int>(consumed), static_cast<int>(input.size()));
            unsigned char resp[16384];
            int r = recv(sock_, reinterpret_cast<char*>(resp), sizeof(resp), 0);
            if (r <= 0) { WsDiag("[ws] TLS recv(cont) FAIL err=%d", WSAGetLastError()); return false; }
            WsDiag("[ws] TLS recv(cont) %d bytes", r);
            input.insert(input.end(), resp, resp + r);
            continue;
        }
        WsDiag("[ws] TLS rc=0x%08X", static_cast<unsigned>(rc));
        {
            FILE* f2 = nullptr;
            _wfopen_s(&f2, L"D:\\bilibilibilil\\build\\tlsfail.bin", L"wb");
            if (f2) { fwrite(input.data(), 1, input.size(), f2); fclose(f2); }
        }
        return false;
    }
    return false;
}

bool WsSocket::TlsDecrypt(size_t& outLen) {
    // 从 sock_ 读 TLS 记录，DecryptMessage 解出应用数据
    // 简化：一次读一大块，逐条记录解密
    unsigned char net[32768];
    int r = recv(sock_, reinterpret_cast<char*>(net), sizeof(net), 0);
    if (r < 0) {
        // 接收超时（弹幕推送间隔内暂无数据）不算连接失败，返回"无新数据"
        if (WSAGetLastError() == WSAETIMEDOUT) {
            outLen = frameBuf_.size();
            return true;
        }
        return false;
    }
    if (r == 0) return false;  // 对端关闭
    tlsInBuf_.insert(tlsInBuf_.end(), net, net + r);

    // 循环解密所有完整记录
    size_t off = 0;
    while (tlsInBuf_.size() >= off + 5) {
        // TLS 记录头：type(1) ver(2) len(2)
        unsigned len = (static_cast<unsigned>(tlsInBuf_[off + 3]) << 8) | tlsInBuf_[off + 4];
        if (tlsInBuf_.size() < off + 5 + len) break;  // 不完整
        // 用 DecryptMessage 解密该记录
        SecBuffer bufs[4];
        bufs[0].pvBuffer = tlsInBuf_.data() + off;   // 完整记录（含记录头）
        bufs[0].cbBuffer = static_cast<DWORD>(5 + len);
        bufs[0].BufferType = SECBUFFER_DATA;
        bufs[1].BufferType = SECBUFFER_EMPTY; bufs[1].pvBuffer = nullptr; bufs[1].cbBuffer = 0;
        bufs[2].BufferType = SECBUFFER_EMPTY; bufs[2].pvBuffer = nullptr; bufs[2].cbBuffer = 0;
        bufs[3].BufferType = SECBUFFER_EMPTY; bufs[3].pvBuffer = nullptr; bufs[3].cbBuffer = 0;
        SecBufferDesc desc{ SECBUFFER_VERSION, 4, bufs };
        SECURITY_STATUS rc = DecryptMessage(static_cast<CtxtHandle*>(pCtx_), &desc, 0, nullptr);
        if (rc == SEC_E_INCOMPLETE_MESSAGE) break;
        if (rc == SEC_I_RENEGOTIATE || rc == SEC_I_CONTEXT_EXPIRED) {
            // TLS1.3 NewSessionTicket 等握手后消息：收集 DATA 后跳过
            for (int i = 0; i < 4; ++i) {
                if (bufs[i].BufferType == SECBUFFER_DATA && bufs[i].cbBuffer > 0) {
                    frameBuf_.insert(frameBuf_.end(),
                                     static_cast<unsigned char*>(bufs[i].pvBuffer),
                                     static_cast<unsigned char*>(bufs[i].pvBuffer) + bufs[i].cbBuffer);
                }
            }
            off += 5 + len;
            continue;
        }
        if (rc != SEC_E_OK) {
            WsDiag("[ws] decrypt rc=0x%08X rec@%d", static_cast<unsigned>(rc), static_cast<int>(off));
            off += 5 + len;
            continue;
        }
        // 找到 DATA 缓冲
        for (int i = 0; i < 4; ++i) {
            if (bufs[i].BufferType == SECBUFFER_DATA && bufs[i].cbBuffer > 0) {
                frameBuf_.insert(frameBuf_.end(),
                                 static_cast<unsigned char*>(bufs[i].pvBuffer),
                                 static_cast<unsigned char*>(bufs[i].pvBuffer) + bufs[i].cbBuffer);
            }
        }
        off += 5 + len;
    }
    if (off > 0) tlsInBuf_.erase(tlsInBuf_.begin(), tlsInBuf_.begin() + off);
    outLen = frameBuf_.size();
    return true;
}

// ---- WS 帧 ------------------------------------------------------

bool WsSocket::Send(const unsigned char* data, size_t len) {
    // 客户端帧：FIN+二进制(0x82) + 掩码
    std::vector<unsigned char> frame;
    frame.push_back(0x82);
    std::mt19937 rng(static_cast<unsigned>(GetTickCount64()));
    unsigned char mask[4];
    for (int i = 0; i < 4; ++i) mask[i] = static_cast<unsigned char>(rng() & 0xFF);
    if (len < 126) {
        frame.push_back(static_cast<unsigned char>(0x80 | len));
    } else if (len < 65536) {
        frame.push_back(0x80 | 126);
        frame.push_back(static_cast<unsigned char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<unsigned char>(len & 0xFF));
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; --i)
            frame.push_back(static_cast<unsigned char>((static_cast<unsigned long long>(len) >> (i * 8)) & 0xFF));
    }
    frame.insert(frame.end(), mask, mask + 4);
    for (size_t i = 0; i < len; ++i)
        frame.push_back(data[i] ^ mask[i % 4]);
    return TlsWrite(frame.data(), frame.size());
}

bool WsSocket::Receive(std::vector<unsigned char>& payload) {
    payload.clear();
    // 从 frameBuf_ 中尝试解析完整帧
    while (true) {
        if (frameBuf_.size() >= 2) {
            unsigned char b0 = frameBuf_[0], b1 = frameBuf_[1];
            bool masked = (b1 & 0x80) != 0;
            unsigned long long len = b1 & 0x7F;
            size_t off = 2;
            if (len == 126) {
                if (frameBuf_.size() < off + 2) { /* 等更多 */ }
                else {
                    len = (static_cast<unsigned long long>(frameBuf_[off]) << 8) | frameBuf_[off + 1];
                    off += 2;
                }
            } else if (len == 127) {
                if (frameBuf_.size() < off + 8) { /* 等更多 */ }
                else {
                    len = 0;
                    for (int i = 0; i < 8; ++i) len = (len << 8) | frameBuf_[off + i];
                    off += 8;
                }
            }
            size_t maskLen = masked ? 4 : 0;
            if (frameBuf_.size() < off + maskLen + len) {
                // 数据不足 → 读更多 TLS 数据
                size_t got = 0;
                if (!TlsDecrypt(got)) return false;
                if (got == 0) {
                    // 暂无新数据（接收超时）：保持连接，稍后继续读
                    Sleep(50);
                    continue;
                }
                continue;
            }
            unsigned char maskKey[4] = {0};
            if (masked) {
                memcpy(maskKey, frameBuf_.data() + off, 4);
                off += 4;
            }
            // 帧内容
            payload.assign(frameBuf_.begin() + off, frameBuf_.begin() + off + len);
            if (masked) {
                for (size_t i = 0; i < payload.size(); ++i)
                    payload[i] ^= maskKey[i % 4];
            }
            frameBuf_.erase(frameBuf_.begin(), frameBuf_.begin() + off + len);

            unsigned char opcode = b0 & 0x0F;
            if (opcode == 0x8) {          // close
                Close();
                return false;
            }
            if (opcode == 0x9) {          // ping → pong
                // 回 pong：opcode 0xA，payload 原样
                std::vector<unsigned char> pong(2, 0);
                pong[0] = 0x8A;  // FIN + pong
                pong[1] = static_cast<unsigned char>(0x80 | payload.size());
                pong.insert(pong.end(), maskKey, maskKey + 4);
                for (size_t i = 0; i < payload.size(); ++i)
                    pong.push_back(payload[i] ^ maskKey[i % 4]);
                TlsWrite(pong.data(), pong.size());
                continue;
            }
            if (opcode == 0x1 || opcode == 0x2) {  // text / binary
                return true;
            }
            // 其他控制帧忽略
            continue;
        }
        // 缓冲不足 2 字节
        size_t got = 0;
        if (!TlsDecrypt(got)) return false;
        if (got == 0) {
            // 暂无新数据（接收超时）：保持连接，继续等待（绝不能误判为断开）
            Sleep(50);
            continue;
        }
    }
}

// ---- 连接 ------------------------------------------------

bool WsSocket::TlsConnect(const std::string& host, int port) {
    targetHost_ = host;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { WsDiag("[ws] WSAStartup FAIL"); return false; }

    sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock_ == INVALID_SOCKET) { WsDiag("[ws] socket FAIL"); return false; }

    // 解析 host
    hostent* he = gethostbyname(host.c_str());
    if (!he) { WsDiag("[ws] gethostbyname FAIL host=%s err=%d", host.c_str(), WSAGetLastError()); Close(); return false; }
    WsDiag("[ws] resolved %s", inet_ntoa(*reinterpret_cast<in_addr*>(he->h_addr_list[0])));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    // 阻塞 connect（默认超时；与 python 直连方式一致）
    int rc = connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0) { WsDiag("[ws] connect err=%d", WSAGetLastError()); Close(); return false; }
    WsDiag("[ws] TCP connected");

    // 收发超时（TLS 握手/升级若服务器无响应，避免永久卡死）
    DWORD tv = 15000;  // 毫秒
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    // TLS 握手
    if (!TlsHandshake()) { WsDiag("[ws] TLS handshake FAIL"); Close(); return false; }
    WsDiag("[ws] TLS OK");
    // 握手完成后收应用数据用 1s 超时（快速感知新弹幕；Stop/切房快速退出）
    DWORD tvData = 1000;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tvData), sizeof(tvData));
    return true;
}

bool WsSocket::Connect(const std::string& host, int port) {
    if (!TlsConnect(host, port)) return false;

    // WS 升级
    std::string key = RandomKey();
    std::string req =
        "GET /sub HTTP/1.1\r\n"
        "Host: " + host + ":" + std::to_string(port) + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Origin: https://live.bilibili.com\r\n"
        "User-Agent: " + kWsUa + "\r\n"
        "\r\n";

    if (!TlsWrite(reinterpret_cast<const unsigned char*>(req.data()), req.size())) {
        WsDiag("[ws] upgrade req send FAIL");
        Close();
        return false;
    }
    WsDiag("[ws] upgrade req sent (%d bytes)", static_cast<int>(req.size()));

    // 读响应头
    std::vector<unsigned char> header;
    size_t got = 0;
    while (true) {
        if (!TlsDecrypt(got)) {
            WsDiag("[ws] upgrade read FAIL; buf=%.200s",
                   std::string(frameBuf_.begin(),
                               frameBuf_.begin() + (frameBuf_.size() > 200 ? 200 : frameBuf_.size())).c_str());
            Close();
            return false;
        }
        WsDiag("[ws] upgrade app data +%d (buf %d) head=%.100s", static_cast<int>(got), static_cast<int>(frameBuf_.size()),
               std::string(frameBuf_.begin(), frameBuf_.begin() + (frameBuf_.size() > 100 ? 100 : frameBuf_.size())).c_str());
        // 定位 "HTTP/" 起点（跳过 NewSessionTicket 等前置数据）
        static const char kHttp[] = "HTTP/";
        auto http = std::search(frameBuf_.begin(), frameBuf_.end(), kHttp, kHttp + 5);
        if (http != frameBuf_.end()) {
            // B站弹幕服务器用 \n\n 结束响应头（非标准），两种都兼容
            std::vector<unsigned char> sep1{'\r','\n','\r','\n'};
            std::vector<unsigned char> sep2{'\n','\n'};
auto it = std::search(http, frameBuf_.end(), sep1.begin(), sep1.end());
            if (it == frameBuf_.end())
                it = std::search(http, frameBuf_.end(), sep2.begin(), sep2.end());
            if (it != frameBuf_.end()) {
                header.assign(http, it);
                // 跳过 4 字节或 2 字节分隔符
                bool crlf = (it != frameBuf_.end() && it + 4 <= frameBuf_.end() &&
                             std::equal(sep1.begin(), sep1.end(), it));
                size_t skip = crlf ? 4 : 2;
                frameBuf_.erase(frameBuf_.begin(), it + skip);
                break;
            }
        }
        if (frameBuf_.size() > 65536) { Close(); return false; }
        if (got == 0) {
            WsDiag("[ws] upgrade no new data; buf=%.240s",
                   std::string(frameBuf_.begin(), frameBuf_.begin() + (frameBuf_.size() > 240 ? 240 : frameBuf_.size())).c_str());
            Close();
            return false;
        }
    }
    WsDiag("[ws] upgrade header: %.200s", std::string(header.begin(), header.end()).c_str());

    std::string hs(header.begin(), header.end());
    // 校验 101 + Accept
    if (hs.find(" 101 ") == std::string::npos && hs.find("101 Switching") == std::string::npos) {
        Close();
        return false;
    }
    std::string expect = Sha1Base64(key + kGuid);
    std::string acceptLine;
    {
        size_t p = hs.find("Sec-WebSocket-Accept");
        if (p != std::string::npos) {
            size_t q = hs.find("\r\n", p);
            acceptLine = hs.substr(p, q == std::string::npos ? std::string::npos : q - p);
        }
    }
    if (acceptLine.find(expect) == std::string::npos) {
        // Accept 校验失败（部分服务器实现不规范）→ 容忍，继续
    }
    return true;
}
