// test_search.cpp —— 验证主播搜索接口（关键词 → 主播列表，粉丝升序）
#include <windows.h>
#include <cstdio>
#include <string>
#include "bili_api.h"

int main(int argc, char** argv) {
    std::wstring kw = L"老";
    if (argc > 1) {
        int n = MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, nullptr, 0);
        std::wstring w(n ? n - 1 : 0, L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, &w[0], n);
        kw = w;
    }
    auto rs = bili::SearchLiveUsers(kw);
    printf("search '%ls' -> %d results\n", kw.c_str(), (int)rs.size());
    for (auto& r : rs) {
        printf("  %-20s room=%-12lld fans=%-10lld status=%d\n",
               r.name.c_str(), r.roomid, r.fans, r.live_status);
    }
    return 0;
}
