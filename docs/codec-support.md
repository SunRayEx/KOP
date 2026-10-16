# KOPAW 编解码与硬解路径

KOPAW 的视频解码节点按 `AVCodecID` 打开 FFmpeg 原生 decoder，因此 HEVC10、HEVC、AVC、VP9 和 AV1 使用同一条解码管线，不需要为每种编码增加独立节点。

## 播放策略

`KOPAW_HWACCEL=auto`（默认）按 VAAPI -> CUDA 探测硬件设备，并检查当前 codec 的 `AVCodecHWConfig`。如果设备创建、codec 打开或 `get_format` 协商失败，自动回退软件解码。`none` 强制软件解码，`vaapi` 和 `cuda` 强制只尝试指定后端。

可选设备变量：

- `KOPAW_VAAPI_DEVICE=/dev/dri/renderD128`：指定 VAAPI render node。
- `KOPAW_CUDA_DEVICE=0`：指定 CUDA/CUVID 使用的 device index 或名称。
- `KOPAW_ZERO_COPY=0`：关闭 VAAPI DMA-BUF 原生输出，但不关闭硬件解码。

硬解实际选择的像素格式会记录在 `KOP_LOG=info|debug` 日志中。硬件格式不在 FFmpeg 提供的候选列表中时，KOPAW 只选择明确的软件格式，不会把另一种硬件格式误当作软件帧。

## 零拷贝边界

VAAPI 原生输出目前只接受单对象、双平面的 NV12/P010 DMA-BUF，并由 Vulkan YCbCr 导入。VAAPI 驱动导出多对象布局、modifier、平面步长、色彩元数据或 Vulkan 外部内存能力不匹配时，播放器关闭后续原生输出并继续使用 CPU RGBA 路径。该降级不影响 VAAPI/CUDA 硬件解码本身。

CUDA/CUVID 解码路径可用：KOPAW 会按具体 codec 检查 FFmpeg 的 CUDA 配置，创建 CUDA device，并接收 CUDA 硬件帧后拉回系统内存，再由 swscale 转换。该路径支持 AVC/H.264、HEVC/H.265，并在 FFmpeg/驱动提供对应配置时支持 AV1、VP9。CUDA-EGL/DMA-BUF 原生导出尚未宣称支持，因此 CUDA 解码当前使用系统内存输出；不能把 CUDA 硬解能力等同于 Vulkan YCbCr 零拷贝能力。实际可用性仍取决于 NVIDIA 驱动、GPU 解码能力和 FFmpeg 构建。

## 编码

`kopaw-transcode --vc` 支持直接传 FFmpeg encoder name，也提供稳定别名：

- `av1`：优先 `libsvtav1`，然后 `libaom-av1`、`av1`。
- `vp9`：优先 `libvpx-vp9`，然后 `vp9`。
- `hevc`：优先 `libx265`，然后 `hevc`。
- `h264`/`avc`：优先 `libx264`，然后 `h264`。

编码器是否可用仍取决于本机 FFmpeg 构建。硬件编码器可通过完整名称传入，例如 `av1_nvenc`、`hevc_nvenc`、`av1_vaapi` 或 `vp9_vaapi`，但对应硬件、驱动、像素格式和输出容器必须自行满足。

示例：

```text
KOPAW_HWACCEL=auto KOP_LOG=info kopaw-player input.mkv --backend vulkan
KOPAW_HWACCEL=cuda KOPAW_CUDA_DEVICE=0 kopaw-player input.mkv --no-audio
kopaw-transcode input.mkv output.webm --vc vp9 --ac libopus
kopaw-transcode input.mkv output.mkv --vc av1 --ac copy
```

## 验证建议

先查看本机能力：

```text
ffmpeg -decoders | grep -E 'av1|vp9|hevc|h264'
ffmpeg -hwaccels
ffmpeg -encoders | grep -E 'av1|vp9|hevc|h264'
```

日志中应区分以下状态：软件解码、硬件上下文已创建但实际格式协商失败、硬件帧实际生效、以及原生 DMA-BUF 导入失败后的 CPU 回退。真实 VAAPI/CUDA 能力仍需在目标设备上用对应媒体样本验证。
