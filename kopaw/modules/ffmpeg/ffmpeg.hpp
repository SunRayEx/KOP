// KOPAW FFmpeg 封装层 —— 统一入口头。
//
// 全项目对 libav* 的引用只经过本头，理由：
//   1. FFmpeg 是 C 库，C++ 使用方必须自行 extern "C" 包裹（FFmpeg 8 起
//      公共头不再自带保护）。此前每个节点各自重复包裹，漏一处即链接错误；
//      现在只在本文包裹一次。
//   2. __STDC_CONSTANT_MACROS / __STDC_LIMIT_MACROS 在部分平台仍是 C++
//      包含 libavutil 的前置要求，统一在此定义。
//   3. 版本差异集中在 ffmpeg_compat.hpp 处理，业务代码不直接 #if 版本宏。
//
// 禁止在别处出现 #include <libav...>。需要 FFmpeg 类型的头文件改为
// #include "ffmpeg.hpp"（本头 deliberately 是 umbrella，编译开销可接受）。
#pragma once

// 必须在包含 libav 头之前定义（部分标准库实现的 inttypes.h 门控）。
#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif
#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

// 各库版本宏（version.h）：用于兼容层与能力检测。
#include <libavcodec/version.h>
#include <libavfilter/version.h>
#include <libavformat/version.h>
#include <libavutil/version.h>
#include <libswresample/version.h>
#include <libswscale/version.h>
}

// 错误映射（av_error_string / av_status）：与 libav 头同属统一入口。
// KopavStatus 需先于 include 定义——ffmpeg_error.hpp 的函数签名要用它。
namespace kopaw {

// FFmpeg 错误码到 KOPAW 状态码的统一映射（见 ffmpeg_error.hpp）。
enum class KopavStatus {
    Ok = 0,
    EAgain,        // AVERROR(EAGAIN)：需要更多输入/输出
    Eof,           // AVERROR_EOF
    InvalidArg,    // AVERROR(EINVAL)
    NoMemory,      // AVERROR(ENOMEM)
    Unsupported,   // 其余不可恢复错误（编解码器/格式不支持等）
};

}  // namespace kopaw

#include "ffmpeg_error.hpp"

// KOPAW 支持的 FFmpeg 版本窗口。低于此版本时兼容层尽力而为（见
// ffmpeg_compat.hpp），CI 矩阵只覆盖窗口内的版本。
#define KOPAV_MIN_LIBAVUTIL_MAJOR 56  // FFmpeg 4.4（Ubuntu 22.04）
#define KOPAV_MIN_LIBAVUTIL_MINOR 0
