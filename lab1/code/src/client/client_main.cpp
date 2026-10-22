#include "client/chat_client.h"
#include "common/net_common.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

// 功能：解析服务器 IP 和端口参数并启动聊天客户端。
int main(int argc, char* argv[])
{
    // 让命令行能够正确显示 UTF-8 中英文。
    chat::configureConsoleUtf8();

    // 用法：chat_client.exe [server_ip] [port]。
    std::string serverAddress = "127.0.0.1";
    std::uint16_t port = 9000;

    if (argc >= 2) {
        serverAddress = argv[1];
    }

    if (argc >= 3) {
        try {
            const unsigned long parsedPort = std::stoul(argv[2]);
            if (parsedPort == 0 || parsedPort > 65535) {
                std::cerr << "Port must be in the range 1..65535.\n";
                return 1;
            }
            port = static_cast<std::uint16_t>(parsedPort);
        }
        catch (const std::exception&) {
            std::cerr << "Invalid port: " << argv[2] << '\n';
            return 1;
        }
    }

    return chat::runChatClient(serverAddress, port);
}
