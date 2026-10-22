#pragma once

#include <cstdint>

namespace chat {

// 功能：启动监听循环并运行聊天服务器。
int runChatServer(std::uint16_t port);

}  // namespace chat
