# ============================================================
# BiliLiveFloat —— B站直播小窗播放器
# 纯 C++ / Win32 / libmpv（MSVC 2022 + CMake）
# 构建：cmake -B build -A x64 && cmake --build build --config Release
# ============================================================

cmake_minimum_required(VERSION 3.20)
project(BiliLiveFloat LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# 第三方依赖目录
set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# ------------------------------------------------------------
# 1) nlohmann-json：仅头文件
# ------------------------------------------------------------
set(JSON_SINGLE_INCLUDE
    "${THIRD_PARTY_DIR}/json/single_include/nlohmann/json.hpp")
if(EXISTS "${JSON_SINGLE_INCLUDE}")
  message(STATUS "nlohmann-json: using single header")
else()
  message(FATAL_ERROR "json.hpp not found at ${JSON_SINGLE_INCLUDE}")
endif()

# ------------------------------------------------------------
# 2) zlib：源码子目录编译（静态，保证单 exe 无额外 dll）
# ------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)   # zlib/brotli 均以静态构建
set(ZLIB_DIR "${THIRD_PARTY_DIR}/zlib")
if(EXISTS "${ZLIB_DIR}/CMakeLists.txt")
  add_subdirectory("${ZLIB_DIR}" "${CMAKE_BINARY_DIR}/zlib" EXCLUDE_FROM_ALL)
else()
  message(FATAL_ERROR "zlib source not found at ${ZLIB_DIR}")
endif()

# ------------------------------------------------------------
# 3) brotli：源码子目录编译（brotlicommon/brotlidec 足够）
# ------------------------------------------------------------
set(BROTLI_DIR "${THIRD_PARTY_DIR}/brotli")
if(EXISTS "${BROTLI_DIR}/CMakeLists.txt")
  add_subdirectory("${BROTLI_DIR}" "${CMAKE_BINARY_DIR}/brotli" EXCLUDE_FROM_ALL)
  # brotli 默认（BUILD_SHARED_LIBS=OFF）构建静态库，target 名不带 -static 后缀
  set(BROTLI_LIBS brotlidec brotlicommon)
else()
  message(FATAL_ERROR "brotli source not found at ${BROTLI_DIR}")
endif()

# ------------------------------------------------------------
# 4) libmpv：使用官方发布包（libmpv-2.dll + 头文件）
#    链接用 MSVC 导入库 libmpv.lib，由脚本从 dll 导出表生成
# ------------------------------------------------------------
set(MPV_DIR "${THIRD_PARTY_DIR}/libmpv")
set(MPV_INCLUDE_DIR "${MPV_DIR}/include")
set(MPV_DLL "${MPV_DIR}/libmpv-2.dll")
set(MPV_LIB "${MPV_DIR}/libmpv.lib")

if(EXISTS "${MPV_INCLUDE_DIR}/mpv/client.h" AND EXISTS "${MPV_DLL}")
  message(STATUS "libmpv: found dll + headers")
else()
  message(FATAL_ERROR "libmpv not found under ${MPV_DIR} (need include/mpv + libmpv-2.dll)")
endif()

# 若缺少 MSVC 导入库，则尝试用 dumpbin + lib 生成
if(NOT EXISTS "${MPV_LIB}")
  message(STATUS "libmpv.lib missing, will try to generate from dll exports at build time")
endif()

# ------------------------------------------------------------
# 主程序
# ------------------------------------------------------------
add_executable(BiliLiveFloat WIN32
  src/main.cpp
  src/app_window.cpp
  src/player.cpp
  src/bili_api.cpp
  src/danmaku_client.cpp
  src/ws_socket.cpp
  src/danmaku_renderer.cpp
  src/room_switch_dialog.cpp
  src/config.cpp
  src/search_window.cpp
  app.rc
)

target_include_directories(BiliLiveFloat PRIVATE
  src
  "${THIRD_PARTY_DIR}/json/single_include"   # nlohmann/json.hpp
  "${MPV_INCLUDE_DIR}"
  "${THIRD_PARTY_DIR}/brotli/c/include"      # brotli/decode.h
  "${THIRD_PARTY_DIR}/zlib"                  # zlib.h
  "${CMAKE_BINARY_DIR}/zlib"                 # zconf.h（CMake 生成）
)

# 运行库：/MT（Release 静态运行库，避免缺 vcruntime dll）
target_compile_options(BiliLiveFloat PRIVATE
  /W4 /permissive- /utf-8
  $<$<CONFIG:Release>:/MT>
  $<$<CONFIG:Debug>:/MTd>
)

target_compile_definitions(BiliLiveFloat PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  _CRT_SECURE_NO_WARNINGS
)

# Windows 系统库
target_link_libraries(BiliLiveFloat PRIVATE
  user32 gdi32 shell32 ole32 comdlg32
  winhttp
  d2d1 dwrite dxgi d3d11 d3dcompiler
  zlibstatic      # zlib 静态库 target
  ${BROTLI_LIBS}  # brotlidec / brotlicommon（静态）
)

# 链接 libmpv：优先 MSVC 导入库，其次 MinGW 导入库（dll.a），
# 两者都没有则编译期报错并给出生成命令
if(EXISTS "${MPV_DIR}/libmpv.lib")
  target_link_libraries(BiliLiveFloat PRIVATE "${MPV_DIR}/libmpv.lib")
elseif(EXISTS "${MPV_DIR}/libmpv.dll.a")
  target_link_libraries(BiliLiveFloat PRIVATE "${MPV_DIR}/libmpv.dll.a")
else()
  message(FATAL_ERROR
    "No libmpv import lib found. Generate it with:\n"
    "  dumpbin /exports libmpv-2.dll > exports.txt\n"
    "  (build a .def, then) lib /machine:x64 /def:libmpv.def /out:libmpv.lib")
endif()

# 把 libmpv-2.dll 复制到输出目录
add_custom_command(TARGET BiliLiveFloat POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${MPV_DLL}" "$<TARGET_FILE_DIR:BiliLiveFloat>/libmpv-2.dll"
  COMMENT "Copying libmpv-2.dll next to exe")

# 无控制台窗口、Release 单 exe
set_target_properties(BiliLiveFloat PROPERTIES
  VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:BiliLiveFloat>"
)

# 诊断用最小入口测试
add_executable(test_winmain WIN32 test_main.cpp)
target_compile_options(test_winmain PRIVATE /utf-8)
add_executable(test_console test_console.cpp)
target_compile_options(test_console PRIVATE /utf-8)
add_executable(test_api_console test_api_console.cpp
  src/bili_api.cpp src/danmaku_client.cpp src/ws_socket.cpp)
add_executable(test_ws_tls test_ws_tls.cpp src/ws_socket.cpp)
add_executable(test_tcp test_tcp.cpp)
target_compile_options(test_tcp PRIVATE /utf-8)
target_compile_options(test_ws_tls PRIVATE /utf-8)
target_include_directories(test_ws_tls PRIVATE src)
target_compile_options(test_api_console PRIVATE /utf-8)
target_include_directories(test_api_console PRIVATE src "${THIRD_PARTY_DIR}/json/single_include"
  "${THIRD_PARTY_DIR}/brotli/c/include" "${THIRD_PARTY_DIR}/zlib" "${CMAKE_BINARY_DIR}/zlib")
target_link_libraries(test_api_console PRIVATE user32 winhttp bcrypt zlibstatic ${BROTLI_LIBS})
add_executable(test_mpv_play test_mpv_play.cpp src/bili_api.cpp)
target_compile_options(test_mpv_play PRIVATE /utf-8)
target_include_directories(test_mpv_play PRIVATE src "${THIRD_PARTY_DIR}/libmpv/include" "${THIRD_PARTY_DIR}/json/single_include")
target_link_libraries(test_mpv_play PRIVATE user32 winhttp bcrypt "${MPV_DIR}/libmpv.lib")
add_executable(test_search test_search.cpp src/bili_api.cpp)
target_compile_options(test_search PRIVATE /utf-8)
target_include_directories(test_search PRIVATE src "${THIRD_PARTY_DIR}/json/single_include")
target_link_libraries(test_search PRIVATE user32 winhttp bcrypt)

add_executable(test_quality test_quality.cpp src/bili_api.cpp)
target_compile_options(test_quality PRIVATE /utf-8)
target_include_directories(test_quality PRIVATE src "${THIRD_PARTY_DIR}/json/single_include")
target_link_libraries(test_quality PRIVATE user32 winhttp bcrypt)
