#include "server/chat_server.h"

#include "common/net_common.h"
#include "common/protocol.h"

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace chat {
namespace {

// 每个 socket 对应一个会话；用户名登录成功后才参与广播。
struct ClientSession {
    std::uint64_t id = 0;
    SOCKET socketValue = INVALID_SOCKET;
    std::string username;
    bool loggedIn = false;
    bool usernameReserved = false;
    std::atomic<bool> active{true};
    std::mutex sendMutex;  // 防止多个线程把同一帧写成交错字节。
};

// 服务器全局状态：监听 socket、在线列表和消息序号。
struct ChatServer {
    SOCKET listenSocket = INVALID_SOCKET;
    std::mutex clientsMutex;   // 保护 clients 和会话登录状态。
    std::vector<std::shared_ptr<ClientSession>> clients;
    std::mutex broadcastMutex; // 串行化序号分配与广播顺序。
    std::uint64_t nextSequence = 1;
    std::uint64_t nextClientId = 1;
    std::atomic<bool> running{true};
};

// 功能：向一个客户端发送完整协议消息，并处理发送失败。
bool sendPayload(const std::shared_ptr<ClientSession>& client,
                 const std::string& payload)
{
    if (!client->active.load()) {
        return false;
    }

    // 同一连接只允许一个线程完整发送一帧。
    std::lock_guard<std::mutex> sendLock(client->sendMutex);
    if (!client->active.load()) {
        return false;
    }

    if (!sendFrame(client->socketValue, payload)) {
        // 写失败后先标记失效，让阻塞中的接收线程尽快退出。
        client->active.store(false);
        shutdown(client->socketValue, SD_BOTH);
        return false;
    }

    return true;
}

// 功能：复制当前已登录客户端列表，供安全广播使用。
std::vector<std::shared_ptr<ClientSession>>
loggedClientSnapshot(ChatServer& server)
{
    std::vector<std::shared_ptr<ClientSession>> snapshot;
    std::lock_guard<std::mutex> lock(server.clientsMutex);

    // 复制 shared_ptr 快照后释放 clientsMutex，避免持锁做网络发送。
    for (const auto& client : server.clients) {
        if (client->loggedIn) {
            snapshot.push_back(client);
        }
    }

    return snapshot;
}

// 功能：把同一条消息广播给全部已登录客户端。
void broadcast(ChatServer& server, const std::string& payload)
{
    const auto clients = loggedClientSnapshot(server);
    for (const auto& client : clients) {
        sendPayload(client, payload);
    }
}

// 功能：在并发登录时检查并保留一个唯一用户名。
bool reserveUsername(ChatServer& server,
                     const std::shared_ptr<ClientSession>& client,
                     const std::string& username)
{
    std::lock_guard<std::mutex> lock(server.clientsMutex);

    // 在 JOIN_OK 发出前先保留用户名，避免并发加入时出现重名。
    for (const auto& other : server.clients) {
        if (other->id != client->id &&
            other->usernameReserved &&
            other->username == username) {
            return false;
        }
    }

    client->username = username;
    client->usernameReserved = true;
    return true;
}

// 功能：把用户名已保留的会话正式标记为已登录。
void markClientLoggedIn(ChatServer& server,
                        const std::shared_ptr<ClientSession>& client)
{
    std::lock_guard<std::mutex> lock(server.clientsMutex);
    client->loggedIn = true;
}

// 功能：从在线列表中删除会话并关闭其 Socket。
std::string removeClient(ChatServer& server,
                         const std::shared_ptr<ClientSession>& client)
{
    std::string removedUsername;

    // 先移出在线列表，再把连接标记为失效。
    {
        std::lock_guard<std::mutex> lock(server.clientsMutex);
        if (client->loggedIn) {
            removedUsername = client->username;
        }

        client->loggedIn = false;
        client->usernameReserved = false;

        for (auto iterator = server.clients.begin();
             iterator != server.clients.end();
             ++iterator) {
            if ((*iterator)->id == client->id) {
                server.clients.erase(iterator);
                break;
            }
        }
    }

    client->active.store(false);
    // 等发送中的线程结束后再关闭 socket，避免使用已释放的句柄。
    {
        std::lock_guard<std::mutex> sendLock(client->sendMutex);
        shutdown(client->socketValue, SD_BOTH);
        closesocket(client->socketValue);
        client->socketValue = INVALID_SOCKET;
    }

    return removedUsername;
}

// 功能：生成并广播一条带服务器时间的 SYSTEM 消息。
void announceSystem(ChatServer& server, const std::string& content)
{
    const std::string payload = makeSystemMessage(
        currentTimestamp(),
        content
    );

    // SYSTEM 与 CHAT 共用广播锁，保证所有客户端观察顺序一致。
    std::lock_guard<std::mutex> broadcastLock(server.broadcastMutex);
    broadcast(server, payload);
}

// 功能：向指定客户端返回一条协议错误消息。
void sendProtocolError(const std::shared_ptr<ClientSession>& client,
                       int code,
                       const std::string& message)
{
    sendPayload(client, makeErrorMessage(code, message));
}

// 功能：执行用户名登记、返回 JOIN_OK 并广播加入通知。
bool performJoin(ChatServer& server,
                 const std::shared_ptr<ClientSession>& client,
                 const std::string& username)
{
    // 先校验并保留用户名，再返回 JOIN_OK，最后公开为已登录用户。
    if (!reserveUsername(server, client, username)) {
        sendProtocolError(client, 2, "用户名已被占用");
        return false;
    }

    if (!sendPayload(client, makeJoinOk(username))) {
        return false;
    }

    markClientLoggedIn(server, client);

    logLine("[JOIN] " + username + " has entered the chat room");
    announceSystem(server, username + " 加入聊天室");
    return true;
}

// 功能：处理一个客户端的完整生命周期，包括登录和消息循环。
void handleClient(ChatServer& server,
                  std::shared_ptr<ClientSession> client)
{
    // 第一条消息必须是 JOIN，未登录连接不能发送聊天内容。
    std::string payload;
    IoStatus status = receiveFrame(client->socketValue, payload);

    if (status != IoStatus::Success) {
        removeClient(server, client);
        return;
    }

    const MessageParts firstMessage = splitMessage(payload, 2);
    if (firstMessage.type != "JOIN") {
        sendProtocolError(client, 3, "连接后必须先发送 JOIN 登录");
        removeClient(server, client);
        return;
    }

    std::string username;
    std::string errorMessage;
    if (!parseJoinRequest(payload, username, errorMessage)) {
        sendProtocolError(client, 1, errorMessage);
        removeClient(server, client);
        return;
    }

    if (!performJoin(server, client, username)) {
        removeClient(server, client);
        return;
    }

    // 登录后循环读取完整帧，直到 QUIT、EOF 或网络错误。
    while (client->active.load() && server.running.load()) {
        status = receiveFrame(client->socketValue, payload);

        if (status == IoStatus::Closed) {
            break;
        }

        if (status == IoStatus::Error) {
            logLine("[NET] receive failed for " + client->username);
            break;
        }

        const MessageParts parts = splitMessage(payload, 2);

        if (parts.type == "SEND") {
            if (parts.arguments.size() != 1) {
                sendProtocolError(client, 4, "SEND 消息缺少正文");
                continue;
            }

            std::string content;
            if (!parseSendRequest(payload, content, errorMessage)) {
                sendProtocolError(client, 5, errorMessage);
                continue;
            }

            // 在同一临界区分配序号并广播，避免并发聊天乱序。
            std::lock_guard<std::mutex> broadcastLock(
                server.broadcastMutex
            );

            const std::uint64_t sequence = server.nextSequence++;
            const std::string timestamp = currentTimestamp();
            const std::string broadcastPayload = makeChatBroadcast(
                sequence,
                timestamp,
                client->username,
                content
            );

            broadcast(server, broadcastPayload);
            logLine(
                "[CHAT] #" + std::to_string(sequence) +
                " " + client->username + ": " + content
            );
            continue;
        }

        if (parts.type == "JOIN") {
            sendProtocolError(client, 4, "该连接已经登录，不能重复发送 JOIN");
            continue;
        }

        if (parts.type == "QUIT") {
            if (!parts.arguments.empty()) {
                sendProtocolError(client, 4, "QUIT 消息不能带参数");
                continue;
            }
            break;
        }

        sendProtocolError(client, 6, "未知消息类型");
    }

    // 正常退出和异常退出共用同一清理流程。
    const std::string removedUsername = removeClient(server, client);
    if (!removedUsername.empty()) {
        logLine("[LEAVE] " + removedUsername + " left the chat room");
        announceSystem(server, removedUsername + " 离开聊天室");
    }
}

}  // namespace

// 功能：初始化服务器 Socket，并持续接受多个客户端连接。
int runChatServer(std::uint16_t port)
{
    std::string networkError;
    if (!initializeNetwork(networkError)) {
        logLine("[ERROR] " + networkError);
        return 1;
    }

    // 服务器固定流程：socket -> bind -> listen -> accept。
    ChatServer server;
    server.listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server.listenSocket == INVALID_SOCKET) {
        logLine("[ERROR] socket() failed: " +
                socketErrorMessage(WSAGetLastError()));
        cleanupNetwork();
        return 1;
    }

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(port);
    serverAddress.sin_addr.s_addr = INADDR_ANY;

    if (bind(
            server.listenSocket,
            reinterpret_cast<sockaddr*>(&serverAddress),
            sizeof(serverAddress)) == SOCKET_ERROR) {
        logLine("[ERROR] bind() failed: " +
                socketErrorMessage(WSAGetLastError()));
        closesocket(server.listenSocket);
        cleanupNetwork();
        return 1;
    }

    if (listen(server.listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        logLine("[ERROR] listen() failed: " +
                socketErrorMessage(WSAGetLastError()));
        closesocket(server.listenSocket);
        cleanupNetwork();
        return 1;
    }

    logLine(
        "[SERVER] listening on all IPv4 interfaces, port " +
        std::to_string(port)
    );
    logLine("[SERVER] press Ctrl+C to stop");

    // 主线程只负责接受连接；聊天处理交给独立工作线程。
    while (server.running.load()) {
        sockaddr_in clientAddress{};
        int clientAddressLength = sizeof(clientAddress);

        const SOCKET acceptedSocket = accept(
            server.listenSocket,
            reinterpret_cast<sockaddr*>(&clientAddress),
            &clientAddressLength
        );

        if (acceptedSocket == INVALID_SOCKET) {
            if (!server.running.load()) {
                break;
            }

            logLine("[ERROR] accept() failed: " +
                    socketErrorMessage(WSAGetLastError()));
            continue;
        }

        auto client = std::make_shared<ClientSession>();
        client->socketValue = acceptedSocket;

        {
            std::lock_guard<std::mutex> lock(server.clientsMutex);
            client->id = server.nextClientId++;
            server.clients.push_back(client);
        }

        std::thread(handleClient, std::ref(server), client).detach();
    }

    server.running.store(false);

    // 进程退出时统一关闭所有在线连接。
    {
        std::lock_guard<std::mutex> lock(server.clientsMutex);
        for (const auto& client : server.clients) {
            client->active.store(false);
            shutdown(client->socketValue, SD_BOTH);
            closesocket(client->socketValue);
        }
        server.clients.clear();
    }

    closesocket(server.listenSocket);
    cleanupNetwork();
    return 0;
}

}  // namespace chat
