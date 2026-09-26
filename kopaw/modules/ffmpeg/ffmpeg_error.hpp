// FFmpeg 错误处理统一层：错误字符串 + 状态码映射。
//
// 此前各节点直接用 av_strerror 或 (int)rc 打日志，错误分类散落；本层把
// FFmpeg 返回值统一成 kopaw::KopavStatus，并把 AVERROR_EOF / EAGAIN 与
// 真正的失败区分开——这两者在管线里语义完全不同（前者收尾，后者重试），
// 混淆会导致解码循环提前结束或空转。
#pragma once

#include <cstdio>
#include <string>

#include "ffmpeg.hpp"

namespace kopaw {

// 把 FFmpeg 错误码格式化为可读文本。rc>=0 时返回 "ok"。
// 返回值引用稳定：内部用线程局部缓冲，可安全 %s 打印。
inline const char* av_error_string(int rc) {
    if (rc >= 0) return "ok";
    static thread_local char buf[AV_ERROR_MAX_STRING_SIZE];
    buf[0] = '\0';
    av_strerror(rc, buf, sizeof(buf));
    if (buf[0] == '\0') {
        std::snprintf(buf, sizeof(buf), "rc=%d", rc);
    }
    return buf;
}

// 分类 FFmpeg 返回码。rc >= 0 视为 Ok。
inline KopavStatus av_status(int rc) {
    if (rc >= 0) return KopavStatus::Ok;
    if (rc == AVERROR(EAGAIN)) return KopavStatus::EAgain;
    if (rc == AVERROR_EOF) return KopavStatus::Eof;
    if (rc == AVERROR(EINVAL)) return KopavStatus::InvalidArg;
    if (rc == AVERROR(ENOMEM)) return KopavStatus::NoMemory;
    return KopavStatus::Unsupported;
}

// 便捷判定：解码器 send/receive 的“正常需要重试”返回值。
inline bool av_is_retry(int rc) {
    return rc == AVERROR(EAGAIN) || rc == AVERROR_EOF;
}

// 日志友好的错误消息拼接：<context>: <av_strerror> (rc=<n>)。
inline std::string av_error_msg(const std::string& context, int rc) {
    char buf[AV_ERROR_MAX_STRING_SIZE];
    buf[0] = '\0';
    av_strerror(rc, buf, sizeof(buf));
    return context + ": " + buf + " (rc=" + std::to_string(rc) + ")";
}

}  // namespace kopaw
