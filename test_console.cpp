// test_console.cpp (diagnostic console)
#include <cstdio>

int main() {
    FILE* f = nullptr;
    fopen_s(&f, "D:\\bilibilibilil\\build\\test_console.log", "w");
    if (f) {
        fprintf(f, "console reached\n");
        fclose(f);
    }
    return 42;
}
