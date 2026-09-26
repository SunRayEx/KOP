#include "color_metadata.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

#include "ffmpeg.hpp"

namespace kopaw {
namespace {

uint32_t map_range(AVColorRange range) {
    switch (range) {
        case AVCOL_RANGE_MPEG:
            return KOPAW_COLOR_RANGE_LIMITED;
        case AVCOL_RANGE_JPEG:
            return KOPAW_COLOR_RANGE_FULL;
        default:
            return KOPAW_COLOR_RANGE_UNKNOWN;
    }
}

uint32_t map_matrix(AVColorSpace colorspace) {
    switch (colorspace) {
        case AVCOL_SPC_BT709:
            return KOPAW_COLOR_MATRIX_BT709;
        case AVCOL_SPC_FCC:
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
        case AVCOL_SPC_SMPTE240M:
            return KOPAW_COLOR_MATRIX_BT601;
        case AVCOL_SPC_BT2020_NCL:
            return KOPAW_COLOR_MATRIX_BT2020_NCL;
        case AVCOL_SPC_BT2020_CL:
            return KOPAW_COLOR_MATRIX_BT2020_CL;
        default:
            return KOPAW_COLOR_MATRIX_UNKNOWN;
    }
}

uint32_t map_transfer(AVColorTransferCharacteristic transfer) {
    switch (transfer) {
        case AVCOL_TRC_BT709:
        case AVCOL_TRC_SMPTE170M:
        case AVCOL_TRC_SMPTE240M:
            return KOPAW_COLOR_TRANSFER_BT709;
        case AVCOL_TRC_IEC61966_2_1:
            return KOPAW_COLOR_TRANSFER_SRGB;
        case AVCOL_TRC_GAMMA22:
            return KOPAW_COLOR_TRANSFER_GAMMA22;
        case AVCOL_TRC_SMPTE2084:
            return KOPAW_COLOR_TRANSFER_PQ;
        case AVCOL_TRC_ARIB_STD_B67:
            return KOPAW_COLOR_TRANSFER_HLG;
        default:
            return KOPAW_COLOR_TRANSFER_UNKNOWN;
    }
}

uint32_t map_primaries(AVColorPrimaries primaries) {
    switch (primaries) {
        case AVCOL_PRI_BT709:
            return KOPAW_COLOR_PRIMARIES_BT709;
        case AVCOL_PRI_BT470M:
        case AVCOL_PRI_BT470BG:
        case AVCOL_PRI_SMPTE170M:
        case AVCOL_PRI_SMPTE240M:
            return KOPAW_COLOR_PRIMARIES_BT601;
        case AVCOL_PRI_BT2020:
            return KOPAW_COLOR_PRIMARIES_BT2020;
        case AVCOL_PRI_SMPTE432:
            return KOPAW_COLOR_PRIMARIES_P3;
        default:
            return KOPAW_COLOR_PRIMARIES_UNKNOWN;
    }
}

uint32_t map_chroma_location(AVChromaLocation location) {
    switch (location) {
        case AVCHROMA_LOC_LEFT:
            return KOPAW_CHROMA_LOCATION_LEFT;
        case AVCHROMA_LOC_CENTER:
            return KOPAW_CHROMA_LOCATION_CENTER;
        case AVCHROMA_LOC_TOPLEFT:
            return KOPAW_CHROMA_LOCATION_TOPLEFT;
        case AVCHROMA_LOC_TOP:
            return KOPAW_CHROMA_LOCATION_TOP;
        case AVCHROMA_LOC_BOTTOMLEFT:
            return KOPAW_CHROMA_LOCATION_BOTTOMLEFT;
        case AVCHROMA_LOC_BOTTOM:
            return KOPAW_CHROMA_LOCATION_BOTTOM;
        default:
            return KOPAW_CHROMA_LOCATION_UNKNOWN;
    }
}

uint32_t rational_to_u32(AVRational value, long double scale) {
    if (value.num < 0 || value.den <= 0) return 0;
    const long double scaled =
        static_cast<long double>(value.num) * scale / value.den;
    if (!std::isfinite(static_cast<double>(scaled))) return 0;
    if (scaled >= std::numeric_limits<uint32_t>::max()) {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(std::llround(scaled));
}

void copy_mastering_display(const AVFrame* frame, KopawColorMetadata* color) {
    const AVFrameSideData* side_data = av_frame_get_side_data(
        frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (!side_data || side_data->size < sizeof(AVMasteringDisplayMetadata)) return;

    const auto* metadata =
        reinterpret_cast<const AVMasteringDisplayMetadata*>(side_data->data);
    if (metadata->has_primaries) {
        for (size_t primary = 0; primary < 3; ++primary) {
            for (size_t coordinate = 0; coordinate < 2; ++coordinate) {
                color->hdr.display_primaries[primary * 2 + coordinate] =
                    rational_to_u32(metadata->display_primaries[primary][coordinate],
                                    100000.0L);
            }
        }
        for (size_t coordinate = 0; coordinate < 2; ++coordinate) {
            color->hdr.white_point[coordinate] =
                rational_to_u32(metadata->white_point[coordinate], 100000.0L);
        }
        color->hdr.flags |= KOPAW_HDR_FLAG_MASTERING_DISPLAY;
    }
    if (metadata->has_luminance) {
        color->hdr.max_luminance =
            rational_to_u32(metadata->max_luminance, 1000.0L);
        color->hdr.min_luminance =
            rational_to_u32(metadata->min_luminance, 1000.0L);
        color->hdr.flags |= KOPAW_HDR_FLAG_MASTERING_DISPLAY;
    }
}

void copy_content_light(const AVFrame* frame, KopawColorMetadata* color) {
    const AVFrameSideData* side_data =
        av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (!side_data || side_data->size < sizeof(AVContentLightMetadata)) return;
    const auto* metadata =
        reinterpret_cast<const AVContentLightMetadata*>(side_data->data);
    color->hdr.max_cll = metadata->MaxCLL;
    color->hdr.max_fall = metadata->MaxFALL;
    color->hdr.flags |= KOPAW_HDR_FLAG_CONTENT_LIGHT;
}

}  // namespace

KopawColorMetadata color_metadata_from_av_frame(const AVFrame* frame) {
    KopawColorMetadata color{};
    if (!frame) return color;
    color.range = map_range(frame->color_range);
    color.matrix = map_matrix(frame->colorspace);
    color.transfer = map_transfer(frame->color_trc);
    color.primaries = map_primaries(frame->color_primaries);
    color.chroma_location = map_chroma_location(frame->chroma_location);
    copy_mastering_display(frame, &color);
    copy_content_light(frame, &color);
    return color;
}

}  // namespace kopaw
