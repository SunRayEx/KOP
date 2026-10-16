#include "hwaccel.hpp"

#include <cstdlib>
#include <string>
#include <vector>

#include "kop/log.h"

namespace kopaw {

static const char* kTag = "hwaccel";

static bool try_device_type(AVCodecID codec_id, AVHWDeviceType type, HwAccelConfig* out) {
    // 1) 解码器是否支持该 hw 像素格式（经 avcodec_get_hw_config）。
    // AV1/VP9 在不同 FFmpeg/驱动组合中可能没有硬件配置，必须保留软解路径。
    const AVCodec* dec = avcodec_find_decoder(codec_id);
    if (!dec) return false;
    AVPixelFormat hw_fmt = AV_PIX_FMT_NONE;
    int config_count = 0;
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(dec, i);
        if (!cfg) break;
        ++config_count;
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
            cfg->device_type == type && hw_fmt == AV_PIX_FMT_NONE) {
            hw_fmt = cfg->pix_fmt;
        }
    }
    if (hw_fmt == AV_PIX_FMT_NONE) {
        KOP_LOG_DEBUG(kTag, "%s 不支持 %s 硬解（configs=%d）",
                      dec->name, av_hwdevice_get_type_name(type), config_count);
        return false;
    }

    // 2) 创建 hw device（VAAPI 可通过 render node 覆盖默认设备；CUDA 使用
    // 空 device 字符串，让 FFmpeg 选择默认 CUDA device）。
    const char* device = nullptr;
    if (type == AV_HWDEVICE_TYPE_VAAPI) {
        device = std::getenv("KOPAW_VAAPI_DEVICE");
    } else if (type == AV_HWDEVICE_TYPE_CUDA) {
        // FFmpeg accepts a CUDA device index/name here. This makes multi-GPU
        // CUVID deployments deterministic without changing the public API.
        device = std::getenv("KOPAW_CUDA_DEVICE");
    }
    AVBufferRef* dev = nullptr;
    int rc = av_hwdevice_ctx_create(&dev, type, device, nullptr, 0);
    if (rc < 0) {
        char buf[128];
        av_strerror(rc, buf, sizeof(buf));
        KOP_LOG_INFO(kTag, "%s 设备创建失败（%s），尝试下一个后端",
                     av_hwdevice_get_type_name(type), buf);
        return false;
    }
    out->active = true;
    out->name = av_hwdevice_get_type_name(type);
    KOP_LOG_INFO(kTag, "%s: %s hw_format=%s device=%s",
                 avcodec_get_name(codec_id), out->name.c_str(),
                 av_get_pix_fmt_name(hw_fmt), device ? device : "default");
    out->type = type;
    out->hw_pix_fmt = hw_fmt;
    out->device_ref = AvBufferRef(dev);
    return true;
}

bool hw_probe(AVCodecID codec_id, HwAccelConfig* out) {
    if (!out) return false;
    *out = HwAccelConfig{};
    const char* env = getenv("KOPAW_HWACCEL");
    std::string mode = env ? env : "auto";
    if (mode == "none") {
        KOP_LOG_INFO(kTag, "KOPAW_HWACCEL=none，使用软解");
        return false;
    }

    std::vector<AVHWDeviceType> order;
    if (mode == "vaapi") {
        order = {AV_HWDEVICE_TYPE_VAAPI};
    } else if (mode == "cuda") {
        order = {AV_HWDEVICE_TYPE_CUDA};
    } else {  // auto
        order = {AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_CUDA};
    }

    for (AVHWDeviceType t : order) {
        if (t == AV_HWDEVICE_TYPE_NONE) continue;
        if (try_device_type(codec_id, t, out)) {
            KOP_LOG_INFO(kTag, "硬解后端已初始化：%s（%s）", out->name.c_str(),
                         av_hwdevice_get_type_name(t));
            return true;
        }
    }
    KOP_LOG_INFO(kTag, "无可用硬解后端，回退软解");
    return false;
}

}  // namespace kopaw
