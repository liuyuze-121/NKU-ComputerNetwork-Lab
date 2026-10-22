#include "common/net_common.h"
#include "server/chat_server.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

// 功能：读取监听端口参数并启动聊天服务器。
int main(int argc, char* argv[])
{
    // 让控制台按 UTF-8 显示中英文日志。
    chat::configureConsoleUtf8();

    // 默认监听 9000，也支持通过命令行参数指定其他端口。
    std::uint16_t port = 9000;
    if (argc >= 2) {
        try {
            const unsigned long parsedPort = std::stoul(argv[1]);
            if (parsedPort == 0 || parsedPort > 65535) {
                std::cerr << "Port must be in the range 1..65535.\n";
                return 1;
            }
            port = static_cast<std::uint16_t>(parsedPort);
        }
        catch (const std::exception&) {
            std::cerr << "Invalid port: " << argv[1] << '\n';
            return 1;
        }
    }

    return chat::runChatServer(port);
}
