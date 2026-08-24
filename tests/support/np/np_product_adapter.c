#include "np_product_adapter.h"

#include <moq/loc.h>
#include <moq/rcbuf.h>

#include <string.h>

size_t np_adapter_encode_loc01(moq_version_t transport_version,
                               const np_adapter_loc01_t *in,
                               uint8_t *out, size_t cap)
{
    if (!in || !out) return 0;

    moq_loc_headers_t h;
    moq_loc_headers_init(&h);
    h.has_timestamp = in->has_capture_timestamp;
    h.timestamp = in->capture_timestamp;
    if (in->has_video_frame_marking) {
        h.has_video_frame_marking = true;
        h.video_frame_marking.start_of_frame  = in->vfm_start_of_frame;
        h.video_frame_marking.end_of_frame    = in->vfm_end_of_frame;
        h.video_frame_marking.independent     = in->vfm_independent;
        h.video_frame_marking.discardable     = in->vfm_discardable;
        h.video_frame_marking.base_layer_sync = in->vfm_base_layer_sync;
        h.video_frame_marking.temporal_id     = in->vfm_temporal_id;
        h.video_frame_marking.has_layer_id    = in->vfm_has_layer_id;
        h.video_frame_marking.layer_id        = in->vfm_layer_id;
    }

    moq_rcbuf_t *buf = NULL;
    if (moq_loc_encode(moq_alloc_default(), transport_version,
                       MOQ_LOC_PROFILE_01, &h, &buf) != MOQ_OK)
        return 0;
    if (!buf) return 0;   /* nothing present: not a block this role emits */

    size_t n = moq_rcbuf_len(buf);
    if (n > cap) {         /* refuse rather than truncate */
        moq_rcbuf_decref(buf);
        return 0;
    }
    memcpy(out, moq_rcbuf_data(buf), n);
    moq_rcbuf_decref(buf);
    return n;
}

bool np_adapter_parse_loc01(moq_version_t transport_version,
                            const uint8_t *buf, size_t len,
                            np_adapter_loc01_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    moq_loc_headers_t h;
    moq_bytes_t span = { buf, len };
    if (moq_loc_parse(transport_version, MOQ_LOC_PROFILE_01, span, &h)
        != MOQ_OK)
        return false;

    out->has_capture_timestamp = h.has_timestamp;
    out->capture_timestamp = h.timestamp;
    out->has_video_frame_marking = h.has_video_frame_marking;
    if (h.has_video_frame_marking) {
        out->vfm_start_of_frame  = h.video_frame_marking.start_of_frame;
        out->vfm_end_of_frame    = h.video_frame_marking.end_of_frame;
        out->vfm_independent     = h.video_frame_marking.independent;
        out->vfm_discardable     = h.video_frame_marking.discardable;
        out->vfm_base_layer_sync = h.video_frame_marking.base_layer_sync;
        out->vfm_temporal_id     = h.video_frame_marking.temporal_id;
        out->vfm_has_layer_id    = h.video_frame_marking.has_layer_id;
        out->vfm_layer_id        = h.video_frame_marking.layer_id;
    }
    return true;
}
