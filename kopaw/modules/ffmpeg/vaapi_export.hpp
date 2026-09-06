// P2 硬解零拷贝：VAAPI 解码表面 → 原生 DMA-BUF 导出。
//
// VaapiDmabufExporter 经 vaExportSurfaceHandle(DRM_PRIME_2) 取得解码表面的
// DMA-BUF 平面描述（fd/offset/stride/modifier），封装为 KOPAW_MEMORY_DMABUF 帧：
//   - drm_fourcc = DRM_FORMAT_NV12 或 DRM_FORMAT_P010，planes[] 携带两平面元数据；
//   - 表面引用由帧持有（external_ctx = AVFrame 克隆），最后一个 release 时
//     归还解码器表面池；fd 由 OwnedFrame::close_external_fds 统一关闭；
//   - 导出前 vaSyncSurface 等待解码完成（VAAPI 无 sync_file 导出，采用保证
//     数据可见性的保守路径），acquire_fence 置 NONE。
// 导出失败（驱动不支持/格式不符）返回 nullptr，由解码节点回退拉回路径。
#pragma once
#include <string>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <va/va.h>
}

#include "../frame.hpp"

namespace kopaw {

class VaapiDmabufExporter {
public:
    VaapiDmabufExporter() = default;
    ~VaapiDmabufExporter() = default;
    VaapiDmabufExporter(const VaapiDmabufExporter&) = delete;
    VaapiDmabufExporter& operator=(const VaapiDmabufExporter&) = delete;

    // display 来自解码器的 AVVAAPIDeviceContext（已由 FFmpeg vaInitialize，
    // 这里只借用；生命周期由调用方保证覆盖全部导出帧）。
    bool init(VADisplay display);
    bool inited() const { return display_ != nullptr; }

    // hw 帧必须是 AV_PIX_FMT_VAAPI 且表面格式为 NV12 或 P010。返回的帧引用归零时：
    // 关闭 DMA-BUF fd → av_frame_free 归还表面。失败返回 nullptr。
    OwnedFrame* export_frame(const AVFrame* hw_frame, std::string* error);

private:
    VADisplay display_ = nullptr;
    bool sync_warned_ = false;
};

}  // namespace kopaw
