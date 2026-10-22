#include "common/protocol.h"

#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>

namespace chat {
namespace {

// 功能：使用竖线把多个字段拼接成一条协议消息。
std::string joinFields(const std::vector<std::string>& fields)
{
    std::string result;

    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index != 0) {
            result.push_back('|');
        }
        result += fields[index];
    }

    return result;
}

// 功能：把纯十进制字符串安全解析为无符号整数。
bool parseUnsignedInteger(const std::string& text,
                          std::uint64_t& value)
{
    if (text.empty()) {
        return false;
    }

    for (const char character : text) {
        if (!std::isdigit(static_cast<unsigned char>(character))) {
            return false;
        }
    }

    // 只接受纯十进制数字，避免把符号或空白解析成序号。
    try {
        const unsigned long long parsed = std::stoull(text);
        if (parsed > std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        value = static_cast<std::uint64_t>(parsed);
        return true;
    }
    catch (...) {
        return false;
    }
}

}  // namespace

// 功能：把协议正文拆分为消息类型和参数字段。
MessageParts splitMessage(const std::string& payload,
                          std::size_t maximumParts)
{
    MessageParts result;

    if (payload.empty() || maximumParts == 0) {
        return result;
    }

    // 先取消息类型；末字段会保留其中的竖线。
    const std::size_t firstSeparator = payload.find('|');
    if (firstSeparator == std::string::npos) {
        result.type = payload;
        return result;
    }

    result.type = payload.substr(0, firstSeparator);
    std::size_t start = firstSeparator + 1;
    const std::size_t maximumArguments = maximumParts - 1;

    // 只切分到上限前一个字段，最后一段整体保留为 content。
    while (result.arguments.size() + 1 < maximumArguments) {
        const std::size_t separator = payload.find('|', start);
        if (separator == std::string::npos) {
            break;
        }

        result.arguments.push_back(
            payload.substr(start, separator - start)
        );
        start = separator + 1;
    }

    result.arguments.push_back(payload.substr(start));
    return result;
}

// 功能：把字段列表组装成使用竖线分隔的协议正文。
std::string buildMessage(const std::vector<std::string>& fields)
{
    return joinFields(fields);
}

// 功能：检查字符串是否为结构合法的 UTF-8。
bool isValidUtf8(const std::string& text)
{
    std::size_t index = 0;

    // 逐码点校验 UTF-8，拒绝非法起始字节、错误续字节和过长编码。
    while (index < text.size()) {
        const unsigned char first =
            static_cast<unsigned char>(text[index]);

        if (first == 0) {
            return false;
        }

        if (first <= 0x7F) {
            ++index;
            continue;
        }

        if (first >= 0xC2 && first <= 0xDF) {
            if (index + 1 >= text.size()) {
                return false;
            }

            const unsigned char second =
                static_cast<unsigned char>(text[index + 1]);
            if ((second & 0xC0) != 0x80) {
                return false;
            }

            index += 2;
            continue;
        }

        if (first >= 0xE0 && first <= 0xEF) {
            if (index + 2 >= text.size()) {
                return false;
            }

            const unsigned char second =
                static_cast<unsigned char>(text[index + 1]);
            const unsigned char third =
                static_cast<unsigned char>(text[index + 2]);

            if ((third & 0xC0) != 0x80) {
                return false;
            }

            const bool validSecond =
                (first == 0xE0 && second >= 0xA0 && second <= 0xBF) ||
                (first >= 0xE1 && first <= 0xEC &&
                 second >= 0x80 && second <= 0xBF) ||
                (first == 0xED && second >= 0x80 && second <= 0x9F) ||
                (first >= 0xEE && first <= 0xEF &&
                 second >= 0x80 && second <= 0xBF);

            if (!validSecond) {
                return false;
            }

            index += 3;
            continue;
        }

        if (first >= 0xF0 && first <= 0xF4) {
            if (index + 3 >= text.size()) {
                return false;
            }

            const unsigned char second =
                static_cast<unsigned char>(text[index + 1]);
            const unsigned char third =
                static_cast<unsigned char>(text[index + 2]);
            const unsigned char fourth =
                static_cast<unsigned char>(text[index + 3]);

            if ((third & 0xC0) != 0x80 ||
                (fourth & 0xC0) != 0x80) {
                return false;
            }

            const bool validSecond =
                (first == 0xF0 && second >= 0x90 && second <= 0xBF) ||
                (first >= 0xF1 && first <= 0xF3 &&
                 second >= 0x80 && second <= 0xBF) ||
                (first == 0xF4 && second >= 0x80 && second <= 0x8F);

            if (!validSecond) {
                return false;
            }

            index += 4;
            continue;
        }

        return false;
    }

    return true;
}

// 功能：检查用户名长度和允许字符是否符合协议。
bool isValidUsername(const std::string& username)
{
    if (username.empty() ||
        username.size() > kMaxUsernameBytes) {
        return false;
    }

    // 用户名限定为 ASCII 字母、数字和下划线，便于协议解析。
    for (const char character : username) {
        const unsigned char value =
            static_cast<unsigned char>(character);

        if (!std::isalnum(value) && character != '_') {
            return false;
        }
    }

    return true;
}

// 功能：检查聊天正文是否非空、未超长且为合法 UTF-8。
bool isValidChatContent(const std::string& content)
{
    if (content.empty() ||
        content.size() > kMaxChatContentBytes) {
        return false;
    }

    // 聊天正文支持中英文，但不能带换行，避免破坏文本协议边界。
    return isValidUtf8(content) &&
           content.find('\n') == std::string::npos &&
           content.find('\r') == std::string::npos;
}

// 功能：生成服务器当前本地时间，格式为 YYYY-MM-DD HH:mm:ss。
std::string currentTimestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm localTime{};

#ifdef _WIN32
    localtime_s(&localTime, &now);
#else
    localtime_r(&now, &localTime);
#endif

    // 所有客户端看到的时间都来自服务器，避免客户端伪造时间。
    std::ostringstream output;
    output << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return output.str();
}

// 功能：构造登录成功消息 JOIN_OK。
std::string makeJoinOk(const std::string& username)
{
    return joinFields({"JOIN_OK", username});
}

// 功能：构造带序号、时间、用户名和正文的 CHAT 广播消息。
std::string makeChatBroadcast(std::uint64_t sequence,
                              const std::string& timestamp,
                              const std::string& username,
                              const std::string& content)
{
    return joinFields({
        "CHAT",
        std::to_string(sequence),
        timestamp,
        username,
        content
    });
}

// 功能：构造用户加入或离开等 SYSTEM 系统消息。
std::string makeSystemMessage(const std::string& timestamp,
                              const std::string& content)
{
    return joinFields({"SYSTEM", timestamp, content});
}

// 功能：构造 ERROR 错误响应消息。
std::string makeErrorMessage(int code, const std::string& message)
{
    return joinFields({"ERROR", std::to_string(code), message});
}

// 功能：解析并校验客户端的 JOIN 登录请求。
bool parseJoinRequest(const std::string& payload,
                      std::string& username,
                      std::string& errorMessage)
{
    // JOIN 只允许一个参数：username。
    const MessageParts parts = splitMessage(payload, 2);
    if (parts.type != "JOIN" || parts.arguments.size() != 1) {
        errorMessage = "登录消息格式应为 JOIN|username";
        return false;
    }

    if (!isValidUsername(parts.arguments[0])) {
        errorMessage = "用户名只能包含字母、数字和下划线，长度为 1 到 20 字节";
        return false;
    }

    username = parts.arguments[0];
    return true;
}

// 功能：解析并校验客户端的 SEND 聊天请求。
bool parseSendRequest(const std::string& payload,
                      std::string& content,
                      std::string& errorMessage)
{
    // SEND 只切一次，正文中可以继续出现竖线。
    const MessageParts parts = splitMessage(payload, 2);
    if (parts.type != "SEND" || parts.arguments.size() != 1) {
        errorMessage = "聊天消息格式应为 SEND|content";
        return false;
    }

    content = parts.arguments[0];
    if (!isValidChatContent(content)) {
        errorMessage = "聊天正文不能为空、不能包含换行，且不能超过 3000 字节";
        return false;
    }

    return true;
}

// 功能：解析服务器返回的 JOIN_OK 登录成功消息。
bool parseJoinOk(const std::string& payload,
                 std::string& username)
{
    const MessageParts parts = splitMessage(payload, 2);
    if (parts.type != "JOIN_OK" || parts.arguments.size() != 1) {
        return false;
    }

    username = parts.arguments[0];
    return true;
}

// 功能：解析 CHAT 广播消息中的序号、时间、用户名和正文。
bool parseChatBroadcast(const std::string& payload,
                        std::uint64_t& sequence,
                        std::string& timestamp,
                        std::string& username,
                        std::string& content)
{
    // CHAT 固定为五个字段，最后一段完整保留为 content。
    const MessageParts parts = splitMessage(payload, 5);
    if (parts.type != "CHAT" || parts.arguments.size() != 4) {
        return false;
    }

    if (!parseUnsignedInteger(parts.arguments[0], sequence)) {
        return false;
    }

    timestamp = parts.arguments[1];
    username = parts.arguments[2];
    content = parts.arguments[3];
    return true;
}

// 功能：解析 SYSTEM 系统消息中的时间和内容。
bool parseSystemMessage(const std::string& payload,
                        std::string& timestamp,
                        std::string& content)
{
    const MessageParts parts = splitMessage(payload, 3);
    if (parts.type != "SYSTEM" || parts.arguments.size() != 2) {
        return false;
    }

    timestamp = parts.arguments[0];
    content = parts.arguments[1];
    return true;
}

// 功能：解析 ERROR 消息中的错误码和错误说明。
bool parseErrorMessage(const std::string& payload,
                       int& code,
                       std::string& message)
{
    const MessageParts parts = splitMessage(payload, 3);
    if (parts.type != "ERROR" || parts.arguments.size() != 2) {
        return false;
    }

    std::uint64_t parsedCode = 0;
    if (!parseUnsignedInteger(parts.arguments[0], parsedCode) ||
        parsedCode > static_cast<std::uint64_t>(
            std::numeric_limits<int>::max())) {
        return false;
    }

    code = static_cast<int>(parsedCode);
    message = parts.arguments[1];
    return true;
}

}  // namespace chat
