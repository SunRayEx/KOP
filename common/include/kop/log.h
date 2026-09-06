// KOP 公共库：分级日志（线程安全，输出到 stderr）
#pragma once
#include <cstdarg>

namespace kop {

enum class LogLevel : int {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
};

// KOP_LOG 环境变量控制级别（debug/info/warn/error），默认 info。
void log_set_level(LogLevel level);
LogLevel log_level_from_env();

void log_write(LogLevel level, const char* tag, const char* fmt, ...) \
    __attribute__((format(printf, 3, 4)));

}  // namespace kop

#define KOP_LOG_DEBUG(tag, ...) ::kop::log_write(::kop::LogLevel::Debug, tag, __VA_ARGS__)
#define KOP_LOG_INFO(tag, ...)  ::kop::log_write(::kop::LogLevel::Info,  tag, __VA_ARGS__)
#define KOP_LOG_WARN(tag, ...)  ::kop::log_write(::kop::LogLevel::Warn,  tag, __VA_ARGS__)
#define KOP_LOG_ERROR(tag, ...) ::kop::log_write(::kop::LogLevel::Error, tag, __VA_ARGS__)
