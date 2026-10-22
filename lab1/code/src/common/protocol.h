#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace chat {

// 协议字段限制：username 使用 ASCII 字符集，content 使用 UTF-8。
constexpr std::size_t kMaxUsernameBytes = 20;
constexpr std::size_t kMaxChatContentBytes = 3000;

// 将 "TYPE|arg1|arg2" 拆为消息类型和参数列表。
struct MessageParts {
    std::string type;
    std::vector<std::string> arguments;
};

// 功能：把协议正文拆成消息类型和参数字段。
MessageParts splitMessage(const std::string& payload,
                          std::size_t maximumParts);

// 功能：把多个字段组装成竖线分隔的协议正文。
std::string buildMessage(const std::vector<std::string>& fields);

// 功能：检查字符串是否为结构合法的 UTF-8。
bool isValidUtf8(const std::string& text);

// 功能：检查用户名格式是否符合协议。
bool isValidUsername(const std::string& username);

// 功能：检查聊天正文长度、编码和换行约束。
bool isValidChatContent(const std::string& content);

// 功能：生成服务器当前时间字符串。
std::string currentTimestamp();

// 功能：构造登录成功消息。
std::string makeJoinOk(const std::string& username);

// 功能：构造聊天广播消息。
std::string makeChatBroadcast(std::uint64_t sequence,
                              const std::string& timestamp,
                              const std::string& username,
                              const std::string& content);

// 功能：构造系统通知消息。
std::string makeSystemMessage(const std::string& timestamp,
                              const std::string& content);

// 功能：构造错误响应消息。
std::string makeErrorMessage(int code, const std::string& message);

// 功能：解析客户端的登录请求。
bool parseJoinRequest(const std::string& payload,
                      std::string& username,
                      std::string& errorMessage);

// 功能：解析客户端的聊天发送请求。
bool parseSendRequest(const std::string& payload,
                      std::string& content,
                      std::string& errorMessage);

// 功能：解析服务器的登录成功响应。
bool parseJoinOk(const std::string& payload,
                 std::string& username);

// 功能：解析服务器广播的聊天消息。
bool parseChatBroadcast(const std::string& payload,
                        std::uint64_t& sequence,
                        std::string& timestamp,
                        std::string& username,
                        std::string& content);

// 功能：解析服务器发送的系统消息。
bool parseSystemMessage(const std::string& payload,
                        std::string& timestamp,
                        std::string& content);

// 功能：解析服务器发送的错误消息。
bool parseErrorMessage(const std::string& payload,
                       int& code,
                       std::string& message);

}  // namespace chat
