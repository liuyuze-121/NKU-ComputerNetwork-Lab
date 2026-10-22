#pragma once

#include <cstdint>
#include <string>

namespace chat {

// 功能：连接服务器并运行命令行聊天循环。
int runChatClient(const std::string& serverAddress,
                  std::uint16_t port);

}  // namespace chat
