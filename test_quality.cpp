// test_quality.cpp —— 验证画质 API（可用画质列表 + 指定画质拉流）
#include <windows.h>
#include <cstdio>
#include "bili_api.h"

int main(int argc, char** argv) {
    long long room = argc > 1 ? atoll(argv[1]) : 1883358196;
    auto qs = bili::GetAcceptQn(room);
    printf("room %lld accept qn: %d items\n", room, (int)qs.size());
    for (auto& q : qs)
        printf("  qn=%-6d %s\n", q.qn, q.desc.c_str());
    // 指定中间档位拉流
    if (!qs.empty()) {
        int mid = qs[qs.size() / 2].qn;
        auto s = bili::GetStreamUrl(room, mid);
        printf("GetStreamUrl(qn=%d) -> %s\n", mid, s ? "ok" : "FAIL");
        if (s) printf("  got qn=%d format=%s url=%.90s\n", s->qn, s->format.c_str(), s->url.c_str());
    }
    return 0;
}
