#pragma once
// ============================================================
// room_switch_dialog.h —— 切房输入对话框
// 纯代码动态构建（避免 rc 资源编译问题）。
// 模态显示，用户输入房间号/短链/完整 URL，确定后返回文本。
// ============================================================

#include <windows.h>
#include <string>

// 模态弹出切房对话框；用户点确定返回 true 并填充 result，
// 取消/关闭返回 false。
bool ShowRoomSwitchDialog(HWND parent, std::wstring& result);
