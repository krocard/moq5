#ifndef MOQ_MSF_MEDIA_H
#define MOQ_MSF_MEDIA_H

/*
 * MSF catalog track → media track info convenience helper.
 *
 * Derives a moq_media_track_info_t from an MSF catalog track
 * entry, handling media-type classification (role or codec),
 * packaging mapping, base64 initData decode, and CMAF init
 * segment parsing for timescale.
 *
 * When initData is decoded -- a CMAF init segment, or a RAW/LOC
 * track's initData when out_init_buf is supplied -- the caller
 * supplies an allocator for the base64-decoded buffer, which is
 * returned via out_init_buf so the caller controls its lifetime
 * (out_init->codec_config borrows from it).
 *
 * For non-CMAF tracks no allocation is performed unless the
 * catalog carries initData and out_init_buf is non-NULL.
 *
 * Does not depend on moq_playback_t.
 *
 * Link against moq::msf (which depends on moq::media-object,
 * moq::cmaf, moq::core).
 */

#include <moq/msf.h>
#include <moq/media_object.h>
#include <moq/cmaf.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Derive media track info from an MSF catalog track.
 *
 * On success, fills out_info with media type, packaging, and
 * timescale derived from the catalog entry.
 *
 * Media type: the `role` field is OPTIONAL (MSF-01 §5.2.6) and custom roles are
 * allowed, so it cannot solely gate classification:
 *   - "video" / "audio" map directly;
 *   - a reserved NON-media role (mediatimeline, eventtimeline, caption,
 *     subtitle, log, metrics) is rejected (MOQ_ERR_INVAL) even with a codec;
 *   - any other role (a custom role, or a media-like reserved role such as
 *     audiodescription / signlanguage) OR an absent role falls back to the
 *     `codec` prefix (required for audio/video tracks, §5.2.18; e.g.
 *     avc1/hev1/av01/vp09 ⇒ video, mp4a/opus ⇒ audio).
 * If neither a role nor a classifiable codec yields a media type, returns
 * MOQ_ERR_INVAL.
 *
 * CMAF with initData:
 *   Always decodes and parses the CMAF init segment to derive
 *   timescale, regardless of whether out_init is NULL.
 *
 *   If out_init is non-NULL, it is filled with codec kind,
 *   dimensions, sample rate, channel count, and codec config.
 *   out_init_buf must also be non-NULL in this case (returns
 *   MOQ_ERR_INVAL otherwise), because out_init->codec_config
 *   borrows from the decoded buffer. The caller must call
 *   moq_rcbuf_decref(*out_init_buf) when done.
 *
 *   If out_init is NULL, the decoded buffer is released
 *   internally after extracting timescale. No allocation
 *   outlives the call.
 *
 * CMAF without initData:
 *   If out_init is non-NULL, returns MOQ_ERR_INVAL — the
 *   caller asked for CMAF init details but none are available.
 *   If out_init is NULL, falls back to MSF timescale field.
 *   If neither initData nor MSF timescale exists, returns
 *   MOQ_ERR_INVAL (CMAF requires a usable timescale).
 *
 * Non-CMAF (RAW/LOC) handling:
 *   Timescale uses the MSF timescale field (or 0 if absent).
 *   A RAW/LOC track's initData is the encoder's decoder config
 *   (extradata) verbatim -- there is no container to parse. When
 *   the track has initData and out_init_buf is non-NULL, the
 *   base64 initData is decoded into *out_init_buf and (if out_init
 *   is non-NULL) out_init->codec_config is set to those bytes, so
 *   callers read decoder extradata from codec_config uniformly
 *   across packagings. The caller owns *out_init_buf
 *   (moq_rcbuf_decref when done); the other out_init fields stay
 *   zeroed. When the track has no initData, or out_init_buf is
 *   NULL, *out_init_buf stays NULL and out_init stays zeroed.
 *
 * alloc is required to decode initData (CMAF, or RAW/LOC with
 * out_init_buf). If NULL for such tracks, returns MOQ_ERR_INVAL.
 *
 * Returns MOQ_OK on success.
 * Returns MOQ_ERR_INVAL if:
 *   - track or out_info is NULL
 *   - track has a reserved non-media role (timeline/text/log/metrics)
 *   - track has neither "video"/"audio" role, nor (for any other/absent role) a
 *     classifiable audio/video codec
 *   - track has unsupported packaging
 *   - out_init != NULL but out_init_buf == NULL for CMAF with initData
 *   - out_init != NULL for CMAF without initData
 *   - CMAF without initData and without MSF timescale
 *   - alloc is NULL on an initData decode path that needs it
 *     (CMAF initData, or RAW/LOC initData with out_init_buf)
 * Returns MOQ_ERR_PROTO if CMAF initData is malformed.
 * Returns MOQ_ERR_NOMEM if base64 decode allocation fails.
 */
/*
 * NARROWED OUTPUT, and why: this entry point takes only a pointer, so it
 * cannot know how large the caller's moq_media_track_info_t is. It therefore
 * initializes and fills ONLY THE FROZEN v0 PREFIX (through `timescale`) and
 * leaves `struct_size` at MOQ_MEDIA_TRACK_INFO_V0_SIZE. That is what keeps it
 * safe for a caller compiled against the pre-version ABI.
 *
 * The consequence for a CURRENT caller: the result expresses NO transport
 * version, so moq_media_object_parse() applies the legacy standalone
 * contract -- draft-16 -- to every object parsed with it. On a draft-18
 * session that silently decodes draft-18 property bytes with the draft-16
 * integer codec, which is wrong at any value at or above 64 and invisible
 * below it.
 *
 * A current caller must therefore use moq_msf_track_to_media_info_sized()
 * and pass the version its LIVE SESSION negotiated. Assigning `struct_size`
 * and `transport_version` by hand after calling this function is NOT a
 * supported integration pattern.
 */
MOQ_API moq_result_t moq_msf_track_to_media_info(
    const moq_alloc_t *alloc,
    const moq_msf_track_t *track,
    moq_media_track_info_t *out_info,
    moq_cmaf_init_info_t *out_init,
    moq_rcbuf_t **out_init_buf);

/*
 * Sized companion: the entry point for a caller that knows its own struct
 * size and its session's negotiated transport draft.
 *
 * Identical catalog mapping to moq_msf_track_to_media_info() -- both are
 * shells over one implementation -- but out_info is prepared for a CURRENT
 * caller: at most `out_info_size` bytes are cleared and written, never more,
 * `struct_size` records what was actually written, and `transport_version`
 * is stamped with `transport_version`.
 *
 * `transport_version` is the draft the LIVE SESSION negotiated. It is the
 * authority for the object-property integer codec; a catalog describes the
 * track and never the session carrying it, so it cannot be derived here.
 * Read it from the session (moq_session_version()), not from endpoint
 * configuration or an offered preference.
 *
 * Refuses rather than downgrading:
 *   - out_info_size too small to cover `transport_version` is MOQ_ERR_INVAL,
 *     NOT a v0 result. A caller that asked for a version and cannot hold one
 *     is a caller bug, and answering it with a silent draft-16 output is the
 *     exact trap this function exists to remove;
 *   - a transport_version of 0, or any version this build does not support,
 *     is MOQ_ERR_INVAL.
 *
 * All other arguments, results and ownership rules are those of
 * moq_msf_track_to_media_info() above.
 */
MOQ_API moq_result_t moq_msf_track_to_media_info_sized(
    const moq_alloc_t *alloc,
    const moq_msf_track_t *track,
    moq_media_track_info_t *out_info,
    size_t out_info_size,
    moq_version_t transport_version,
    moq_cmaf_init_info_t *out_init,
    moq_rcbuf_t **out_init_buf);

#ifdef __cplusplus
}
#endif

#endif /* MOQ_MSF_MEDIA_H */
