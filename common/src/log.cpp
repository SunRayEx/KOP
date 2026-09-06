#include "kop/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace kop {

namespace {
std::atomic<LogLevel> g_level{LogLevel::Info};

const char* level_tag(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "D";
        case LogLevel::Info: return "I";
        case LogLevel::Warn: return "W";
        case LogLevel::Error: return "E";
    }
    return "?";
}
}  // namespace

void log_set_level(LogLevel level) { g_level.store(level, std::memory_order_relaxed); }

LogLevel log_level_from_env() {
    const char* e = getenv("KOP_LOG");
    if (!e) return LogLevel::Info;
    if (e[0] == 'd' || e[0] == 'D') return LogLevel::Debug;
    if (e[0] == 'i' || e[0] == 'I') return LogLevel::Info;
    if (e[0] == 'w' || e[0] == 'W') return LogLevel::Warn;
    if (e[0] == 'e' || e[0] == 'E') return LogLevel::Error;
    return LogLevel::Info;
}

void log_write(LogLevel level, const char* tag, const char* fmt, ...) {
    if (level < g_level.load(std::memory_order_relaxed)) return;

    using clock = std::chrono::system_clock;
    auto now = clock::now().time_since_epoch();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count() % 1000;

    std::tm tm{};
    localtime_r(&secs, &tm);

    char head[64];
    snprintf(head, sizeof(head), "%02d:%02d:%02d.%03d %s [%s] ", tm.tm_hour, tm.tm_min,
             tm.tm_sec, static_cast<int>(millis), level_tag(level), tag);

    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    fprintf(stderr, "%s%s\n", head, body);
}

}  // namespace kop
