#include "common/net_common.h"

#include <windows.h>

#include <iostream>
#include <mutex>

namespace chat {
namespace {

// 功能：获取全局日志锁，防止多个线程的输出互相穿插。
std::mutex& logMutex()
{
    static std::mutex mutex;
    return mutex;
}

}  // namespace

// 功能：初始化 Windows Socket 环境并协商使用 Socket 2.2。
bool initializeNetwork(std::string& errorMessage)
{
    WSADATA wsaData{};
    const int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        errorMessage = "WSAStartup failed: " + socketErrorMessage(result);
        return false;
    }

    return true;
}

// 功能：释放 Windows Socket 环境占用的资源。
void cleanupNetwork()
{
    WSACleanup();
}

// 功能：把 Socket 错误码转换成便于日志输出的文字。
std::string socketErrorMessage(int errorCode)
{
    return "error " + std::to_string(errorCode);
}

// 功能：循环发送指定长度的数据，直到全部字节发送完成。
bool sendAll(SOCKET socketValue, const char* data, int length)
{
    int totalSent = 0;

    // TCP 的 send 可能只发送一部分，必须继续发送剩余字节。
    while (totalSent < length) {
        const int sentCount = send(
            socketValue,
            data + totalSent,
            length - totalSent,
            0
        );

        if (sentCount <= 0) {
            return false;
        }

        totalSent += sentCount;
    }

    return true;
}

// 功能：循环接收指定长度的数据，直到收满或连接异常结束。
IoStatus receiveExactly(SOCKET socketValue, char* data, int length)
{
    int totalReceived = 0;

    // 固定长度协议字段必须收满，不能把一次 recv 当成完整边界。
    while (totalReceived < length) {
        const int receivedCount = recv(
            socketValue,
            data + totalReceived,
            length - totalReceived,
            0
        );

        if (receivedCount == 0) {
            return IoStatus::Closed;
        }

        if (receivedCount == SOCKET_ERROR) {
            return IoStatus::Error;
        }

        totalReceived += receivedCount;
    }

    return IoStatus::Success;
}

// 功能：按“长度字段 + 正文”的格式发送一条完整协议消息。
bool sendFrame(SOCKET socketValue, const std::string& payload)
{
    if (payload.empty() || payload.size() > kMaxFrameBytes) {
        return false;
    }

    // TCP 使用网络字节序；先发送 4 字节长度，再发送正文。
    const std::uint32_t networkLength =
        htonl(static_cast<std::uint32_t>(payload.size()));

    if (!sendAll(
            socketValue,
            reinterpret_cast<const char*>(&networkLength),
            static_cast<int>(sizeof(networkLength)))) {
        return false;
    }

    return sendAll(
        socketValue,
        payload.data(),
        static_cast<int>(payload.size())
    );
}

// 功能：先收满长度字段，再收满正文，返回一条完整协议消息。
IoStatus receiveFrame(SOCKET socketValue, std::string& payload)
{
    std::uint32_t networkLength = 0;

    // 第一步：收满长度字段。
    const IoStatus headerStatus = receiveExactly(
        socketValue,
        reinterpret_cast<char*>(&networkLength),
        static_cast<int>(sizeof(networkLength))
    );

    if (headerStatus != IoStatus::Success) {
        return headerStatus;
    }

    // 第二步：转换回本机字节序并校验上限。
    const std::uint32_t payloadLength = ntohl(networkLength);
    if (payloadLength == 0 || payloadLength > kMaxFrameBytes) {
        return IoStatus::Error;
    }

    // 第三步：按长度收满正文，避免粘包和拆包被误解析。
    payload.resize(payloadLength);

    return receiveExactly(
        socketValue,
        payload.data(),
        static_cast<int>(payloadLength)
    );
}

// 功能：把 Windows 控制台输入输出编码切换为 UTF-8。
void configureConsoleUtf8()
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

// 功能：线程安全地输出一行服务器或客户端日志。
void logLine(const std::string& text)
{
    std::lock_guard<std::mutex> lock(logMutex());
    std::cout << text << '\n';
}

}  // namespace chat
