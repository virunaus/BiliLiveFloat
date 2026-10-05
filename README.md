# BiliLiveFloat —— B站直播小窗播放器

纯 C++ / Win32 / libmpv 桌面应用：小窗置顶播放 B站直播，叠加实时弹幕，支持鼠标点击穿透双模式、快速切房、配置持久化。无 Electron、无 WebView、无 Python，Release x64 单 exe。**已通过真实桌面全量验收（2026-10-05）**。

## 功能

- **小窗置顶播放**：无边框置顶窗口，libmpv（`--wid` 嵌入 + gpu + d3d11 + hwdec=auto 硬解），可拖动、可缩放
- **实时弹幕**：wss 直连 B站弹幕服务器，brotli/zlib 解压、递归拆包、DANMU_MSG 解析，D2D/DirectWrite 分层窗口直绘弹幕（右→左游泳道防碰撞，每帧限 6 条、队列上限 200）
- **鼠标点击穿透双模式**：`Ctrl+Alt+X` 切换穿透/不透传（WS_EX_TRANSPARENT + NOACTIVATE + LAYERED，NCHITTEST 恒 HTTRANSPARENT）
- **快速切房**：`Ctrl+Alt+R` 呼出切房对话框，支持房间号、短号、完整链接三种输入；托盘右键菜单可一键回到最近直播间（最多 10 个）
- **配置持久化**：位置、尺寸、置顶、穿透、弹幕开关、最近房间、最后房间保存于 `%APPDATA%\BiliLiveFloat\config.json`
- **匿名拉流**：不登录、不操作账号，仅用公开接口（wbi 签名 + buvid cookie 获取弹幕配置）

## 快速开始

1. 双击 `BiliLiveFloat.exe`（默认播 cs-advent 直播间，或恢复上次房间）
2. 命令行带房间号启动：`BiliLiveFloat.exe 1883358196`
3. `Ctrl+Alt+R` 输入房间号 / 短号 / 链接切换直播间
4. `Ctrl+Alt+X` 切换鼠标穿透；穿透模式下可边看直播边操作其他窗口

## 构建

环境：Windows 10/11 x64、MSVC 2022 Build Tools（含 CMake、Windows SDK）。

```bat
cmake -S BiliLiveFloat -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

产物：`build\Release\BiliLiveFloat.exe`（静态链接，单 exe）+ 旁置 `libmpv-2.dll`。

依赖（工程内 `third_party\`，本地源码静态编译，不走 vcpkg）：libmpv（视频）、zlib v1.3.1 / brotli（弹幕解压）、nlohmann-json（JSON）。

## 里程碑状态（M1→M5，全部 ✅）

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M1 | 无边框置顶窗口 + D3D11 + libmpv 播放 | ✅ 桌面验收：画面/声音正常 |
| M2 | 弹幕 HTTP/WS/解压/渲染全链路 | ✅ 实测打通（WSS 认证 code=0 + 真实弹幕 + GUI 中文渲染） |
| M3 | 穿透模式（WS_EX_TRANSPARENT + 热键） | ✅ 桌面验收通过 |
| M4 | 切房（三种输入解析 + 配置持久化） | ✅ 桌面验收通过 |
| M5 | 托盘 / 提示 / README / 全量验收 | ✅ 完成 |

## 技术要点

- **WSS 自实现**：WinHTTP WebSocket API 与 B站弹幕服务器 TLS 不兼容（12019/12030 实证死路），改为原生 socket + schannel（SSPI）手写 TLS1.3 握手与 WS 帧（RFC6455，客户端掩码）；schannel 流加密（EncryptMessage/DecryptMessage）；升级响应以 `\n\n`（非标准）结束，解析已兼容
- **弹幕协议**：16B 大端头，op7 认证（roomid/protover=3/platform/buvid/key），op8 回包校验，op2 每 30s 心跳，op5 按 protover 解压后递归拆包
- **接口**：`room/v1/Room/get_info`、`xlive/web-room/v2/index/getRoomPlayInfo`（flv+avc 优先）、`xlive/web-room/v1/index/getDanmuInfo`（wbi 签名 + buvid3 cookie）
- **播放**：libmpv wid 嵌入 + gpu/d3d11 + hwdec=auto；**强制 http-version=1.1**（B站 CDN HTTP/2 与 curl 实测不兼容，报 HTTP/2 framing layer error）
- **弹幕层**：WS_EX_LAYERED 不能用于子窗口（err=87 实证）→ 独立顶层分层窗口；WM_MOVE 时 ClientToScreen 同步；UpdateLayeredWindow 的 ptDst 必须传 NULL（传 (0,0) 会把窗口钉回左上角）

## 目录结构

```
BiliLiveFloat\
├─ src\          main / app_window / player / bili_api / danmaku_client /
│                ws_socket / danmaku_renderer / room_switch_dialog / config
├─ third_party\  libmpv / zlib / brotli / json（本地源码静态）
├─ build\        Release 产物（exe + libmpv-2.dll）
├─ test_*.cpp    诊断测试（接口/弹幕/mpv 播放）
└─ CMakeLists.txt
```

## 已知边界

- 弹幕服务器部分后端 TLS1.2 偶发握手失败（schannel 分批输入），程序按服务器列表轮询 + 指数退避（1→30s 封顶）自动重连，实测多轮后稳定
- 仅匿名播放与弹幕，不实现登录、发弹幕、互动等账号操作（提示词明确禁止）
