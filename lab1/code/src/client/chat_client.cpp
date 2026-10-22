#include "client/chat_client.h"

#include "common/net_common.h"
#include "common/protocol.h"

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace chat {
namespace {

// 客户端共享状态：主线程负责输入，接收线程负责处理服务器广播。
struct ClientContext {
    SOCKET socketValue = INVALID_SOCKET;
    std::atomic<bool> running{true};
    std::mutex sendMutex;   // 保证同一时刻只有一个线程发送完整帧。
    std::mutex outputMutex; // 避免提示符和远程消息输出互相穿插。
};

// 功能：线程安全地向命令行输出一行文字。
void printOutput(ClientContext& context, const std::string& text)
{
    std::lock_guard<std::mutex> lock(context.outputMutex);
    std::cout << text << '\n';
}

// 功能：加锁后向服务器发送一条完整协议消息。
bool sendWithLock(ClientContext& context,
                  const std::string& payload)
{
    std::lock_guard<std::mutex> lock(context.sendMutex);
    return sendFrame(context.socketValue, payload);
}

// 功能：持续接收服务器消息，并按 CHAT、SYSTEM、ERROR 分类显示。
void receiveMessages(ClientContext& context)
{
    // 接收线程持续读取完整帧，并按 message type 分派显示逻辑。
    while (context.running.load()) {
        std::string payload;
        const IoStatus status = receiveFrame(
            context.socketValue,
            payload
        );

        if (status == IoStatus::Closed) {
            printOutput(context, "\n[系统] 服务器关闭了连接。");
            break;
        }

        if (status == IoStatus::Error) {
            if (context.running.load()) {
                printOutput(
                    context,
                    "\n[网络] 接收数据失败，错误码：" +
                    std::to_string(WSAGetLastError())
                );
            }
            break;
        }

        // 只读取类型用于分发；具体字段由对应的 parse 函数校验。
        const MessageParts parts = splitMessage(payload, 5);

        if (parts.type == "CHAT") {
            std::uint64_t sequence = 0;
            std::string timestamp;
            std::string username;
            std::string content;

            // CHAT 展示服务器生成的 timestamp 和 sequence number。
            if (parseChatBroadcast(
                    payload,
                    sequence,
                    timestamp,
                    username,
                    content)) {
                printOutput(
                    context,
                    "[" + timestamp + "] " + username +
                    " (#" + std::to_string(sequence) + "): " +
                    content
                );
            }
            else {
                printOutput(context, "[协议] 收到无法解析的 CHAT 消息。");
            }
            continue;
        }

        if (parts.type == "SYSTEM") {
            std::string timestamp;
            std::string content;
            if (parseSystemMessage(payload, timestamp, content)) {
                printOutput(
                    context,
                    "[" + timestamp + "] * " + content
                );
            }
            continue;
        }

        if (parts.type == "ERROR") {
            int code = 0;
            std::string message;
            if (parseErrorMessage(payload, code, message)) {
                printOutput(
                    context,
                    "[错误 " + std::to_string(code) + "] " + message
                );
            }
            continue;
        }

        printOutput(context, "[协议] 未知服务器消息：" + payload);
    }

    // 接收线程结束时唤醒主线程中可能阻塞的 socket 操作。
    context.running.store(false);
    shutdown(context.socketValue, SD_BOTH);
}

// 功能：输出客户端支持的聊天和测试命令。
void printUsage(ClientContext& context)
{
    printOutput(
        context,
        "命令说明：\n"
        "  直接输入文字：发送聊天消息\n"
        "  /stress 数量：连续发送测试消息，例如 /stress 100\n"
        "  /help：显示命令说明\n"
        "  /quit：退出聊天室"
    );
}

// 功能：连续发送指定数量的 SEND 消息，用于观察丢包和消息序号。
bool sendStressMessages(ClientContext& context,
                        const std::string& argument)
{
    long count = 0;

    try {
        count = std::stol(argument);
    }
    catch (...) {
        printOutput(context, "用法：/stress 数量，例如 /stress 100");
        return true;
    }

    if (count < 1 || count > 500) {
        printOutput(context, "压力测试数量必须在 1 到 500 之间。");
        return true;
    }

    // 连续发送固定数量的测试消息，用于观察接收序号是否完整。
    for (long index = 1; index <= count; ++index) {
        const std::string content =
            "丢包测试消息 " + std::to_string(index);

        if (!sendWithLock(context, buildMessage({"SEND", content}))) {
            printOutput(context, "压力测试发送失败。");
            return false;
        }
    }

    printOutput(
        context,
        "已连续发送 " + std::to_string(count) +
        " 条消息，请观察显示的序号是否连续。"
    );
    return true;
}

// 功能：创建客户端 Socket 并连接到指定服务器地址和端口。
bool connectToServer(ClientContext& context,
                     const std::string& serverAddress,
                     std::uint16_t port)
{
    const unsigned long addressValue = inet_addr(serverAddress.c_str());
    if (addressValue == INADDR_NONE &&
        serverAddress != "255.255.255.255") {
        printOutput(context, "服务器 IPv4 地址格式无效。");
        return false;
    }

    // 客户端流程：socket -> connect，连接成功后即可收发。
    context.socketValue = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (context.socketValue == INVALID_SOCKET) {
        printOutput(
            context,
            "创建 Socket 失败，错误码：" +
            std::to_string(WSAGetLastError())
        );
        return false;
    }

    sockaddr_in serverEndpoint{};
    serverEndpoint.sin_family = AF_INET;
    serverEndpoint.sin_port = htons(port);
    serverEndpoint.sin_addr.s_addr = addressValue;

    if (connect(
            context.socketValue,
            reinterpret_cast<sockaddr*>(&serverEndpoint),
            sizeof(serverEndpoint)) == SOCKET_ERROR) {
        printOutput(
            context,
            "连接服务器失败，错误码：" +
            std::to_string(WSAGetLastError())
        );
        closesocket(context.socketValue);
        context.socketValue = INVALID_SOCKET;
        return false;
    }

    printOutput(
        context,
        "已连接服务器 " + serverAddress + ":" +
        std::to_string(port)
    );
    return true;
}

// 功能：输入用户名、发送 JOIN，并等待服务器返回登录结果。
bool performLogin(ClientContext& context)
{
    std::string username;

    // 本地先做格式检查，减少无效 JOIN 往返。
    while (true) {
        std::cout << "请输入用户名（字母、数字、下划线，最多 20 字节）：";
        if (!std::getline(std::cin, username)) {
            return false;
        }

        if (isValidUsername(username)) {
            break;
        }

        std::cout << "用户名格式不正确，请重新输入。\n";
    }

    // 登录后先同步等待 JOIN_OK，确认用户名已被服务器接受。
    if (!sendWithLock(
            context,
            buildMessage({"JOIN", username}))) {
        printOutput(context, "发送登录消息失败。");
        return false;
    }

    std::string responsePayload;
    const IoStatus status = receiveFrame(
        context.socketValue,
        responsePayload
    );

    if (status != IoStatus::Success) {
        printOutput(context, "登录时连接中断。");
        return false;
    }

    std::string acceptedUsername;
    if (parseJoinOk(responsePayload, acceptedUsername)) {
        printOutput(
            context,
            "登录成功，当前用户：" + acceptedUsername
        );
        return true;
    }

    int code = 0;
    std::string message;
    if (parseErrorMessage(responsePayload, code, message)) {
        printOutput(
            context,
            "登录失败（错误 " + std::to_string(code) +
            "）：" + message
        );
    }
    else {
        printOutput(
            context,
            "登录失败，服务器返回了未知消息。"
        );
    }

    return false;
}

// 功能：读取命令行输入并发送聊天、退出或压力测试命令。
void runInputLoop(ClientContext& context)
{
    printUsage(context);

    std::string line;
    while (context.running.load()) {
        std::cout << "> ";
        std::cout.flush();

        if (!std::getline(std::cin, line)) {
            break;
        }

        if (line.empty()) {
            continue;
        }

        if (line == "/quit") {
            // 先发送 QUIT 让服务器清理会话，再关闭发送方向。
            sendWithLock(context, "QUIT");
            shutdown(context.socketValue, SD_SEND);
            break;
        }

        if (line == "/help") {
            printUsage(context);
            continue;
        }

        // /stress 用于实验丢包观察，不属于线上聊天协议字段。
        if (line.rfind("/stress ", 0) == 0) {
            if (!sendStressMessages(context, line.substr(8))) {
                break;
            }
            continue;
        }

        if (!isValidChatContent(line)) {
            printOutput(
                context,
                "消息不能为空、不能包含换行，且不能超过 3000 字节。"
            );
            continue;
        }

        // 普通文本统一封装为 SEND 请求，由服务器补充 username 和时间。
        if (!sendWithLock(
                context,
                buildMessage({"SEND", line}))) {
            printOutput(context, "发送消息失败。");
            break;
        }
    }
}

}  // namespace

// 功能：启动客户端，并行运行输入循环和网络接收线程。
int runChatClient(const std::string& serverAddress,
                  std::uint16_t port)
{
    std::string networkError;
    if (!initializeNetwork(networkError)) {
        logLine("[ERROR] " + networkError);
        return 1;
    }

    ClientContext context;
    if (!connectToServer(context, serverAddress, port)) {
        cleanupNetwork();
        return 1;
    }

    if (!performLogin(context)) {
        closesocket(context.socketValue);
        cleanupNetwork();
        return 1;
    }

    // 接收线程和输入循环并行运行，用户可以边输入边接收广播。
    std::thread receiver(receiveMessages, std::ref(context));
    runInputLoop(context);

    // 先停止接收线程，再关闭 socket，最后等待线程退出。
    context.running.store(false);
    shutdown(context.socketValue, SD_BOTH);

    if (receiver.joinable()) {
        receiver.join();
    }

    {
        std::lock_guard<std::mutex> sendLock(context.sendMutex);
        closesocket(context.socketValue);
        context.socketValue = INVALID_SOCKET;
    }

    cleanupNetwork();
    printOutput(context, "客户端已经退出。");
    return 0;
}

}  // namespace chat
