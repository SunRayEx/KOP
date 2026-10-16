// FFmpeg AVFrame colorimetry -> stable KOPAW frame metadata.
#pragma once

#include "kopaw_abi.h"

struct AVFrame;

namespace kopaw {

// Copies standardized AVFrame color fields and HDR side data. Unspecified
// YUV range/matrix/chroma location use the pipeline defaults (limited,
// BT.709, centered); transfer and primaries remain UNKNOWN when absent.
KopawColorMetadata color_metadata_from_av_frame(const AVFrame* frame);

}  // namespace kopaw
