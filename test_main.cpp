// test_main.cpp (diagnostic v3)
#include <windows.h>
#include <cstdio>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    FILE* f = nullptr;
    _wfopen_s(&f, L"D:\bilibilibilil\build\test_winmain.log", L"w");
    if (f) {
        fprintf(f, "reached\n");
        fclose(f);
    }
    return 42;   // 独特退出码：证明 WinMain 被执行
}
