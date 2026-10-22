#pragma once

#include <winsock2.h>

#include <cstdint>
#include <string>

namespace chat {

// 单帧正文上限，防止异常长度导致无界内存分配。
constexpr std::uint32_t kMaxFrameBytes = 4096;

// 区分“正常结束”“连接异常”和“成功”，避免把 EOF 当成错误。
enum class IoStatus {
    Success,
    Closed,
    Error
};

// 功能：初始化 Windows Socket 环境。
bool initializeNetwork(std::string& errorMessage);

// 功能：释放 Windows Socket 环境资源。
void cleanupNetwork();

// 功能：返回 Socket 错误码对应的可读文字。
std::string socketErrorMessage(int errorCode);

// 功能：循环发送，直到指定字节数全部发送完成。
bool sendAll(SOCKET socketValue, const char* data, int length);

// 功能：循环接收，直到指定字节数全部接收完成。
IoStatus receiveExactly(SOCKET socketValue, char* data, int length);

// 功能：发送一条“长度 + 正文”格式的完整消息帧。
bool sendFrame(SOCKET socketValue, const std::string& payload);

// 功能：接收并返回一条完整消息帧。
IoStatus receiveFrame(SOCKET socketValue, std::string& payload);

// 功能：将控制台切换为 UTF-8 编码。
void configureConsoleUtf8();

// 功能：线程安全地输出一行日志。
void logLine(const std::string& text);

}  // namespace chat
