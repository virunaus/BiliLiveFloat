// ============================================================
// bili_api.cpp —— B站直播网络接口实现
// 所有接口路径/参数均于 2026-10-05 在本机实测核对。
// JSON 解析使用 nlohmann-json（third_party/json 单头文件）。
// ============================================================

#include "bili_api.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <ctime>
#include <regex>
#include <sstream>
#include <algorithm>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

using nlohmann::json;

namespace {

const wchar_t* kUA =
    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    L"(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

// wbi 密钥置换表（B站当前版本，32 项，2026-10-05 实测）
const int kWbiKeyTable[32] = {
    46,47,18,2,53,8,23,32,15,50,10,31,58,3,45,35,
    27,43,5,49,33,9,42,19,29,28,14,39,12,38,41,13
};

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

// 宽字符 → UTF-8（搜索关键词等）
std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string u(n ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &u[0], n, nullptr, nullptr);
    return u;
}

// 剥离搜索结果昵称中的 HTML 高亮标签（如 <em class="keyword">）
std::string StripHtml(const std::string& s) {
    std::string out;
    bool inTag = false;
    for (char ch : s) {
        if (ch == '<') { inTag = true; continue; }
        if (ch == '>') { inTag = false; continue; }
        if (!inTag) out += ch;
    }
    return out;
}

} // namespace

namespace bili {

// ---- 工具实现 -------------------------------------------------

std::string UrlEncode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string Md5Hex(const std::string& input) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0);
    BCRYPT_HASH_HANDLE hash = nullptr;
    BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
                   static_cast<ULONG>(input.size()), 0);
    unsigned char digest[16] = {0};
    BCryptFinishHash(hash, digest, sizeof(digest), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    static const char hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : digest) {
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return out;
}

std::string WbiSign(const std::string& imgKey, const std::string& subKey,
                    const std::vector<std::pair<std::string, std::string>>& params) {
    // 1) 附加 wts 时间戳并按键名排序
    std::vector<std::pair<std::string, std::string>> all = params;
    char ts[32];
    snprintf(ts, sizeof(ts), "%lld", static_cast<long long>(::time(nullptr)));
    all.emplace_back("wts", ts);
    std::sort(all.begin(), all.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    // 2) 过滤特殊字符 !'()*
    std::string strToSign;
    bool first = true;
    for (auto& kv : all) {
        std::string v;
        for (char c : kv.second) {
            if (c != '!' && c != '\'' && c != '(' && c != ')' && c != '*')
                v += c;
        }
        if (!first) strToSign += '&';
        strToSign += kv.first + '=' + v;
        first = false;
    }
    // 3) 计算 mixin key（img+sub 按置换表取 32 字符）
    std::string mixin = imgKey + subKey;
    std::string wbiKey;
    for (int idx : kWbiKeyTable) {
        if (idx < static_cast<int>(mixin.size()))
            wbiKey += mixin[static_cast<size_t>(idx)];
    }
    // 4) md5 签名
    std::string wRid = Md5Hex(strToSign + wbiKey);
    return strToSign + "&w_rid=" + wRid;
}

std::optional<std::string> HttpGet(const std::wstring& host,
                                   const std::wstring& path,
                                   const std::string& cookieHeader,
                                   const std::wstring& referer) {
    HINTERNET hSession = WinHttpOpen(L"BiliLiveFloat/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return std::nullopt;

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
                                        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return std::nullopt;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
                                            WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return std::nullopt;
    }

    std::wstring headers = L"Referer: " + referer + L"\r\n"
                           L"User-Agent: " + std::wstring(kUA) + L"\r\n"
                           L"Accept: */*\r\n"
                           L"Accept-Language: zh-CN,zh;q=0.9\r\n";
    if (!cookieHeader.empty()) {
        headers += L"Cookie: " + ToWide(cookieHeader) + L"\r\n";
    }

    std::optional<std::string> result;
    if (WinHttpSendRequest(hRequest, headers.c_str(), static_cast<DWORD>(-1),
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {
        std::string body;
        char buf[8192];
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
            DWORD read = 0;
            if (WinHttpReadData(hRequest, buf, std::min<DWORD>(avail, sizeof(buf)), &read)) {
                body.append(buf, read);
            } else {
                break;
            }
        }
        result = std::move(body);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

// ---- Session --------------------------------------------------

void Session::FetchBuvid() {
    // spi 接口：b_3 -> buvid3（加 .infoc 后缀），b_4 -> buvid4
    auto body = HttpGet(L"api.bilibili.com", L"/x/frontend/finger/spi", "", L"https://www.bilibili.com/");
    if (!body) return;
    try {
        auto j = json::parse(*body);
        auto& data = j["data"];
        std::string b3 = data.value("b_3", "");
        std::string b4 = data.value("b_4", "");
        if (b3.empty()) return;
        // spi 返回的 b_3 已带 ".infoc" 后缀，避免重复追加
        std::string suffix = ".infoc";
        if (b3.size() >= suffix.size() &&
            b3.compare(b3.size() - suffix.size(), suffix.size(), suffix) == 0)
            suffix.clear();
        cookie_header = "buvid3=" + b3 + suffix;
        if (!b4.empty()) cookie_header += "; buvid4=" + b4;
    } catch (...) {
        return;
    }
}

void Session::FetchWbiKeys() {
    auto body = HttpGet(L"api.bilibili.com", L"/x/web-interface/nav",
                        cookie_header, L"https://www.bilibili.com/");
    if (!body) return;
    try {
        auto j = json::parse(*body);
        auto& wbi = j["data"]["wbi_img"];
        std::string imgUrl = wbi.value("img_url", "");
        std::string subUrl = wbi.value("sub_url", "");
        auto keyFromUrl = [](const std::string& u) -> std::string {
            auto pos = u.find_last_of('/');
            std::string name = (pos == std::string::npos) ? u : u.substr(pos + 1);
            auto dot = name.find('.');
            return (dot == std::string::npos) ? name : name.substr(0, dot);
        };
        img_key = keyFromUrl(imgUrl);
        sub_key = keyFromUrl(subUrl);
        if (!img_key.empty() && !sub_key.empty()) ready = true;
    } catch (...) {
        return;
    }
}

void Session::EnsureReady() {
    if (cookie_header.empty()) FetchBuvid();
    if (!ready) FetchWbiKeys();
}

// ---- 业务接口 -------------------------------------------------

std::optional<long long> ParseRoomInput(const std::wstring& input) {
    std::wstring s = input;
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' ||
                          s.back() == L'\r' || s.back() == L'\n'))
        s.pop_back();
    size_t start = 0;
    while (start < s.size() && (s[start] == L' ' || s[start] == L'\t'))
        ++start;
    s = s.substr(start);
    if (s.empty()) return std::nullopt;

    // 完整 URL / 短链：live.bilibili.com/12345
    std::wregex reUrl(L"live\\.bilibili\\.com/(\\d+)");
    std::wsmatch m;
    if (std::regex_search(s, m, reUrl)) {
        return std::stoll(m[1].str());
    }
    // 纯数字
    if (std::regex_match(s, std::wregex(L"\\d+"))) {
        return std::stoll(s);
    }
    return std::nullopt;
}

std::optional<RoomInfo> GetRoomInfo(long long roomIdOrShort) {
    char path[256];
    snprintf(path, sizeof(path), "/room/v1/Room/get_info?room_id=%lld", roomIdOrShort);
    auto body = HttpGet(L"api.live.bilibili.com", ToWide(path), "", L"https://live.bilibili.com/");
    if (!body) return std::nullopt;
    try {
        auto j = json::parse(*body);
        if (j.value("code", -1) != 0) return std::nullopt;
        auto& d = j["data"];
        RoomInfo info;
        info.room_id = d.value("room_id", 0LL);
        info.live_status = d.value("live_status", 0);
        info.title = d.value("title", "");
        info.user_name = d.value("user_name", "");
        info.uid = d.value("uid", 0LL);
        if (info.room_id == 0) return std::nullopt;
        return info;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<DanmakuConf> GetDanmakuConf(long long realRoomId, Session& session) {
    session.EnsureReady();
    char idStr[32];
    snprintf(idStr, sizeof(idStr), "%lld", realRoomId);
    auto signedQuery = WbiSign(session.img_key, session.sub_key,
                               {{"id", idStr}, {"type", "0"}});
    std::string pathStr = "/xlive/web-room/v1/index/getDanmuInfo?" + signedQuery;
    auto body = HttpGet(L"api.live.bilibili.com", ToWide(pathStr), session.cookie_header,
                        L"https://live.bilibili.com/");
    if (!body) return std::nullopt;
    try {
        auto j = json::parse(*body);
        if (j.value("code", -1) != 0) return std::nullopt;
        auto& d = j["data"];
        DanmakuConf conf;
        conf.token = d.value("token", "");
        for (auto& h : d.at("host_list")) {
            DanmakuServer srv;
            srv.host = h.value("host", "");
            srv.port = h.value("port", 2243);
            srv.wss_port = h.value("wss_port", 443);
            if (!srv.host.empty()) conf.servers.push_back(std::move(srv));
        }
        if (conf.servers.empty() || conf.token.empty()) return std::nullopt;
        return conf;
    } catch (...) {
        return std::nullopt;
    }
}

// 从响应里挑选最合适的流：优先 exact==qn，其次 <=qn 的最大，最后任意最大
static std::optional<StreamUrl> PickStream(const json& j, int wantQn) {
    auto& streams = j["data"]["playurl_info"]["playurl"]["stream"];
    struct Cand { std::string url, format; int qn; };
    std::vector<Cand> cands;
    // 收集 flv/avc（http_stream）与 hls（http_hls）候选
    for (auto& st : streams) {
        std::string proto = st.value("protocol_name", "");
        bool isFlv = (proto == "http_stream");
        bool isHls = (proto == "http_hls");
        if (!isFlv && !isHls) continue;
        for (auto& fmt : st.at("format")) {
            std::string fmtName = fmt.value("format_name", "");
            for (auto& codec : fmt.at("codec")) {
                if (codec.value("codec_name", "") != "avc") continue;
                int qn = codec.value("current_qn", 0);
                auto& uris = codec.at("url_info");
                if (uris.empty()) continue;
                std::string full = uris[0].value("host", "") +
                                   codec.value("base_url", "") +
                                   uris[0].value("extra", "");
                cands.push_back({full, isFlv ? "flv" : fmtName, qn});
            }
        }
    }
    if (cands.empty()) return std::nullopt;
    // 1) 精确匹配
    for (auto& c : cands) if (c.qn == wantQn) return StreamUrl{c.url, c.format, c.qn};
    // 2) <= 目标的最大
    int bestQn = -1; Cand* best = nullptr;
    for (auto& c : cands)
        if (c.qn <= wantQn && c.qn > bestQn) { bestQn = c.qn; best = &c; }
    if (best) return StreamUrl{best->url, best->format, best->qn};
    // 3) 任意最大（全部高于目标 → 降级给最大）
    for (auto& c : cands)
        if (c.qn > bestQn) { bestQn = c.qn; best = &c; }
    if (best) return StreamUrl{best->url, best->format, best->qn};
    return std::nullopt;
}

std::optional<StreamUrl> GetStreamUrl(long long realRoomId, int qn) {
    char path[512];
    snprintf(path, sizeof(path),
             "/xlive/web-room/v2/index/getRoomPlayInfo?room_id=%lld"
             "&protocol=0,1&format=0,1,2&codec=0,1&qn=%d&platform=web",
             realRoomId, qn);
    auto body = HttpGet(L"api.live.bilibili.com", ToWide(path), "", L"https://live.bilibili.com/");
    if (!body) return std::nullopt;
    try {
        auto j = json::parse(*body);
        if (j.value("code", -1) != 0) return std::nullopt;
        return PickStream(j, qn);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<StreamUrl> GetStreamUrl(long long realRoomId) {
    return GetStreamUrl(realRoomId, 10000);  // 最高画质（兼容旧调用）
}

std::vector<QualityDesc> GetAcceptQn(long long realRoomId) {
    std::vector<QualityDesc> out;
    char path[256];
    snprintf(path, sizeof(path),
             "/xlive/web-room/v2/index/getRoomPlayInfo?room_id=%lld"
             "&protocol=0,1&format=0,1,2&codec=0,1&qn=10000&platform=web",
             realRoomId);
    auto body = HttpGet(L"api.live.bilibili.com", ToWide(path), "", L"https://live.bilibili.com/");
    if (!body) return out;
    try {
        auto j = json::parse(*body);
        if (j.value("code", -1) != 0) return out;
        // g_qn_desc：{qn, desc} 画质档位说明
        auto& desc = j["data"]["playurl_info"]["playurl"]["g_qn_desc"];
        for (auto& d : desc) {
            QualityDesc q;
            q.qn = d.value("qn", 0);
            q.desc = d.value("desc", "");
            if (q.qn > 0) out.push_back(std::move(q));
        }
        // 由高到低
        std::sort(out.begin(), out.end(), [](const QualityDesc& a, const QualityDesc& b) {
            return a.qn > b.qn;
        });
    } catch (...) { /* 解析失败返回空 */ }
    return out;
}

std::vector<SearchResult> SearchLiveUsers(const std::wstring& keyword) {
    std::vector<SearchResult> out;
    std::string q = "/x/web-interface/search/type?search_type=live_user&keyword=" +
                    UrlEncode(ToUtf8(keyword)) + "&page=1";
    auto body = HttpGet(L"api.bilibili.com", ToWide(q), "",
                        L"https://search.bilibili.com/");
    if (!body) return out;
    try {
        auto j = json::parse(*body);
        if (j.value("code", -1) != 0) return out;
        auto& results = j["data"]["result"];
        for (auto& r : results) {
            SearchResult s;
            s.name = StripHtml(r.value("uname", ""));
            s.roomid = r.value("roomid", 0LL);
            s.uid = r.value("uid", 0LL);
            s.fans = r.value("attentions", 0LL);
            s.live_status = r.value("live_status", 0);
            if (s.roomid != 0 && !s.name.empty()) out.push_back(std::move(s));
        }
    } catch (...) { /* 解析失败返回已收集结果 */ }
    // 按粉丝量由高到低排序（需求明确）
    std::sort(out.begin(), out.end(), [](const SearchResult& a, const SearchResult& b) {
        return a.fans > b.fans;
    });
    return out;
}

} // namespace bili
