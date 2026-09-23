// FFmpeg AVFrame colorimetry -> stable KOPAW frame metadata.
#pragma once

#include "kopaw_abi.h"

struct AVFrame;

namespace kopaw {

// Copies only standardized AVFrame color fields and HDR side data. The
// resulting values remain UNKNOWN when FFmpeg did not provide them; consumers
// must not infer a matrix from image dimensions.
KopawColorMetadata color_metadata_from_av_frame(const AVFrame* frame);

}  // namespace kopaw
