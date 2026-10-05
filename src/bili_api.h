#pragma once
// ============================================================
// bili_api.h —— B站直播网络接口层（WinHTTP，同步）
// 实现：房间信息/真实房号解析、弹幕服务器配置（wbi 签名）、
//       播放流地址。所有请求带 Referer/UA 反爬头。
// 接口均已在本机实测（2026-10-05）。
// ============================================================

#include <string>
#include <vector>
#include <optional>

namespace bili {

// ---- 数据结构 -------------------------------------------------

// 直播间信息（get_info 返回）
struct RoomInfo {
    long long room_id = 0;    // 真实房号
    int live_status = 0;      // 0=未开播 1=直播中 2=轮播
    std::string title;
    std::string user_name;
    long long uid = 0;
};

// 弹幕服务器（getDanmuInfo 返回）
struct DanmakuServer {
    std::string host;
    int port = 2243;
    int wss_port = 443;
};

struct DanmakuConf {
    std::string token;
    std::vector<DanmakuServer> servers;
};

// 播放流（getRoomPlayInfo 返回）
struct StreamUrl {
    std::string url;      // 拼好的完整播放 URL
    std::string format;   // flv / ts / fmp4
    int qn = 0;           // 请求画质
};

// 画质档位（g_qn_desc：qn + 名称）
struct QualityDesc {
    int qn = 0;
    std::string desc;     // 如"超清 4K"/"蓝光"/"高清"
};

// 主播搜索结果（search_type=live_user，含未开播主播与粉丝数）
struct SearchResult {
    std::string name;      // 主播昵称（已剥离 HTML 标签）
    long long roomid = 0;  // 直播间房号
    long long uid = 0;
    long long fans = 0;    // 粉丝量（attentions）
    int live_status = 0;   // 0=未开播 1=直播中 2=轮播
};

// ---- 会话（buvid cookie + wbi 密钥缓存）-----------------------

class Session {
public:
    // 确保 buvid3/buvid4 cookie 与 wbi 密钥可用（惰性初始化）
    void EnsureReady();

    std::string cookie_header;    // "buvid3=...; buvid4=..."
    std::string img_key, sub_key; // wbi 密钥
    bool ready = false;

private:
    void FetchBuvid();   // spi 接口
    void FetchWbiKeys(); // nav 接口
};

// ---- 工具 -----------------------------------------------------

std::string UrlEncode(const std::string& s);
std::string WbiSign(const std::string& imgKey, const std::string& subKey,
                    const std::vector<std::pair<std::string, std::string>>& params);
std::string Md5Hex(const std::string& input);
// WinHTTP GET，返回响应正文；失败返回 std::nullopt
std::optional<std::string> HttpGet(const std::wstring& host,
                                   const std::wstring& path,
                                   const std::string& cookieHeader,
                                   const std::wstring& referer);

// ---- 业务接口 -------------------------------------------------

// 解析用户输入（纯数字/短链/完整URL）→ 房间号（可为短号，未解析出返回 nullopt）
std::optional<long long> ParseRoomInput(const std::wstring& input);

// 房间信息（内部先 get_info 拿真实房号，支持短号）
std::optional<RoomInfo> GetRoomInfo(long long roomIdOrShort);

// 弹幕服务器配置（getDanmuInfo + wbi 签名）
std::optional<DanmakuConf> GetDanmakuConf(long long realRoomId, Session& session);

// 播放流地址（getRoomPlayInfo，优先 flv/avc；qn 指定画质，失败回退降级）
std::optional<StreamUrl> GetStreamUrl(long long realRoomId, int qn);
// 播放流地址（最高画质，兼容旧调用）
std::optional<StreamUrl> GetStreamUrl(long long realRoomId);
// 房间可用画质列表（由高到低）
std::vector<QualityDesc> GetAcceptQn(long long realRoomId);

// 按名称关键词搜索主播直播间（live_user 搜索，含未开播；结果按粉丝量由高到低排序）
std::vector<SearchResult> SearchLiveUsers(const std::wstring& keyword);

} // namespace bili
