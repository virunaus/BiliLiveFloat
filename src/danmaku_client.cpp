// ============================================================
// danmaku_client.cpp —— B站直播弹幕客户端实现
// 协议细节（包格式/认证/心跳/解压）已于 2026-10-05 实测验证。
// ============================================================

// winsock2 必须先于 windows.h（ws_socket.h 包含两者）
#include "ws_socket.h"
#include "danmaku_client.h"
#include "bili_api.h"  // DanmakuConf

#include <winhttp.h>
#include <brotli/decode.h>
#include <zlib.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <cstdio>
#include <vector>

#pragma comment(lib, "winhttp.lib")

using nlohmann::json;


// ---- 工具 -------------------------------------------------
// UTF-8 字节串 → 宽字符（弹幕 JSON 均为 UTF-8）
static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

namespace {

// 弹幕包：16 字节大端头
#pragma pack(push, 1)
struct PacketHeader {
    unsigned int   packLen;     // [0..3]  包总长度（含头）
    unsigned short headerLen;   // [4..5]  头长度，固定 16
    unsigned short protover;    // [6..7]  0=明文 2=zlib 3=brotli
    unsigned int   op;          // [8..11] 2=心跳 3=回包 5=推送 7=认证 8=认证回包
    unsigned int   seq;         // [12..15] 序号，固定 1
};
#pragma pack(pop)

// 构造一个协议包（网络字节序）
std::vector<unsigned char> MakePacket(const char* body, size_t bodyLen,
                                      unsigned short protover, unsigned int op) {
    std::vector<unsigned char> pkt(16 + bodyLen);
    PacketHeader h;
    h.packLen   = static_cast<unsigned int>(16 + bodyLen);
    h.headerLen = 16;
    h.protover  = protover;
    h.op        = op;
    h.seq       = 1;
    // 手动转大端
    pkt[0] = static_cast<unsigned char>(h.packLen >> 24);
    pkt[1] = static_cast<unsigned char>(h.packLen >> 16);
    pkt[2] = static_cast<unsigned char>(h.packLen >> 8);
    pkt[3] = static_cast<unsigned char>(h.packLen);
    pkt[4] = static_cast<unsigned char>(h.headerLen >> 8);
    pkt[5] = static_cast<unsigned char>(h.headerLen);
    pkt[6] = static_cast<unsigned char>(h.protover >> 8);
    pkt[7] = static_cast<unsigned char>(h.protover);
    pkt[8] = static_cast<unsigned char>(h.op >> 24);
    pkt[9] = static_cast<unsigned char>(h.op >> 16);
    pkt[10] = static_cast<unsigned char>(h.op >> 8);
    pkt[11] = static_cast<unsigned char>(h.op);
    pkt[12] = static_cast<unsigned char>(h.seq >> 24);
    pkt[13] = static_cast<unsigned char>(h.seq >> 16);
    pkt[14] = static_cast<unsigned char>(h.seq >> 8);
    pkt[15] = static_cast<unsigned char>(h.seq);
    if (bodyLen > 0) std::memcpy(pkt.data() + 16, body, bodyLen);
    return pkt;
}

// 诊断日志（写 D:\bilibilibilil\build\danmaku_diag.log）
#include <cstdio>
void DanmakuDiag(const char* fmt, ...) {
    FILE* f = nullptr;
    _wfopen_s(&f, L"D:\\bilibilibilil\\build\\danmaku_diag.log", L"a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// 解析包头

bool ParseHeader(const unsigned char* d, size_t len, PacketHeader& h) {
    if (len < 16) return false;
    h.packLen   = (static_cast<unsigned int>(d[0]) << 24) | (static_cast<unsigned int>(d[1]) << 16) |
                  (static_cast<unsigned int>(d[2]) << 8) | static_cast<unsigned int>(d[3]);
    h.headerLen = static_cast<unsigned short>((d[4] << 8) | d[5]);
    h.protover  = static_cast<unsigned short>((d[6] << 8) | d[7]);
    h.op        = (static_cast<unsigned int>(d[8]) << 24) | (static_cast<unsigned int>(d[9]) << 16) |
                  (static_cast<unsigned int>(d[10]) << 8) | static_cast<unsigned int>(d[11]);
    h.seq       = (static_cast<unsigned int>(d[12]) << 24) | (static_cast<unsigned int>(d[13]) << 16) |
                  (static_cast<unsigned int>(d[14]) << 8) | static_cast<unsigned int>(d[15]);
    return h.packLen >= 16 && h.headerLen == 16;
}

// brotli 解压（单次解压 API：输出缓冲不足时增大重试）
bool BrotliDecompress(const unsigned char* src, size_t srcLen,
                      std::vector<unsigned char>& out) {
    if (srcLen == 0) return false;
    size_t outLen = srcLen * 4 + 1024;  // 弹幕文本的膨胀率一般小于 4
    for (int attempt = 0; attempt < 8; ++attempt) {
        out.resize(outLen);
        size_t actual = outLen;
        BrotliDecoderResult r = BrotliDecoderDecompress(srcLen, src, &actual, out.data());
        if (r == BROTLI_DECODER_RESULT_SUCCESS) {
            out.resize(actual);
            return true;
        }
        if (r == BROTLI_DECODER_RESULT_ERROR) return false;  // 输入损坏
        // NEEDS_MORE_OUTPUT → 扩大缓冲重试
        outLen *= 4;
    }
    return false;
}

// zlib 解压
bool ZlibDecompress(const unsigned char* src, size_t srcLen,
                    std::vector<unsigned char>& out) {
    uLongf outLen = 0;
    if (uncompress(nullptr, &outLen, src, static_cast<uLong>(srcLen)) != Z_OK) {
        // 第一次调用只用来探测长度会失败（buf 为空），用膨胀的保守方式
        outLen = static_cast<uLongf>(srcLen) * 8 + 1024;
    }
    out.resize(outLen);
    uLongf actual = outLen;
    int ret = uncompress(out.data(), &actual, src, static_cast<uLong>(srcLen));
    if (ret != Z_OK) return false;
    out.resize(actual);
    return true;
}

// 把 JSON 字符串里的转义还原（unicode \uXXXX 等由 json 库处理，这里只还原常见转义）
std::string UnescapeJson(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[++i];
            switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                case '/': out += '/'; break;
                default: out += c; break;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

} // namespace

// ---- 公共接口 -------------------------------------------------

DanmakuClient::DanmakuClient() = default;
DanmakuClient::~DanmakuClient() { Stop(); }

void DanmakuClient::NotifyStatus(DanmakuStatus st) {
    StatusCallback cb = nullptr;
    void* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lk(cbMutex_);
        cb = stCb_; ctx = ctx_;
    }
    if (cb) cb(st, ctx);
}

void DanmakuClient::Start(long long realRoomId, const bili::DanmakuConf& conf,
                          const std::string& buvid3) {
    Stop();
    roomId_ = realRoomId;
    token_ = conf.token;
    buvid3_ = buvid3;
    servers_ = conf.servers;
    if (servers_.empty()) return;
    stopRequested_ = false;
    running_ = true;
    thread_ = std::thread(&DanmakuClient::ThreadMain, this);
}

void DanmakuClient::Stop() {
    stopRequested_ = true;
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

// ---- 线程主体（断线自动重连：指数退避 1s→2s→…→30s 封顶）----

void DanmakuClient::ThreadMain() {
    int serverIdx = 0;
    int retry = 0;
    while (!stopRequested_.load()) {
        const auto& srv = servers_[static_cast<size_t>(serverIdx) % servers_.size()];
        ConnectOnce(srv.host, srv.wss_port);
        if (stopRequested_.load()) break;
        // 指数退避
        int delaySec = 1;
        for (int i = 0; i < retry && delaySec < 30; ++i) delaySec *= 2;
        if (delaySec > 30) delaySec = 30;
        NotifyStatus(DanmakuStatus::Reconnecting);
        for (int i = 0; i < delaySec * 2 && !stopRequested_.load(); ++i)
            Sleep(500);
        retry++;
        serverIdx++;
    }
    NotifyStatus(DanmakuStatus::Stopped);
}

// ---- 单次连接：TLS+WS 升级 → 认证 → 心跳 → 接收解析 -----------------

void DanmakuClient::ConnectOnce(const std::string& host, int wssPort) {
    WsSocket ws;
    if (!ws.Connect(host, wssPort)) {
        DanmakuDiag("[ws] Connect FAIL host=%s port=%d", host.c_str(), wssPort);
        return;
    }

    // ---- 认证包（op=7，header protover=1，body JSON 含 buvid）----
    json auth;
    auth["uid"] = 0;
    auth["roomid"] = roomId_;
    auth["protover"] = 3;          // 请求 brotli 推送
    auth["platform"] = "web";
    auth["type"] = 2;
    auth["buvid"] = buvid3_;
    if (!token_.empty()) auth["key"] = token_;
    std::string authBody = auth.dump();
    auto authPkt = MakePacket(authBody.data(), authBody.size(), 1, 7);

    if (!ws.Send(authPkt.data(), authPkt.size())) {
        DanmakuDiag("[ws] auth send FAIL");
        return;
    }

    // ---- 接收循环 ----
    bool authed = false;
    ULONGLONG lastHeartbeat = GetTickCount64();
    ULONGLONG lastRecv = GetTickCount64();   // 最近一次收到数据的时间
    std::vector<unsigned char> frame;

    while (!stopRequested_.load()) {
        // 45s 没有任何数据：服务器可能已静默断开（TCP 未通知）→ 主动重连
        if (GetTickCount64() - lastRecv >= 45000) {
            DanmakuDiag("[ws] no data 45s, force reconnect");
            break;
        }
        // 每 30 秒心跳（op=2）
        if (GetTickCount64() - lastHeartbeat >= 30000) {
            auto hb = MakePacket("[object Object]", 16, 1, 2);
            if (!ws.Send(hb.data(), hb.size())) {
                DanmakuDiag("[ws] heartbeat send FAIL");
                break;  // 连接已死，立即重连
            }
            lastHeartbeat = GetTickCount64();
        }

        // 读取一帧（二进制消息，可能包含多个子包）
        if (!ws.Receive(frame)) {
            DanmakuDiag("[ws] receive FAIL/closed");
            break;  // 连接断开 → 外层重连
        }
        if (frame.empty()) continue;
        lastRecv = GetTickCount64();  // 收到数据，刷新存活时间

        // 处理一帧（可能含多个子包）
        size_t off = 0;
        const unsigned char* data = frame.data();
        size_t frameLen = frame.size();
        while (off + 16 <= frameLen) {
            PacketHeader h;
            if (!ParseHeader(data + off, frameLen - off, h)) break;
            if (h.packLen < 16 || off + h.packLen > frameLen) break;
            const unsigned char* body = data + off + h.headerLen;
            size_t bodyLen = h.packLen - h.headerLen;

            if (h.op == 8) {
                // 认证回包
                std::string s(body, body + bodyLen);
                DanmakuDiag("[ws] AUTH_REPLY op=8 body=%.120s", s.c_str());
                try {
                    auto j = json::parse(s);
                    if (j.value("code", -1) == 0) {
                        authed = true;
                        NotifyStatus(DanmakuStatus::Connected);
                        // 认证成功立即发一次心跳
                        auto hb = MakePacket("[object Object]", 16, 1, 2);
                        ws.Send(hb.data(), hb.size());
                        lastHeartbeat = GetTickCount64();
                    } else {
                        NotifyStatus(DanmakuStatus::AuthFailed);
                    }
                } catch (...) { /* 忽略解析失败 */ }
            } else if (h.op == 3) {
                // 心跳回包（前 4 字节人气值），无需处理
            } else if (h.op == 5) {
                // 业务推送：先解压，再递归拆包
                std::vector<unsigned char> decompressed;
                if (h.protover == 3) {
                    if (!BrotliDecompress(body, bodyLen, decompressed)) { off += h.packLen; continue; }
                } else if (h.protover == 2) {
                    if (!ZlibDecompress(body, bodyLen, decompressed)) { off += h.packLen; continue; }
                } else {
                    decompressed.assign(body, body + bodyLen);
                }
                // 解压后的数据可能含多个子包
                size_t innerOff = 0;
                while (innerOff + 16 <= decompressed.size()) {
                    PacketHeader ih;
                    if (!ParseHeader(decompressed.data() + innerOff, decompressed.size() - innerOff, ih))
                        break;
                    if (ih.packLen < 16 || innerOff + ih.packLen > decompressed.size()) break;
                    const unsigned char* ibody = decompressed.data() + innerOff + ih.headerLen;
                    size_t ibodyLen = ih.packLen - ih.headerLen;
                    innerOff += ih.packLen;
                    if (ih.op != 5 || ibodyLen == 0) continue;

                    // 单条业务消息 JSON
                    try {
                        auto m = json::parse(std::string(ibody, ibody + ibodyLen));
                        std::string cmd = m.value("cmd", "");
                        if (cmd == "DANMU_MSG") {
                            auto& info = m["info"];
                            DanmakuMessage msg;
                            // info[1] = 弹幕文本
                            std::string text = info.at(1).get<std::string>();
                            // info[2][1] = 用户名
                            std::string user = info.at(2).at(1).get<std::string>();
                            // info[0][4] = uid
                            long long uid = info.at(0).at(4).get<long long>();
                            // 弹幕文本/用户名为 UTF-8，必须经 MultiByteToWideChar 转宽字符，
                            // 直接逐字节拷贝（std::wstring(iter,iter)）会乱码
                            msg.text = Utf8ToWide(text);
                            msg.user = Utf8ToWide(user);
                            msg.uid = uid;
                            msg.ts = GetTickCount64();

                            MsgCallback cb = nullptr;
                            void* ctx = nullptr;
                            {
                                std::lock_guard<std::mutex> lk(cbMutex_);
                                cb = msgCb_; ctx = ctx_;
                            }
                            if (cb) cb(msg, ctx);
                        }
                    } catch (...) { /* 非 JSON 或字段缺失，忽略 */ }
                }
            }
            off += h.packLen;
        }
    }

    ws.Close();
    (void)authed;
}
