/*****************************************************************************
 * video.c: video decoder using the libavcodec library
 *****************************************************************************
 * Copyright (C) 1999-2001 VLC authors and VideoLAN
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Gildas Bazin <gbazin@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdatomic.h>

#include <vlc_common.h>
#include <vlc_codec.h>
#include <vlc_avcodec.h>
#include <vlc_cpu.h>
#include <vlc_ancillary.h>
#include <assert.h>

#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/mastering_display_metadata.h>

#include "avcodec.h"
#include "va.h"

#if defined(_WIN32)
# include <winapifamily.h>
#endif

#include <libavutil/stereo3d.h>

#if LIBAVUTIL_VERSION_CHECK( 57, 16, 100 )
# include <libavutil/dovi_meta.h>
#endif

#include <libavutil/hdr_dynamic_metadata.h>

#include "../../packetizer/av1_obu.h"
#include "../../packetizer/av1.h"
#include "../cc.h"

#define OPAQUE_REF_ONLY LIBAVCODEC_VERSION_CHECK( 59, 63, 100 )

struct frame_info_s
{
#if OPAQUE_REF_ONLY
    uint64_t i_sequence_number;
#endif
    bool b_eos;
    bool b_display;
};

/*****************************************************************************
 * decoder_sys_t : decoder descriptor
 *****************************************************************************/
typedef struct
{
    AVCodecContext *p_context;
    const AVCodec  *p_codec;

    /* Video decoder specific part */
    date_t  pts; /* Protected by lock */

    /* Closed captions for decoders */
    cc_data_t cc;

    /* for frame skipping algo */
    bool b_hurry_up;
    bool b_show_corrupted;
    bool b_from_preroll;
    bool b_hardware_only;
    enum AVDiscard i_skip_frame;

#if OPAQUE_REF_ONLY
    uint64_t i_next_sequence_number;
#else
# define FRAME_INFO_DEPTH 64
    struct frame_info_s frame_info[FRAME_INFO_DEPTH];
#endif

    enum
    {
        FRAMEDROP_NONE,
        FRAMEDROP_NONREF,
        FRAMEDROP_AGGRESSIVE_RECOVER,
    } framedrop;
    /* how many decoded frames are late */
    int     i_late_frames;
    int64_t i_last_output_frame;
    vlc_tick_t i_last_late_delay;

    /* for direct rendering */
    bool        b_direct_rendering;
    bool        b_dr_failure; /* Protected by lock */

    /* Hack to force display of still pictures */
    bool b_first_frame;


    /* */
    bool palette_sent;

    /* VA API */
    vlc_va_t *p_va;
    enum AVPixelFormat pix_fmt;
    int profile;
    int level;
    vlc_video_context *vctx_out;
    bool use_hwframes;

    // decoder output seen by lavc, regardless of texture padding
    unsigned decoder_width;
    unsigned decoder_height;

    /* Protect dec->fmt_out, decoder_Update*() and decoder_NewPicture()
     * functions from lavc_GetFrame(). ffmpeg_GetFormat() doesn't need locking
     * as ffmpeg is taking care of the synchronisation. */
    vlc_mutex_t lock;
} decoder_sys_t;

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static void ffmpeg_InitCodec      ( decoder_t * );
static int lavc_GetFrame(struct AVCodecContext *, AVFrame *, int);
static enum AVPixelFormat ffmpeg_GetFormat( AVCodecContext *,
                                          const enum AVPixelFormat * );
static int  DecodeVideo( decoder_t *, block_t * );
static void Flush( decoder_t * );

static uint32_t ffmpeg_CodecTag( vlc_fourcc_t fcc )
{
    uint8_t *p = (uint8_t*)&fcc;
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

/*****************************************************************************
 * Local Functions
 *****************************************************************************/

static void FrameInfoInit( decoder_sys_t *p_sys )
{
#if OPAQUE_REF_ONLY
    p_sys->i_next_sequence_number = 0;
#else
    AVCodecContext *p_context = p_sys->p_context;
    p_context->reordered_opaque = 0;
#endif
}

static struct frame_info_s * FrameInfoGet( decoder_sys_t *p_sys, AVFrame *frame )
{
#if OPAQUE_REF_ONLY
    (void)p_sys;
    /* There's no pkt to frame opaque mapping guarantee */
    return (struct frame_info_s *) frame->opaque_ref->data;
#else
    return &p_sys->frame_info[frame->reordered_opaque % FRAME_INFO_DEPTH];
#endif
}

static struct frame_info_s * FrameInfoAdd( decoder_sys_t *p_sys, AVPacket *pkt )
{
#if OPAQUE_REF_ONLY
    AVBufferRef *bufref = av_buffer_allocz(sizeof(struct frame_info_s));
    if( !bufref )
        return NULL;
    pkt->opaque_ref = bufref;

    struct frame_info_s *p_frame_info = (struct frame_info_s *) bufref->data;
    p_frame_info->i_sequence_number = p_sys->i_next_sequence_number++;
    return p_frame_info;
#else
    VLC_UNUSED(pkt);
    AVCodecContext *p_context = p_sys->p_context;
    return &p_sys->frame_info[p_context->reordered_opaque++ % FRAME_INFO_DEPTH];
#endif
}

static bool FrameCanStoreInfo( const AVFrame *frame )
{
#if OPAQUE_REF_ONLY
    return !!frame->opaque_ref;
#else
    return true;
#endif
}

static void FrameSetPicture( AVFrame *frame, picture_t *pic )
{
    frame->opaque = pic;
}

static picture_t * FrameGetPicture( AVFrame *frame )
{
    return frame->opaque;
}

static int64_t NextPktSequenceNumber( decoder_sys_t *p_sys )
{
#if OPAQUE_REF_ONLY
    return p_sys->i_next_sequence_number;
#else
    AVCodecContext *p_context = p_sys->p_context;
    return p_context->reordered_opaque;
#endif
}

static int64_t FrameSequenceNumber( const AVFrame *frame, const struct frame_info_s *info )
{
#if OPAQUE_REF_ONLY
    VLC_UNUSED(frame);
    return info->i_sequence_number;
#else
    VLC_UNUSED(info);
    return frame->reordered_opaque;
#endif
}

static void lavc_Frame8PaletteCopy( video_palette_t *dst, const uint8_t *src )
{
    // (A << 24) | (R << 16) | (G << 8) | B
    // stored in host endianness
    const uint8_t *srcp = src;
    for(size_t i=0; i<AVPALETTE_COUNT; i++)
    {
        // we want RGBA byte order storage
#ifdef WORDS_BIGENDIAN
        // AV mem is ARGB in byte order
        dst->palette[i][0] = srcp[1];
        dst->palette[i][1] = srcp[2];
        dst->palette[i][2] = srcp[3];
        dst->palette[i][3] = srcp[0];
#else
        // AV mem is BGRA in byte order
        dst->palette[i][0] = srcp[2];
        dst->palette[i][1] = srcp[1];
        dst->palette[i][2] = srcp[0];
        dst->palette[i][3] = srcp[3];
#endif
        srcp += sizeof(dst->palette[0]);
    }

    dst->i_entries = AVPALETTE_COUNT;
}

/**
 * Sets the decoder output format.
 */
static int lavc_GetVideoFormat(decoder_t *dec, video_format_t *restrict fmt,
                               AVCodecContext *ctx, enum AVPixelFormat pix_fmt,
                               enum AVPixelFormat sw_pix_fmt)
{
    int width = ctx->coded_width;
    int height = ctx->coded_height;

    video_format_Init(fmt, 0);

    if (pix_fmt == sw_pix_fmt)
    {   /* software decoding */
        int aligns[AV_NUM_DATA_POINTERS];

        if (GetVlcChroma(fmt, pix_fmt))
            return -1;

        /* The libavcodec palette can only be fetched when the first output
         * frame is decoded. Assume that the current chroma is RGB32 while we
         * are waiting for a valid palette. Indeed, fmt_out.video.p_palette
         * doesn't trigger a new vout request, but a new chroma yes. */
        if (pix_fmt == AV_PIX_FMT_PAL8 && !dec->fmt_out.video.p_palette)
            fmt->i_chroma = VLC_CODEC_XRGB;

        avcodec_align_dimensions2(ctx, &width, &height, aligns);
    }

    if( width == 0 || height == 0 || width > 8192 || height > 8192 ||
        width < ctx->width || height < ctx->height )
    {
        msg_Err(dec, "Invalid frame size %dx%d vsz %dx%d",
                     width, height, ctx->width, ctx->height );
        video_format_Clean(fmt);
        return -1; /* invalid display size */
    }

    fmt->i_width = width;
    fmt->i_height = height;
    if ( dec->fmt_in->video.i_visible_width != 0 &&
         dec->fmt_in->video.i_visible_width <= (unsigned)ctx->width &&
         dec->fmt_in->video.i_visible_height != 0 &&
         dec->fmt_in->video.i_visible_height <= (unsigned)ctx->height )
    {
        /* the demuxer/packetizer provided crop info that are lost in lavc */
        fmt->i_visible_width  = dec->fmt_in->video.i_visible_width;
        fmt->i_visible_height = dec->fmt_in->video.i_visible_height;
        fmt->i_x_offset       = dec->fmt_in->video.i_x_offset;
        fmt->i_y_offset       = dec->fmt_in->video.i_y_offset;
    }
    else
    {
        fmt->i_visible_width = ctx->width;
        fmt->i_visible_height = ctx->height;
        fmt->i_x_offset       = 0;
        fmt->i_y_offset       = 0;
    }

    /* If an aspect-ratio was specified in the input format then force it */
    if (dec->fmt_in->video.i_sar_num > 0 && dec->fmt_in->video.i_sar_den > 0)
    {
        fmt->i_sar_num = dec->fmt_in->video.i_sar_num;
        fmt->i_sar_den = dec->fmt_in->video.i_sar_den;
    }
    else
    {
        fmt->i_sar_num = ctx->sample_aspect_ratio.num;
        fmt->i_sar_den = ctx->sample_aspect_ratio.den;

        if (fmt->i_sar_num == 0 || fmt->i_sar_den == 0)
            fmt->i_sar_num = fmt->i_sar_den = 1;
    }

    if (dec->fmt_in->video.i_frame_rate > 0
     && dec->fmt_in->video.i_frame_rate_base > 0)
    {
        fmt->i_frame_rate = dec->fmt_in->video.i_frame_rate;
        fmt->i_frame_rate_base = dec->fmt_in->video.i_frame_rate_base;
    }
    else if (ctx->framerate.num > 0 && ctx->framerate.den > 0)
    {
        fmt->i_frame_rate = ctx->framerate.num;
        fmt->i_frame_rate_base = ctx->framerate.den;
    }
    else if (ctx->time_base.num > 0 && ctx->time_base.den > 0)
    {
        fmt->i_frame_rate = ctx->time_base.den;
        fmt->i_frame_rate_base = ctx->time_base.num;
    }

    get_video_color_settings(ctx, fmt);
    return 0;
}

static int lavc_UpdateVideoFormat(decoder_t *dec, AVCodecContext *ctx,
                                  enum AVPixelFormat fmt,
                                  enum AVPixelFormat swfmt)
{
    video_format_t fmt_out;
    int val;

    val = lavc_GetVideoFormat(dec, &fmt_out, ctx, fmt, swfmt);
    if (val)
        return val;

    decoder_sys_t *p_sys = dec->p_sys;

    /* always have date in fields/ticks units */
    if(p_sys->pts.i_divider_num)
        date_Change(&p_sys->pts, fmt_out.i_frame_rate,
                                 fmt_out.i_frame_rate_base);
    else
        date_Init(&p_sys->pts, fmt_out.i_frame_rate,
                               fmt_out.i_frame_rate_base);

    fmt_out.p_palette = dec->fmt_out.video.p_palette;
    dec->fmt_out.video.p_palette = NULL;

    vlc_fourcc_t i_chroma;
    if (fmt == swfmt)
        i_chroma = fmt_out.i_chroma;
    else
        i_chroma = 0;
    es_format_Change(&dec->fmt_out, VIDEO_ES, i_chroma);
    dec->fmt_out.video = fmt_out;
    dec->fmt_out.video.i_chroma = i_chroma;
    dec->fmt_out.video.orientation = dec->fmt_in->video.orientation;
    dec->fmt_out.video.projection_mode = dec->fmt_in->video.projection_mode;
    dec->fmt_out.video.multiview_mode = dec->fmt_in->video.multiview_mode;
    dec->fmt_out.video.pose = dec->fmt_in->video.pose;
    if ( dec->fmt_in->video.mastering.max_luminance )
        dec->fmt_out.video.mastering = dec->fmt_in->video.mastering;
    if ( dec->fmt_in->video.dovi.version_major )
        dec->fmt_out.video.dovi = dec->fmt_in->video.dovi;
    dec->fmt_out.video.lighting = dec->fmt_in->video.lighting;
    p_sys->decoder_width  = dec->fmt_out.video.i_width;
    p_sys->decoder_height = dec->fmt_out.video.i_height;

    return 0;
}

static bool chroma_compatible(const video_format_t *a, const video_format_t *b)
{
    if (a->i_chroma != b->i_chroma)
        return false;

    if (a->color_range != b->color_range && a->color_range != COLOR_RANGE_UNDEF)
        return false;

    return true;
}

/**
 * Copies a picture from the libavcodec-allocate buffer to a picture_t.
 * This is used when not in direct rendering mode.
 */
static int lavc_CopyPicture(decoder_t *dec, picture_t *pic, AVFrame *frame)
{
    decoder_sys_t *sys = dec->p_sys;

    video_format_t test_chroma;
    video_format_Init(&test_chroma, 0);
    if (GetVlcChroma(&test_chroma, frame->format) != VLC_SUCCESS)
    {
        const char *name = av_get_pix_fmt_name(frame->format);

        msg_Err(dec, "Unsupported decoded output format %d (%s)",
                sys->p_context->pix_fmt, (name != NULL) ? name : "unknown");
        return VLC_EGENERIC;
    } else if (!chroma_compatible(&test_chroma, &pic->format)
     /* ensure we never read more than dst lines/pixels from src */
     || frame->width != (int) pic->format.i_visible_width
     || frame->height < (int) pic->format.i_visible_height)
    {
        msg_Warn(dec, "dropping frame because the vout changed");
        return VLC_EGENERIC;
    }

    for (int plane = 0; plane < pic->i_planes; plane++)
    {
        const uint8_t *src = frame->data[plane];
        uint8_t *dst = pic->p[plane].p_pixels;
        size_t src_stride = frame->linesize[plane];
        size_t dst_stride = pic->p[plane].i_pitch;
        size_t size = __MIN(src_stride, dst_stride);

        for (int line = 0; line < pic->p[plane].i_visible_lines; line++)
        {
            memcpy(dst, src, size);
            src += src_stride;
            dst += dst_stride;
        }
    }
    return VLC_SUCCESS;
}

static int OpenVideoCodec( decoder_t *p_dec )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    AVCodecContext *ctx = p_sys->p_context;
    const AVCodec *codec = p_sys->p_codec;
    int ret;

    if( ctx->extradata_size <= 0 )
    {
        if( codec->id == AV_CODEC_ID_VC1 ||
            codec->id == AV_CODEC_ID_THEORA )
        {
            msg_Warn( p_dec, "waiting for extra data for codec %s",
                      codec->name );
            return 1;
        }
    }

    ctx->width  = p_dec->fmt_in->video.i_visible_width;
    ctx->height = p_dec->fmt_in->video.i_visible_height;

    if (!ctx->coded_width || !ctx->coded_height)
    {
        ctx->coded_width = p_dec->fmt_in->video.i_width;
        ctx->coded_height = p_dec->fmt_in->video.i_height;
    }

    ctx->bits_per_coded_sample = vlc_fourcc_GetChromaBPP(p_dec->fmt_in->video.i_chroma);
    if ( ctx->bits_per_coded_sample == 0 &&
         p_dec->fmt_in->i_profile == -1 && p_dec->fmt_in->i_level != -1 )
        // HACK: the bits per sample was stored in the i_level
        ctx->bits_per_coded_sample = p_dec->fmt_in->i_level;
    p_sys->pix_fmt = AV_PIX_FMT_NONE;
    cc_Init( &p_sys->cc );

    set_video_color_settings( &p_dec->fmt_in->video, ctx );
    if( var_InheritBool(p_dec, "low-delay") ||
      ( p_dec->fmt_in->video.i_frame_rate_base &&
        p_dec->fmt_in->video.i_frame_rate &&
        (double) p_dec->fmt_in->video.i_frame_rate /
                 p_dec->fmt_in->video.i_frame_rate_base < 6 ) )
    {
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    }

    ret = ffmpeg_OpenCodec( p_dec, ctx, codec );
    if( ret < 0 )
        return ret;

    switch( ctx->active_thread_type )
    {
        case FF_THREAD_FRAME:
            msg_Dbg( p_dec, "using frame thread mode with %d threads",
                     ctx->thread_count );
            break;
        case FF_THREAD_SLICE:
            msg_Dbg( p_dec, "using slice thread mode with %d threads",
                     ctx->thread_count );
            break;
        case 0:
            if( ctx->thread_count > 1 )
                msg_Warn( p_dec, "failed to enable threaded decoding" );
            break;
        default:
            msg_Warn( p_dec, "using unknown thread mode with %d threads",
                      ctx->thread_count );
            break;
    }
    return 0;
}

static int InitVideoDecCommon( decoder_t *p_dec )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    AVCodecContext *p_context = p_sys->p_context;
    const AVCodec *p_codec = p_sys->p_codec;
    int i_val;

    p_sys->p_va = NULL;
    vlc_mutex_init( &p_sys->lock );

    /* ***** Fill p_context with init values ***** */
    p_context->codec_tag = ffmpeg_CodecTag( p_dec->fmt_in->i_original_fourcc ?
                                p_dec->fmt_in->i_original_fourcc : p_dec->fmt_in->i_codec );

    /*  ***** Get configuration of ffmpeg plugin ***** */
    p_context->workaround_bugs =
        var_InheritInteger( p_dec, "avcodec-workaround-bugs" );
    p_context->err_recognition =
        var_InheritInteger( p_dec, "avcodec-error-resilience" );

    if( var_CreateGetBool( p_dec, "grayscale" ) )
        p_context->flags |= AV_CODEC_FLAG_GRAY;

    /* ***** Output always the frames ***** */
    p_context->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;

#if OPAQUE_REF_ONLY
    p_context->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
#endif

#if LIBAVCODEC_VERSION_CHECK( 61, 03, 100 )
    if( p_dec->fmt_in->i_codec == VLC_CODEC_VVC )
        p_context->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
#endif

    i_val = var_CreateGetInteger( p_dec, "avcodec-skiploopfilter" );
    if( i_val >= 4 ) p_context->skip_loop_filter = AVDISCARD_ALL;
    else if( i_val == 3 ) p_context->skip_loop_filter = AVDISCARD_NONKEY;
    else if( i_val == 2 ) p_context->skip_loop_filter = AVDISCARD_BIDIR;
    else if( i_val == 1 ) p_context->skip_loop_filter = AVDISCARD_NONREF;
    else p_context->skip_loop_filter = AVDISCARD_DEFAULT;

    /* ***** libavcodec frame skipping ***** */
    p_sys->b_hurry_up = var_CreateGetBool( p_dec, "avcodec-hurry-up" );
    p_sys->b_show_corrupted = var_CreateGetBool( p_dec, "avcodec-corrupted" );

    i_val = var_CreateGetInteger( p_dec, "avcodec-skip-frame" );
    if( i_val >= 4 ) p_sys->i_skip_frame = AVDISCARD_ALL;
    else if( i_val == 3 ) p_sys->i_skip_frame = AVDISCARD_NONKEY;
    else if( i_val == 2 ) p_sys->i_skip_frame = AVDISCARD_BIDIR;
    else if( i_val == 1 ) p_sys->i_skip_frame = AVDISCARD_NONREF;
    else if( i_val == -1 ) p_sys->i_skip_frame = AVDISCARD_NONE;
    else p_sys->i_skip_frame = AVDISCARD_DEFAULT;
    p_context->skip_frame = p_sys->i_skip_frame;

    i_val = var_CreateGetInteger( p_dec, "avcodec-skip-idct" );
    if( i_val >= 4 ) p_context->skip_idct = AVDISCARD_ALL;
    else if( i_val == 3 ) p_context->skip_idct = AVDISCARD_NONKEY;
    else if( i_val == 2 ) p_context->skip_idct = AVDISCARD_BIDIR;
    else if( i_val == 1 ) p_context->skip_idct = AVDISCARD_NONREF;
    else if( i_val == -1 ) p_context->skip_idct = AVDISCARD_NONE;
    else p_context->skip_idct = AVDISCARD_DEFAULT;

    /* ***** libavcodec direct rendering ***** */
    p_sys->b_direct_rendering = false;
    p_sys->b_dr_failure = false;
    if( var_CreateGetBool( p_dec, "avcodec-dr" ) &&
       (p_codec->capabilities & AV_CODEC_CAP_DR1) &&
        /* No idea why ... but this fixes flickering on some TSCC streams */
        p_sys->p_codec->id != AV_CODEC_ID_TSCC &&
        p_sys->p_codec->id != AV_CODEC_ID_CSCD &&
        p_sys->p_codec->id != AV_CODEC_ID_CINEPAK )
    {
        /* Some codecs set pix_fmt only after the 1st frame has been decoded,
         * so we need to do another check in ffmpeg_GetFrameBuf() */
        p_sys->b_direct_rendering = true;
    }

    p_context->get_format = ffmpeg_GetFormat;
    /* Always use our get_buffer wrapper so we can calculate the
     * PTS correctly */
    p_context->get_buffer2 = lavc_GetFrame;
    p_context->opaque = p_dec;

    FrameInfoInit( p_sys );

    int max_thread_count;
    int i_thread_count = p_sys->b_hardware_only ? 1 : var_InheritInteger( p_dec, "avcodec-threads" );
    if( i_thread_count <= 0 )
    {
        i_thread_count = vlc_GetCPUCount();
        if( i_thread_count > 1 )
            i_thread_count++;

        //FIXME: take in count the decoding time
        max_thread_count = p_codec->id == AV_CODEC_ID_HEVC ? 10 : 6;
#if defined(_WIN32)
# if !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
        // reduce memory when decoding on Xbox
        max_thread_count = 6 ;
# endif
#endif
    }
    else
        max_thread_count = p_codec->id == AV_CODEC_ID_HEVC ? 32 : 16;
    i_thread_count = __MIN( i_thread_count, max_thread_count );
    msg_Dbg( p_dec, "allowing %d thread(s) for decoding", i_thread_count );
    p_context->thread_count = i_thread_count;
#if LIBAVCODEC_VERSION_MAJOR < 60
    p_context->thread_safe_callbacks = true;
#endif

    switch( p_codec->id )
    {
        case AV_CODEC_ID_MPEG4:
        case AV_CODEC_ID_H263:
            p_context->thread_type = 0;
            break;
        case AV_CODEC_ID_MPEG1VIDEO:
        case AV_CODEC_ID_MPEG2VIDEO:
            p_context->thread_type &= ~FF_THREAD_SLICE;
            /* fall through */
        default:
            break;
    }

    if( var_InheritBool(p_dec, "low-delay") )
        p_context->thread_type &= ~FF_THREAD_FRAME;

    if( p_context->thread_type & FF_THREAD_FRAME )
        p_dec->i_extra_picture_buffers = 2 * p_context->thread_count;

    /* ***** misc init ***** */
    date_Init(&p_sys->pts, 1, 30001);
    p_sys->b_first_frame = true;
    p_sys->i_late_frames = 0;
    p_sys->b_from_preroll = false;
    p_sys->i_last_output_frame = -1;
    p_sys->framedrop = FRAMEDROP_NONE;

    /* Set output properties */
    if (GetVlcChroma( &p_dec->fmt_out.video, p_context->pix_fmt ) == VLC_SUCCESS)
        p_dec->fmt_out.i_codec = p_dec->fmt_out.video.i_chroma;

    p_dec->fmt_out.video.orientation = p_dec->fmt_in->video.orientation;

    if( p_dec->fmt_in->video.p_palette ) {
        p_sys->palette_sent = false;
        p_dec->fmt_out.video.p_palette = malloc( sizeof(video_palette_t) );
        if( p_dec->fmt_out.video.p_palette )
            *p_dec->fmt_out.video.p_palette = *p_dec->fmt_in->video.p_palette;
    } else
        p_sys->palette_sent = true;

    /* ***** init this codec with special data ***** */
    ffmpeg_InitCodec( p_dec );

    /* ***** Open the codec ***** */
    if( OpenVideoCodec( p_dec ) < 0 )
    {
        free( p_sys );
        avcodec_free_context( &p_context );
        return VLC_EGENERIC;
    }

    p_dec->pf_decode = DecodeVideo;
    p_dec->pf_flush  = Flush;

    return VLC_SUCCESS;
}

static int lavc_UpdateHWVideoFormat(decoder_t *p_dec, AVCodecContext *p_context,
                                    enum AVPixelFormat hwfmt, enum AVPixelFormat swfmt,
                                    vlc_decoder_device **pp_dec_device)
{
    if( hwfmt == AV_PIX_FMT_NONE )
        return VLC_EGENERIC;

    if (!vlc_va_MightDecode(hwfmt))
        return VLC_EGENERIC; /* Unknown brand of hardware acceleration */
    if (p_context->width == 0 || p_context->height == 0)
    {   /* should never happen */
        msg_Err(p_dec, "unspecified video dimensions");
        return VLC_EGENERIC;
    }
    const AVPixFmtDescriptor *dsc = av_pix_fmt_desc_get(hwfmt);
    msg_Dbg(p_dec, "trying format %s", dsc ? dsc->name : "unknown");
    if (lavc_UpdateVideoFormat(p_dec, p_context, hwfmt, swfmt))
        return VLC_EGENERIC; /* Unsupported brand of hardware acceleration */

    *pp_dec_device = decoder_GetDecoderDevice(p_dec);
    if (*pp_dec_device == NULL)
        return VLC_EGENERIC;

    return VLC_SUCCESS;
}

static void ffmpeg_CloseVa(decoder_t *p_dec, AVCodecContext *context)
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    assert(p_sys->p_va != NULL);
    assert(p_sys->vctx_out != NULL);

    vlc_va_Delete(p_sys->p_va, NULL);
    p_sys->p_va = NULL;
    vlc_video_context_Release(p_sys->vctx_out);
    p_sys->vctx_out = NULL;
    p_sys->use_hwframes = false;

    if (context != NULL)
        context->hwaccel_context = NULL;
}

static int ffmpeg_OpenVa(decoder_t *p_dec, AVCodecContext *p_context,
                         enum AVPixelFormat hwfmt,
                         const AVPixFmtDescriptor *src_desc,
                         vlc_decoder_device *dec_device)
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    p_dec->fmt_out.video.i_chroma = 0; // make sure the va sets its output chroma
    struct vlc_va_cfg cfg = {
        .avctx = p_context,
        .hwfmt = hwfmt,
        .desc = src_desc,
        .fmt_in = p_dec->fmt_in,
        .dec_device = dec_device,
        .video_fmt_out = &p_dec->fmt_out.video,
        .vctx_out = NULL,
        .use_hwframes = false,
        .extra_pictures = 0,
    };
    vlc_va_t *va = vlc_va_New(VLC_OBJECT(p_dec), &cfg);

    if (va == NULL)
        return VLC_EGENERIC; /* Unsupported codec profile or such */
    assert(p_dec->fmt_out.video.i_chroma != 0);
    assert(cfg.vctx_out != NULL);
    p_dec->fmt_out.i_codec = p_dec->fmt_out.video.i_chroma;
    p_dec->i_extra_picture_buffers += cfg.extra_pictures;

    if (decoder_UpdateVideoOutput(p_dec, cfg.vctx_out))
    {
        vlc_va_Delete(va, p_context);
        return VLC_EGENERIC; /* Unsupported codec profile or such */
    }

    p_sys->p_va = va;
    p_sys->vctx_out = vlc_video_context_Hold( cfg.vctx_out );
    p_sys->pix_fmt = hwfmt;
    p_sys->use_hwframes = cfg.use_hwframes;
    return VLC_SUCCESS;
}

static int ffmpeg_RecreateVa(decoder_t *p_dec, AVCodecContext *p_context,
                             enum AVPixelFormat swfmt)
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    assert(p_sys->vctx_out != NULL);

    vlc_decoder_device *dec_device =
        vlc_video_context_HoldDevice(p_sys->vctx_out);

    ffmpeg_CloseVa(p_dec, p_context);

    const AVPixFmtDescriptor *src_desc = av_pix_fmt_desc_get(swfmt);

    int ret = ffmpeg_OpenVa(p_dec, p_context, p_sys->pix_fmt,
                            src_desc, dec_device);
    if (dec_device != NULL)
        vlc_decoder_device_Release(dec_device);
    return ret;
}

static const enum AVPixelFormat hwfmts[] =
{
#ifdef _WIN32
    AV_PIX_FMT_D3D11VA_VLD,
    AV_PIX_FMT_DXVA2_VLD,
#endif
    AV_PIX_FMT_VAAPI,
    AV_PIX_FMT_VDPAU,
    AV_PIX_FMT_NONE,
};

static int ExtractAV1Profile(AVCodecContext *p_context, const es_format_t *fmt_in, decoder_sys_t *p_sys)
{
    av1_OBU_sequence_header_t *sequence_hdr = NULL;
    unsigned w, h;

    if (fmt_in->i_extra > 4)
    {
        // in ISOBMFF/WebM/Matroska the first 4 bytes are from the AV1CodecConfigurationRecord
        // and then one or more OBU
        const uint8_t *obu_start = ((const uint8_t*) fmt_in->p_extra) + 4;
        int obu_size = fmt_in->i_extra - 4;
        if (AV1_OBUIsValid(obu_start, obu_size) && AV1_OBUGetType(obu_start) == AV1_OBU_SEQUENCE_HEADER)
            sequence_hdr = AV1_OBU_parse_sequence_header(obu_start, obu_size);
    }

    if (sequence_hdr == NULL)
        return VLC_ENOTSUP;

    // fill the AVCodecContext with the values from the sequence header
    // so we can create the expected VA right away:
    // coded_width, coded_height, framerate, profile and sw_pix_fmt

    vlc_fourcc_t chroma = AV1_get_chroma(sequence_hdr);
    if (chroma == 0)
    {
        AV1_release_sequence_header(sequence_hdr);
        return VLC_ENOTSUP;
    }
    bool uv_flipped;
    p_context->sw_pix_fmt = FindFfmpegChroma(chroma, &uv_flipped);
    if (p_context->sw_pix_fmt == AV_PIX_FMT_NONE)
    {
        AV1_release_sequence_header(sequence_hdr);
        return VLC_ENOTSUP;
    }
    assert(uv_flipped == false);

    AV1_get_frame_max_dimensions(sequence_hdr, &w, &h);

    p_context->coded_width  = p_context->width  = w;
    p_context->coded_height = p_context->height = h;

    if (!fmt_in->video.i_frame_rate || !fmt_in->video.i_frame_rate_base)
    {
        unsigned num, den;
        if (AV1_get_frame_rate(sequence_hdr, &num, &den))
        {
            p_context->framerate.num = num;
            p_context->framerate.den = den;
        }
    }

    int tier;
    AV1_get_profile_level(sequence_hdr, &p_sys->profile, &p_sys->level, &tier);

    AV1_release_sequence_header(sequence_hdr);

    return VLC_SUCCESS;
}

int InitVideoHwDec( vlc_object_t *obj )
{
    decoder_t *p_dec = container_of(obj, decoder_t, obj);

    if (p_dec->fmt_in->i_codec != VLC_CODEC_AV1)
        return VLC_EGENERIC;

    decoder_sys_t *p_sys = calloc(1, sizeof(*p_sys));
    if( unlikely(p_sys == NULL) )
        return VLC_ENOMEM;

    const AVCodec *p_codec;
    AVCodecContext *p_context = ffmpeg_AllocContext( p_dec, &p_codec );
    if( unlikely(p_context == NULL) )
    {
        free(p_sys);
        return VLC_ENOMEM;
    }

    if (ExtractAV1Profile(p_context, p_dec->fmt_in, p_sys) != VLC_SUCCESS)
        goto failed;

    p_dec->p_sys = p_sys;
    p_sys->p_context = p_context;
    p_sys->p_codec = p_codec;
    p_sys->pix_fmt = AV_PIX_FMT_NONE;
    p_sys->b_hardware_only = true;

    int res = InitVideoDecCommon( p_dec );
    if (res != VLC_SUCCESS)
        return res;

    const AVPixFmtDescriptor *src_desc = av_pix_fmt_desc_get(p_context->sw_pix_fmt);

    for( size_t i = 0; hwfmts[i] != AV_PIX_FMT_NONE; i++ )
    {
        vlc_decoder_device *dec_device;
        int ret = lavc_UpdateHWVideoFormat(p_dec, p_context, hwfmts[i],
                                           p_context->sw_pix_fmt, &dec_device);
        if (ret != VLC_SUCCESS)
            continue;

        ret = ffmpeg_OpenVa(p_dec, p_context, hwfmts[i], src_desc, dec_device);
        if (dec_device != NULL)
            vlc_decoder_device_Release(dec_device);

        if (ret == VLC_SUCCESS)
            // we have a matching hardware decoder
            return VLC_SUCCESS;
    }

    EndVideoDec(obj);
    return VLC_EGENERIC;
failed:
    avcodec_free_context( &p_context );
    free(p_sys);
    return VLC_EGENERIC;
}

/*****************************************************************************
 * InitVideo: initialize the video decoder
 *****************************************************************************
 * the ffmpeg codec will be opened, some memory allocated. The vout is not yet
 * opened (done after the first decoded frame).
 *****************************************************************************/
int InitVideoDec( vlc_object_t *obj )
{
    decoder_t *p_dec = (decoder_t *)obj;
    const AVCodec *p_codec;
    AVCodecContext *p_context = ffmpeg_AllocContext( p_dec, &p_codec );
    if( p_context == NULL )
        return VLC_EGENERIC;

    /* Allocate the memory needed to store the decoder's structure */
    decoder_sys_t *p_sys = calloc( 1, sizeof(*p_sys) );
    if( unlikely(p_sys == NULL) )
    {
        avcodec_free_context( &p_context );
        return VLC_ENOMEM;
    }

    p_dec->p_sys = p_sys;
    p_sys->p_context = p_context;
    p_sys->p_codec = p_codec;
    p_sys->profile = -1;
    p_sys->level = -1;
    p_sys->b_hardware_only = false;

    return InitVideoDecCommon( p_dec );
}

/*****************************************************************************
 * Flush:
 *****************************************************************************/
static void Flush( decoder_t *p_dec )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    AVCodecContext *p_context = p_sys->p_context;

    p_sys->i_late_frames = 0;
    p_sys->framedrop = FRAMEDROP_NONE;
    cc_Flush( &p_sys->cc );

    /* do not flush buffers if codec hasn't been opened (theora/vorbis/VC1) */
    if( avcodec_is_open( p_context ) )
        avcodec_flush_buffers( p_context );

    date_Set(&p_sys->pts, VLC_TICK_INVALID); /* To make sure we recover properly */
}

static block_t * filter_earlydropped_blocks( decoder_t *p_dec, block_t *block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    if( !block )
        return NULL;

    if( block->i_flags & BLOCK_FLAG_PREROLL )
    {
        /* Do not care about late frames when prerolling
         * TODO avoid decoding of non reference frame
         * (ie all B except for H264 where it depends only on nal_ref_idc) */
        p_sys->i_late_frames = 0;
        p_sys->framedrop = FRAMEDROP_NONE;
        p_sys->b_from_preroll = true;
        p_sys->i_last_late_delay = VLC_TICK_MAX;
    }

    if( p_sys->i_late_frames == 0 )
        p_sys->framedrop = FRAMEDROP_NONE;

    if( p_sys->framedrop == FRAMEDROP_NONE && p_sys->i_late_frames < 11 )
        return block;

    if( p_sys->i_last_output_frame >= 0 &&
        NextPktSequenceNumber( p_sys ) - p_sys->i_last_output_frame > 24 )
    {
        p_sys->framedrop = FRAMEDROP_AGGRESSIVE_RECOVER;
    }

    /* A good idea could be to decode all I pictures and see for the other */
    if( p_sys->framedrop == FRAMEDROP_AGGRESSIVE_RECOVER )
    {
        if( !(block->i_flags & BLOCK_FLAG_TYPE_I) )
        {
            msg_Err( p_dec, "more than %"PRId64" frames of late video -> "
                            "dropping frame (computer too slow ?)",
                     NextPktSequenceNumber( p_sys ) - p_sys->i_last_output_frame );

            vlc_mutex_lock(&p_sys->lock);
            date_Set( &p_sys->pts, VLC_TICK_INVALID ); /* To make sure we recover properly */
            vlc_mutex_unlock(&p_sys->lock);
            block_Release( block );
            p_sys->i_late_frames--;
            return NULL;
        }
    }

    return block;
}

static vlc_tick_t interpolate_next_pts( decoder_t *p_dec, AVFrame *frame )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    AVCodecContext *p_context = p_sys->p_context;

    if( p_sys->pts.i_divider_num == 0 ||
        date_Get( &p_sys->pts ) == VLC_TICK_INVALID )
        return VLC_TICK_INVALID;

#if LIBAVCODEC_VERSION_CHECK( 60, 12, 100 )
    int i_tick = p_context->codec_descriptor->props & AV_CODEC_PROP_FIELDS ? 2 : 1;
#else
    int i_tick = p_context->ticks_per_frame;
#endif

    /* interpolate the next PTS */
    return date_Increment( &p_sys->pts, i_tick + frame->repeat_pict );
}

static void update_late_frame_count( decoder_t *p_dec, block_t *p_block,
                                     vlc_tick_t current_time, vlc_tick_t i_pts,
                                     vlc_tick_t i_next_pts, int64_t i_fnum )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
   /* Update frame late count (except when doing preroll) */
   vlc_tick_t i_display_date = VLC_TICK_INVALID;
   if( !p_block || !(p_block->i_flags & BLOCK_FLAG_PREROLL) )
       i_display_date = decoder_GetDisplayDate( p_dec, current_time, i_pts );

   vlc_tick_t i_threshold = i_next_pts != VLC_TICK_INVALID
                          ? (i_next_pts - i_pts) / 2 : VLC_TICK_FROM_MS(20);

   if( i_display_date != VLC_TICK_INVALID && i_display_date + i_threshold <= current_time )
   {
       /* Out of preroll, consider only late frames on rising delay */
       if( p_sys->b_from_preroll )
       {
           if( p_sys->i_last_late_delay > current_time - i_display_date )
           {
               p_sys->i_last_late_delay = current_time - i_display_date;
               return;
           }
           p_sys->b_from_preroll = false;
       }

       p_sys->i_late_frames++;
   }
   else
   {
       p_sys->i_last_output_frame = i_fnum;
       p_sys->i_late_frames = 0;
   }
}

#if LIBAVUTIL_VERSION_CHECK( 57, 16, 100 )
static void map_dovi_metadata( vlc_video_dovi_metadata_t *out,
                               const AVDOVIMetadata *data )
{
    const AVDOVIRpuDataHeader *hdr = av_dovi_get_header( data );
    const AVDOVIDataMapping *vdm = av_dovi_get_mapping( data );
    const AVDOVIColorMetadata *color = av_dovi_get_color( data );
    out->bl_bit_depth = hdr->bl_bit_depth;
    out->el_bit_depth = hdr->el_bit_depth;
    out->coef_log2_denom = hdr->coef_log2_denom;
    out->nlq_method_idc = (enum vlc_dovi_nlq_method_t) vdm->nlq_method_idc;
    for( size_t i = 0; i < ARRAY_SIZE( out->nonlinear_offset ); i++ )
        out->nonlinear_offset[i] = av_q2d( color->ycc_to_rgb_offset[i] );
    for( size_t i = 0; i < ARRAY_SIZE( out->nonlinear_matrix ); i++ )
        out->nonlinear_matrix[i] = av_q2d( color->ycc_to_rgb_matrix[i] );
    for( size_t i = 0; i < ARRAY_SIZE( out->linear_matrix); i++ )
        out->linear_matrix[i] = av_q2d( color->rgb_to_lms_matrix[i] );
    out->source_min_pq = color->source_min_pq;
    out->source_max_pq = color->source_max_pq;

    static_assert(sizeof(out->curves) == sizeof(vdm->curves), "struct mismatch");
    static_assert(sizeof(out->nlq)    == sizeof(vdm->nlq),    "struct mismatch");
    memcpy(out->curves, vdm->curves, sizeof(out->curves));
    memcpy(out->nlq,    vdm->nlq,    sizeof(out->nlq));
    for( size_t i = 0; i < ARRAY_SIZE( out->curves ); i++)
        assert( out->curves[i].num_pivots <= ARRAY_SIZE( out->curves[i].pivots ));
}
#endif

static void map_hdrplus_metadata( vlc_video_hdr_dynamic_metadata_t *out,
                                  const AVDynamicHDRPlus *data )
{
    out->country_code = data->itu_t_t35_country_code;
    out->application_version = data->application_version;
    out->targeted_luminance = av_q2d( data->targeted_system_display_maximum_luminance );

    assert( data->num_windows > 0 );
    const AVHDRPlusColorTransformParams *pars = &data->params[0];
    for ( size_t i = 0; i < ARRAY_SIZE( out->maxscl ); i++ )
        out->maxscl[i] = av_q2d( pars->maxscl[i] );
    out->average_maxrgb = av_q2d( pars->average_maxrgb );
    out->num_histogram = pars->num_distribution_maxrgb_percentiles;
    assert( out->num_histogram < ARRAY_SIZE( out->histogram ));
    for ( size_t i = 0; i < out->num_histogram; i++ ) {
        const AVHDRPlusPercentile pct = pars->distribution_maxrgb[i];
        out->histogram[i].percentage = pct.percentage;
        out->histogram[i].percentile = av_q2d( pct.percentile );
    }
    out->fraction_bright_pixels = av_q2d( pars->fraction_bright_pixels );
    out->tone_mapping_flag = pars->tone_mapping_flag;
    if ( out->tone_mapping_flag ) {
        out->knee_point_x = av_q2d( pars->knee_point_x );
        out->knee_point_y = av_q2d( pars->knee_point_y );
        out->num_bezier_anchors = pars->num_bezier_curve_anchors;
        assert( out->num_bezier_anchors < ARRAY_SIZE( out->bezier_curve_anchors ));
        for ( size_t i = 0; i < out->num_bezier_anchors; i++ )
            out->bezier_curve_anchors[i] = av_q2d( pars->bezier_curve_anchors[i] );
    }
}

static int DecodeSidedata( decoder_t *p_dec, const AVFrame *frame, picture_t *p_pic )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    bool format_changed = false;

#define FROM_AVRAT(default_factor, avrat) \
(uint64_t)(default_factor) * (avrat).num / (avrat).den
    const AVFrameSideData *metadata =
            av_frame_get_side_data( frame,
                                    AV_FRAME_DATA_MASTERING_DISPLAY_METADATA );
    if ( metadata )
    {
        const AVMasteringDisplayMetadata *hdr_meta =
                (const AVMasteringDisplayMetadata *) metadata->data;
        if ( hdr_meta->has_luminance )
        {
#define ST2086_LUMA_FACTOR 10000
            p_pic->format.mastering.max_luminance =
                    FROM_AVRAT(ST2086_LUMA_FACTOR, hdr_meta->max_luminance);
            p_pic->format.mastering.min_luminance =
                    FROM_AVRAT(ST2086_LUMA_FACTOR, hdr_meta->min_luminance);
        }
        if ( hdr_meta->has_primaries )
        {
#define ST2086_RED   2
#define ST2086_GREEN 0
#define ST2086_BLUE  1
#define LAV_RED    0
#define LAV_GREEN  1
#define LAV_BLUE   2
#define ST2086_PRIM_FACTOR 50000
            p_pic->format.mastering.primaries[ST2086_RED*2   + 0] =
                    FROM_AVRAT(ST2086_PRIM_FACTOR, hdr_meta->display_primaries[LAV_RED][0]);
            p_pic->format.mastering.primaries[ST2086_RED*2   + 1] =
                    FROM_AVRAT(ST2086_PRIM_FACTOR, hdr_meta->display_primaries[LAV_RED][1]);
            p_pic->format.mastering.primaries[ST2086_GREEN*2 + 0] =
                    FROM_AVRAT(ST2086_PRIM_FACTOR, hdr_meta->display_primaries[LAV_GREEN][0]);
            p_pic->format.mastering.primaries[ST2086_GREEN*2 + 1] =
                    FROM_AVRAT(ST2086_PRIM_FACTOR, hdr_meta->display_primaries[LAV_GREEN][1]);
            p_pic->format.mastering.primaries[ST2086_BLUE*2  + 0] =
                    FROM_AVRAT(ST2086_PRIM_FACTOR, hdr_meta->display_primaries[LAV_BLUE][0]);
            p_pic->format.mastering.primaries[ST2086_BLUE*2  + 1] =
                    FROM_AVRAT(ST2086_PRIM_FACTOR, hdr_meta->display_primaries[LAV_BLUE][1]);
            p_pic->format.mastering.white_point[0] =
                    FROM_AVRAT(ST2086_PRIM_FACTOR, hdr_meta->white_point[0]);
            p_pic->format.mastering.white_point[1] =
                    FROM_AVRAT(ST2086_PRIM_FACTOR, hdr_meta->white_point[1]);
        }

        if ( memcmp( &p_dec->fmt_out.video.mastering,
                     &p_pic->format.mastering,
                     sizeof(p_pic->format.mastering) ) )
        {
            p_dec->fmt_out.video.mastering = p_pic->format.mastering;
            format_changed = true;
        }
#undef FROM_AVRAT
    }
    const AVFrameSideData *metadata_lt =
            av_frame_get_side_data( frame,
                                    AV_FRAME_DATA_CONTENT_LIGHT_LEVEL );
    if ( metadata_lt )
    {
        const AVContentLightMetadata *light_meta =
                (const AVContentLightMetadata *) metadata_lt->data;
        p_pic->format.lighting.MaxCLL = light_meta->MaxCLL;
        p_pic->format.lighting.MaxFALL = light_meta->MaxFALL;
        if ( memcmp( &p_dec->fmt_out.video.lighting,
                     &p_pic->format.lighting,
                     sizeof(p_pic->format.lighting) ) )
        {
            p_dec->fmt_out.video.lighting  = p_pic->format.lighting;
            format_changed = true;
        }
    }

    const AVFrameSideData *p_stereo3d_data =
            av_frame_get_side_data( frame,
                                    AV_FRAME_DATA_STEREO3D );
    if( p_stereo3d_data )
    {
        const struct AVStereo3D *stereo_data =
                (const AVStereo3D *) p_stereo3d_data->data;
        switch (stereo_data->type)
        {
        case AV_STEREO3D_SIDEBYSIDE:
            p_pic->format.multiview_mode = MULTIVIEW_STEREO_SBS;
            break;
        case AV_STEREO3D_TOPBOTTOM:
            p_pic->format.multiview_mode = MULTIVIEW_STEREO_TB;
            break;
        case AV_STEREO3D_FRAMESEQUENCE:
            p_pic->format.multiview_mode = MULTIVIEW_STEREO_FRAME;
            break;
        case AV_STEREO3D_COLUMNS:
            p_pic->format.multiview_mode = MULTIVIEW_STEREO_ROW;
            break;
        case AV_STEREO3D_LINES:
            p_pic->format.multiview_mode = MULTIVIEW_STEREO_COL;
            break;
        case AV_STEREO3D_CHECKERBOARD:
            p_pic->format.multiview_mode = MULTIVIEW_STEREO_CHECKERBOARD;
            break;
        default:
        case AV_STEREO3D_2D:
            p_pic->format.multiview_mode = MULTIVIEW_2D;
            break;
        }
        p_pic->format.b_multiview_right_eye_first = stereo_data->flags & AV_STEREO3D_FLAG_INVERT;
        p_pic->b_multiview_left_eye = (stereo_data->view == AV_STEREO3D_VIEW_LEFT);

        p_dec->fmt_out.video.b_multiview_right_eye_first = p_pic->format.b_multiview_right_eye_first;

        if (p_dec->fmt_out.video.multiview_mode != p_pic->format.multiview_mode)
        {
            p_dec->fmt_out.video.multiview_mode = p_pic->format.multiview_mode;
            format_changed = true;
        }
    }
    else
        p_pic->format.multiview_mode = p_dec->fmt_out.video.multiview_mode;

    if (format_changed && decoder_UpdateVideoOutput( p_dec, p_sys->vctx_out ))
        return -1;

    const AVFrameSideData *p_avcc = av_frame_get_side_data( frame, AV_FRAME_DATA_A53_CC );
    if( p_avcc )
    {
        cc_Extract( &p_sys->cc, CC_PAYLOAD_RAW, true, p_avcc->data, p_avcc->size );
        if( p_sys->cc.b_reorder || p_sys->cc.i_data )
        {
            block_t *p_cc = block_Alloc( p_sys->cc.i_data );
            if( p_cc )
            {
                memcpy( p_cc->p_buffer, p_sys->cc.p_data, p_sys->cc.i_data );
                if( p_sys->cc.b_reorder )
                    p_cc->i_dts = p_cc->i_pts = p_pic->date;
                else
                    p_cc->i_pts = p_cc->i_dts;
                decoder_cc_desc_t desc;
                desc.i_608_channels = p_sys->cc.i_608channels;
                desc.i_708_channels = p_sys->cc.i_708channels;
                desc.i_reorder_depth = 4;
                decoder_QueueCc( p_dec, p_cc, &desc );
            }
            cc_Flush( &p_sys->cc );
        }
    }

#if LIBAVUTIL_VERSION_CHECK( 57, 16, 100 )
    const AVFrameSideData *p_dovi = av_frame_get_side_data( frame, AV_FRAME_DATA_DOVI_METADATA );
    if( p_dovi )
    {
        vlc_video_dovi_metadata_t *dst;
        dst = picture_AttachNewAncillary( p_pic, VLC_ANCILLARY_ID_DOVI, sizeof(*dst) );
        if( !dst )
            return VLC_ENOMEM;
        map_dovi_metadata( dst, (AVDOVIMetadata *) p_dovi->data );
    }
#endif

    const AVFrameSideData *p_hdrplus = av_frame_get_side_data( frame, AV_FRAME_DATA_DYNAMIC_HDR_PLUS );
    if( p_hdrplus )
    {
        vlc_video_hdr_dynamic_metadata_t *dst;
        dst = picture_AttachNewAncillary( p_pic, VLC_ANCILLARY_ID_HDR10PLUS, sizeof(*dst) );
        if( !dst )
            return VLC_ENOMEM;
        map_hdrplus_metadata( dst, (AVDynamicHDRPlus *) p_hdrplus->data );
    }

    const AVFrameSideData *p_icc = av_frame_get_side_data( frame, AV_FRAME_DATA_ICC_PROFILE );
    if( p_icc )
    {
        vlc_icc_profile_t *icc;
        icc = picture_AttachNewAncillary( p_pic, VLC_ANCILLARY_ID_ICC, sizeof(*icc) + p_icc->size );
        if( !icc )
            return VLC_ENOMEM;
        memcpy( icc->data, p_icc->data, p_icc->size );
        icc->size = p_icc->size;
    }

    return 0;
}

/*****************************************************************************
 * DecodeBlock: Called to decode one or more frames
 *              drains if pp_block == NULL
 *              tries to output only if p_block == NULL
 *****************************************************************************/
static int DecodeBlock( decoder_t *p_dec, block_t **pp_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    AVCodecContext *p_context = p_sys->p_context;
    /* Boolean if we assume that we should get valid pic as result */
    bool b_need_output_picture = true;
    bool b_error = false;

    block_t *p_block = pp_block ? *pp_block : NULL;

    if( !p_context->extradata_size && p_dec->fmt_in->i_extra )
    {
        ffmpeg_InitCodec( p_dec );
        if( !avcodec_is_open( p_context ) && OpenVideoCodec(p_dec) < 0 )
        {
            if( p_block != NULL )
                block_Release( p_block );
            return VLCDEC_ECRITICAL;
        }
    }

    if( !avcodec_is_open( p_context ) )
    {
        if( p_block )
            block_Release( p_block );
        return VLCDEC_SUCCESS;
    }

    /* Defaults that if we aren't in prerolling, we want output picture
       same for if we are flushing (p_block==NULL) */
    if( !p_block || !(p_block->i_flags & BLOCK_FLAG_PREROLL) )
        b_need_output_picture = true;
    else
        b_need_output_picture = false;

    /* Change skip_frame config only if hurry_up is enabled */
    if( p_sys->b_hurry_up )
    {
        p_context->skip_frame = p_sys->i_skip_frame;

        /* Check also if we should/can drop the block and move to next block
            as trying to catchup the speed*/
        if( p_dec->b_frame_drop_allowed )
            p_block = filter_earlydropped_blocks( p_dec, p_block );
    }

    if( !b_need_output_picture || p_sys->framedrop == FRAMEDROP_NONREF )
    {
        p_context->skip_frame = __MAX( p_context->skip_frame, AVDISCARD_NONREF );
    }

    /*
     * Do the actual decoding now */

    /* Don't forget that libavcodec requires a little more bytes
     * that the real frame size */
    if( p_block && p_block->i_buffer > 0 )
    {
        p_block = block_Realloc( p_block, 0,
                            p_block->i_buffer + FF_INPUT_BUFFER_PADDING_SIZE );
        if( !p_block )
            return VLCDEC_ECRITICAL;
        p_block->i_buffer -= FF_INPUT_BUFFER_PADDING_SIZE;
        *pp_block = p_block;
        memset( p_block->p_buffer + p_block->i_buffer, 0,
                FF_INPUT_BUFFER_PADDING_SIZE );
    }

    bool b_drain = ( pp_block == NULL );
    bool b_drained = false;
    bool b_first_output_sequence = true;

    do
    {
        int i_used = 0;

        if( (p_block && p_block->i_buffer > 0) || b_drain )
        {
            AVPacket *pkt = av_packet_alloc();
            if(!pkt)
            {
                b_error = true;
                break;
            }
            if( p_block && p_block->i_buffer > 0 )
            {
                pkt->data = p_block->p_buffer;
                pkt->size = p_block->i_buffer;
                pkt->pts = p_block->i_pts != VLC_TICK_INVALID ? TO_AV_TS(p_block->i_pts) : AV_NOPTS_VALUE;
                pkt->dts = p_block->i_dts != VLC_TICK_INVALID ? TO_AV_TS(p_block->i_dts) : AV_NOPTS_VALUE;
                if (p_block->i_flags & BLOCK_FLAG_TYPE_I)
                    pkt->flags |= AV_PKT_FLAG_KEY;
            }
            else
            {
                /* Drain */
                pkt->data = NULL;
                pkt->size = 0;
                b_drain = false;
                b_drained = true;
            }

            if( !p_sys->palette_sent )
            {
                uint8_t *pal = av_packet_new_side_data(pkt, AV_PKT_DATA_PALETTE, AVPALETTE_SIZE);
                if (pal) {
                    const video_palette_t *p_palette = p_dec->fmt_in->video.p_palette;
                    for (size_t i=0; i<ARRAY_SIZE(p_palette->palette); i++)
                    {
                        memcpy(pal, p_palette->palette[i], sizeof(p_palette->palette[0]));
                        pal += sizeof(p_palette->palette[0]);
                    }
                    p_sys->palette_sent = true;
                }
            }

            /* Make sure we don't reuse the same timestamps twice */
            if( p_block )
            {
                p_block->i_pts =
                p_block->i_dts = VLC_TICK_INVALID;
            }

            struct frame_info_s *p_frame_info = FrameInfoAdd( p_sys, pkt );
            if( !p_frame_info )
            {
                av_packet_free( &pkt );
                b_error = true;
                break;
            }
            const bool b_eos = p_block && (p_block->i_flags & BLOCK_FLAG_END_OF_SEQUENCE);
            p_frame_info->b_eos = b_eos;
            p_frame_info->b_display = b_need_output_picture;

            int ret = avcodec_send_packet(p_context, pkt);
            if( ret != 0 && ret != AVERROR(EAGAIN) )
            {
                if (ret == AVERROR(ENOMEM) || ret == AVERROR(EINVAL))
                {
                    msg_Err(p_dec, "avcodec_send_packet critical error");
                    b_error = true;
                }
                av_packet_free( &pkt );
                break;
            }

            i_used = ret != AVERROR(EAGAIN) ? pkt->size : 0;
            av_packet_free( &pkt );

            if( b_eos && !b_drained )
            {
                 avcodec_send_packet( p_context, NULL );
                 b_drained = true;
            }
        }

        AVFrame *frame = av_frame_alloc();
        if (unlikely(frame == NULL))
        {
            b_error = true;
            break;
        }

        int ret = avcodec_receive_frame(p_context, frame);
        if( ret != 0 && ret != AVERROR(EAGAIN) )
        {
            if (ret == AVERROR(ENOMEM) || ret == AVERROR(EINVAL))
            {
                msg_Err(p_dec, "avcodec_receive_frame critical error");
                b_error = true;
            }
            av_frame_free(&frame);
            if( ret == AVERROR_EOF )
                break;
        }
        bool not_received_frame = ret;

        if( p_block )
        {
            /* Consumed bytes */
            p_block->p_buffer += i_used;
            p_block->i_buffer -= i_used;
        }

        /* Nothing to display */
        if( not_received_frame )
        {
            av_frame_free(&frame);
            if( i_used == 0 ) break;
            continue;
        }

        struct frame_info_s *p_frame_info = FrameInfoGet( p_sys, frame );
        if( p_frame_info && p_frame_info->b_eos )
            p_sys->b_first_frame = true;

        vlc_mutex_lock(&p_sys->lock);

        /* Compute the PTS */
        int64_t av_pts = frame->best_effort_timestamp;
        if( av_pts == AV_NOPTS_VALUE )
            av_pts = frame->pkt_dts;

        vlc_tick_t i_pts;
        if( av_pts == AV_NOPTS_VALUE )
            i_pts = date_Get( &p_sys->pts );
        else
            i_pts = FROM_AV_TS(av_pts);

        /* Interpolate the next PTS */
        if( i_pts != VLC_TICK_INVALID )
            date_Set( &p_sys->pts, i_pts );

        const vlc_tick_t i_next_pts = interpolate_next_pts(p_dec, frame);

        if( b_first_output_sequence )
        {
            if( p_frame_info )
                update_late_frame_count( p_dec, p_block, vlc_tick_now(), i_pts,
                                        i_next_pts, FrameSequenceNumber( frame, p_frame_info ) );
            b_first_output_sequence = false;
        }

        if( (p_frame_info && !p_frame_info->b_display) ||
           ( !p_sys->p_va && !frame->linesize[0] ) ||
           ( p_dec->b_frame_drop_allowed && (frame->flags & AV_FRAME_FLAG_CORRUPT) &&
             !p_sys->b_show_corrupted ) )
        {
            vlc_mutex_unlock(&p_sys->lock);
            av_frame_free(&frame);
            continue;
        }

        if( p_context->pix_fmt == AV_PIX_FMT_PAL8
         && !p_dec->fmt_out.video.p_palette )
        {
            /* See AV_PIX_FMT_PAL8 comment in avc_GetVideoFormat(): update the
             * fmt_out palette and change the fmt_out chroma to request a new
             * vout */
            assert( p_dec->fmt_out.video.i_chroma != VLC_CODEC_RGBP );

            video_palette_t *p_palette;
            p_palette = p_dec->fmt_out.video.p_palette
                      = malloc( sizeof(video_palette_t) );
            if( !p_palette )
            {
                vlc_mutex_unlock(&p_sys->lock);
                b_error = true;
                av_frame_free(&frame);
                break;
            }
            static_assert( sizeof(p_palette->palette) == AVPALETTE_SIZE,
                           "Palette size mismatch between vlc and libavutil" );
            assert( frame->data[1] != NULL );
            lavc_Frame8PaletteCopy( p_palette, frame->data[1] );
            p_dec->fmt_out.video.i_chroma = VLC_CODEC_RGBP;
            if( decoder_UpdateVideoFormat( p_dec ) )
            {
                vlc_mutex_unlock(&p_sys->lock);
                av_frame_free(&frame);
                continue;
            }
        }

        picture_t *p_pic = FrameGetPicture( frame );
        if( p_pic == NULL )
        {   /* When direct rendering is not used, get_format() and get_buffer()
             * might not be called. The output video format must be set here
             * then picture buffer can be allocated. */
            if (p_sys->p_va == NULL
             && lavc_UpdateVideoFormat(p_dec, p_context, p_context->pix_fmt,
                                       p_context->pix_fmt) == 0
             && decoder_UpdateVideoOutput(p_dec, NULL) == 0)
                p_pic = decoder_NewPicture(p_dec);

            if( !p_pic )
            {
                vlc_mutex_unlock(&p_sys->lock);
                av_frame_free(&frame);
                break;
            }

            /* Fill picture_t from AVFrame */
            if( lavc_CopyPicture( p_dec, p_pic, frame ) != VLC_SUCCESS )
            {
                vlc_mutex_unlock(&p_sys->lock);
                av_frame_free(&frame);
                picture_Release( p_pic );
                break;
            }
        }
        else
        {
            /* Some codecs can return the same frame multiple times. By the
             * time that the same frame is returned a second time, it will be
             * too late to clone the underlying picture. So clone proactively.
             * A single picture CANNOT be queued multiple times.
             */
            p_pic = picture_Clone( p_pic );
            if( unlikely(p_pic == NULL) )
            {
                vlc_mutex_unlock(&p_sys->lock);
                av_frame_free(&frame);
                break;
            }
        }

        if( !p_dec->fmt_in->video.i_sar_num || !p_dec->fmt_in->video.i_sar_den )
        {
            /* Fetch again the aspect ratio in case it changed */
            p_dec->fmt_out.video.i_sar_num
                = p_context->sample_aspect_ratio.num;
            p_dec->fmt_out.video.i_sar_den
                = p_context->sample_aspect_ratio.den;

            if( !p_dec->fmt_out.video.i_sar_num || !p_dec->fmt_out.video.i_sar_den )
            {
                p_dec->fmt_out.video.i_sar_num = 1;
                p_dec->fmt_out.video.i_sar_den = 1;
            }
        }

        p_pic->date = i_pts;
        /* Hack to force display of still pictures */
        p_pic->b_force = p_sys->b_first_frame;
        p_pic->i_nb_fields = 2 + frame->repeat_pict;
#if LIBAVUTIL_VERSION_CHECK( 58, 7, 100 )
        p_pic->b_progressive = !(frame->flags & AV_FRAME_FLAG_INTERLACED);
        p_pic->b_top_field_first = !!(frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST);
#else
        p_pic->b_progressive = !frame->interlaced_frame;
        p_pic->b_top_field_first = frame->top_field_first;
#endif
        p_pic->b_still = p_frame_info && p_frame_info->b_eos;

        if (DecodeSidedata(p_dec, frame, p_pic))
            i_pts = VLC_TICK_INVALID;

        av_frame_free(&frame);

        /* Send decoded frame to vout */
        if (i_pts != VLC_TICK_INVALID)
        {
            p_sys->b_first_frame = false;
            vlc_mutex_unlock(&p_sys->lock);
            decoder_QueueVideo( p_dec, p_pic );
        }
        else
        {
            vlc_mutex_unlock(&p_sys->lock);
            picture_Release( p_pic );
        }
    } while( true );

    /* After draining, we need to reset decoder with a flush */
    if( b_drained )
        avcodec_flush_buffers( p_sys->p_context );

    if( p_block )
        block_Release( p_block );

    return b_error ? VLCDEC_ECRITICAL : VLCDEC_SUCCESS;
}

static int DecodeVideo( decoder_t *p_dec, block_t *p_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    block_t **pp_block = p_block ? &p_block : NULL /* drain signal */;

    if( p_block &&
        p_block->i_flags & (BLOCK_FLAG_DISCONTINUITY|BLOCK_FLAG_CORRUPTED) )
    {
        p_sys->i_late_frames = 0;
        p_sys->i_last_output_frame = -1;
        p_sys->framedrop = FRAMEDROP_NONE;

        vlc_mutex_lock(&p_sys->lock);
        date_Set( &p_sys->pts, VLC_TICK_INVALID ); /* To make sure we recover properly */
        vlc_mutex_unlock(&p_sys->lock);

        cc_Flush( &p_sys->cc );

        if( p_block->i_flags & BLOCK_FLAG_CORRUPTED )
        {
            block_Release( p_block );
            p_block = NULL; /* output only */
        }
    }

    return DecodeBlock( p_dec, pp_block );
}

/*****************************************************************************
 * EndVideo: decoder destruction
 *****************************************************************************
 * This function is called when the thread ends after a successful
 * initialization.
 *****************************************************************************/
void EndVideoDec( vlc_object_t *obj )
{
    decoder_t *p_dec = (decoder_t *)obj;
    decoder_sys_t *p_sys = p_dec->p_sys;
    AVCodecContext *ctx = p_sys->p_context;

    cc_Flush( &p_sys->cc );
    avcodec_free_context( &ctx );

    if( p_sys->p_va )
        ffmpeg_CloseVa(p_dec, NULL);

    free( p_sys );
}

/*****************************************************************************
 * ffmpeg_InitCodec: setup codec extra initialization data for ffmpeg
 *****************************************************************************/
static void ffmpeg_InitCodec( decoder_t *p_dec )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    size_t i_size = p_dec->fmt_in->i_extra;

    if( !i_size ) return;

    if( p_sys->p_codec->id == AV_CODEC_ID_SVQ3 )
    {
        uint8_t *p;

        p_sys->p_context->extradata_size = i_size + 12;
        p = p_sys->p_context->extradata =
            av_malloc( p_sys->p_context->extradata_size +
                       FF_INPUT_BUFFER_PADDING_SIZE );
        if( !p )
            return;

        memcpy( &p[0],  "SVQ3", 4 );
        memset( &p[4], 0, 8 );
        memcpy( &p[12], p_dec->fmt_in->p_extra, i_size );

        /* Now remove all atoms before the SMI one */
        if( p_sys->p_context->extradata_size > 0x5a &&
            strncmp( (char*)&p[0x56], "SMI ", 4 ) )
        {
            uint8_t *psz = &p[0x52];

            while( psz < &p[p_sys->p_context->extradata_size - 8] )
            {
                uint_fast32_t atom_size = GetDWBE( psz );
                if( atom_size <= 1 )
                {
                    /* FIXME handle 1 as long size */
                    break;
                }
                if( !strncmp( (char*)&psz[4], "SMI ", 4 ) )
                {
                    memmove( &p[0x52], psz,
                             &p[p_sys->p_context->extradata_size] - psz );
                    break;
                }

                psz += atom_size;
            }
        }
    }
    else
    {
        p_sys->p_context->extradata_size = i_size;
        p_sys->p_context->extradata =
            av_malloc( i_size + FF_INPUT_BUFFER_PADDING_SIZE );
        if( p_sys->p_context->extradata )
        {
            memcpy( p_sys->p_context->extradata,
                    p_dec->fmt_in->p_extra, i_size );
            memset( p_sys->p_context->extradata + i_size,
                    0, FF_INPUT_BUFFER_PADDING_SIZE );
        }
    }
}

static void lavc_ReleaseFrame(void *opaque, uint8_t *data)
{
    (void) data;
    picture_t *picture = opaque;

    picture_Release(picture);
}

static int lavc_va_GetFrame(struct AVCodecContext *ctx, AVFrame *frame, int flags)
{
    decoder_t *dec = ctx->opaque;
    decoder_sys_t *p_sys = dec->p_sys;
    vlc_va_t *va = p_sys->p_va;

    if(!FrameCanStoreInfo(frame))
        return -1;

    picture_t *pic;
    pic = decoder_NewPicture(dec);
    if (pic == NULL)
        return -1;

    /* data[3] will contains the format-specific surface handle. */
    if (vlc_va_Get(va, pic, ctx, frame))
    {
        msg_Err(dec, "hardware acceleration picture allocation failed");
        picture_Release(pic);
        return -1;
    }

    AVBufferRef *buf = av_buffer_create(NULL, 0, lavc_ReleaseFrame, pic, flags);
    if (unlikely(buf == NULL))
    {
        lavc_ReleaseFrame(pic, NULL);
        return -1;
    }

    /* frame->buf[0] must be valid but can be previously set by the VA module. */
    if (frame->buf[0] == NULL)
        frame->buf[0] = buf;
    else
    {
        AVBufferRef **extended_buf = av_realloc_array(frame->extended_buf,
                                                      sizeof(*extended_buf),
                                                      frame->nb_extended_buf + 1);
        if(!extended_buf)
        {
            av_buffer_unref(&buf);
            return -1;
        }
        frame->extended_buf = extended_buf;
        frame->extended_buf[frame->nb_extended_buf++] = buf;
    }

    FrameSetPicture( frame, pic );
    return 0;
}

static int lavc_dr_GetFrame(struct AVCodecContext *ctx, AVFrame *frame, int flags)
{
    decoder_t *dec = ctx->opaque;
    decoder_sys_t *sys = dec->p_sys;

    if (sys->b_dr_failure)
        return -1;

    if (ctx->pix_fmt == AV_PIX_FMT_PAL8)
        return -1;

    picture_t *pic = decoder_NewPicture(dec);
    if (pic == NULL)
        return -1;

    int width = frame->width;
    int height = frame->height;
    int aligns[AV_NUM_DATA_POINTERS];

    avcodec_align_dimensions2(ctx, &width, &height, aligns);

    /* Check that the picture is suitable for libavcodec */
    assert(pic->p[0].i_pitch >= width * pic->p[0].i_pixel_pitch);
    assert(pic->p[0].i_lines >= height);

    for (int i = 0; i < pic->i_planes; i++)
    {
        if (pic->p[i].i_pitch % aligns[i])
        {
            msg_Warn(dec, "plane %d: pitch not aligned (%d%%%d): disabling direct rendering",
                     i, pic->p[i].i_pitch, aligns[i]);
            sys->b_dr_failure = true;
            goto error;
        }
        if (((uintptr_t)pic->p[i].p_pixels) % aligns[i])
        {
            msg_Warn(dec, "plane %d not aligned: disabling direct rendering", i);
            sys->b_dr_failure = true;
            goto error;
        }
    }

    /* Allocate buffer references and initialize planes */
    assert(pic->i_planes < PICTURE_PLANE_MAX);
    static_assert(PICTURE_PLANE_MAX <= AV_NUM_DATA_POINTERS, "Oops!");

    for (int i = 0; i < pic->i_planes; i++)
    {
        uint8_t *data = pic->p[i].p_pixels;
        int size = pic->p[i].i_pitch * pic->p[i].i_lines;

        frame->data[i] = data;
        frame->linesize[i] = pic->p[i].i_pitch;
        frame->buf[i] = av_buffer_create(data, size, lavc_ReleaseFrame,
                                         pic, flags);
        if (unlikely(frame->buf[i] == NULL))
        {
            while (i > 0)
                av_buffer_unref(&frame->buf[--i]);
            goto error;
        }
        picture_Hold(pic);
    }

    FrameSetPicture( frame, pic );
    /* The loop above held one reference to the picture for each plane. */
    assert(pic->i_planes > 0);
    picture_Release(pic);
    return 0;
error:
    picture_Release(pic);
    return -1;
}

/**
 * Callback used by libavcodec to get a frame buffer.
 *
 * It is used for direct rendering as well as to get the right PTS for each
 * decoded picture (even in indirect rendering mode).
 */
static int lavc_GetFrame(struct AVCodecContext *ctx, AVFrame *frame, int flags)
{
    decoder_t *dec = ctx->opaque;
    decoder_sys_t *sys = dec->p_sys;

    for (unsigned i = 0; i < AV_NUM_DATA_POINTERS; i++)
    {
        frame->data[i] = NULL;
        frame->linesize[i] = 0;
        frame->buf[i] = NULL;
    }
    FrameSetPicture( frame, NULL );

    if (sys->p_va == NULL)
    {
        if (!sys->b_direct_rendering)
            return avcodec_default_get_buffer2(ctx, frame, flags);

        vlc_mutex_lock(&sys->lock);
        /* Most unaccelerated decoders do not call get_format(), so we need to
         * update the output video format here. The MT semaphore must be held
         * to protect p_dec->fmt_out. */
        if (lavc_UpdateVideoFormat(dec, ctx, ctx->pix_fmt, ctx->pix_fmt) ||
            decoder_UpdateVideoOutput(dec, NULL))
        {
            vlc_mutex_unlock(&sys->lock);
            return -1;
        }
        vlc_mutex_unlock(&sys->lock);
    }

    if (sys->p_va != NULL)
    {
        int ret = lavc_va_GetFrame(ctx, frame, flags);
        return ret;
    }

    /* Some codecs set pix_fmt only after the 1st frame has been decoded,
     * so we need to check for direct rendering again. */
    int ret = lavc_dr_GetFrame(ctx, frame, flags);
    if (ret)
        ret = avcodec_default_get_buffer2(ctx, frame, flags);
    return ret;
}

static enum AVPixelFormat ffmpeg_GetFormat( AVCodecContext *p_context,
                                          const enum AVPixelFormat *pi_fmt )
{
    decoder_t *p_dec = p_context->opaque;
    decoder_sys_t *p_sys = p_dec->p_sys;
    video_format_t fmt;

    /* Enumerate available formats */
    enum AVPixelFormat defaultfmt = avcodec_default_get_format(p_context, pi_fmt);
    enum AVPixelFormat swfmt = AV_PIX_FMT_NONE;
    enum AVPixelFormat skipfmt = AV_PIX_FMT_NONE;
    bool can_hwaccel = false;

    for (size_t i = 0; pi_fmt[i] != AV_PIX_FMT_NONE; i++)
    {
        const AVPixFmtDescriptor *dsc = av_pix_fmt_desc_get(pi_fmt[i]);
        if (dsc == NULL)
            continue;
        bool hwaccel = (dsc->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0;

        msg_Dbg( p_dec, "available %sware decoder output format %d (%s)",
                 hwaccel ? "hard" : "soft", pi_fmt[i], dsc->name );
        if (hwaccel)
        {
            /* The default fmt is a hw format, it can happen with some va
             * implementations (when using a hw_device_ctx). */
            if (defaultfmt == pi_fmt[i])
                defaultfmt = AV_PIX_FMT_NONE;

            can_hwaccel = true;
        }
        else if (swfmt == AV_PIX_FMT_NONE && !p_sys->b_hardware_only)
            swfmt = pi_fmt[i];
    }

    /* Use the default fmt in priority of any sw fmt if the default fmt is a hw
     * one */
    if (defaultfmt != AV_PIX_FMT_NONE)
    {
        if (p_sys->b_hardware_only)
        {
            if (defaultfmt != p_context->sw_pix_fmt)
            {
                // the source format changed and we didn't detect it
                vlc_assert_unreachable();
            }
        }
        swfmt = defaultfmt;
    }

    if (p_sys->pix_fmt == AV_PIX_FMT_NONE)
        goto no_reuse;

    /* If the format did not actually change (e.g. seeking), try to reuse the
     * existing output format, and if present, hardware acceleration back-end.
     * This avoids resetting the pipeline downstream. This also avoids
     * needlessly probing for hardware acceleration support. */
     if (lavc_GetVideoFormat(p_dec, &fmt, p_context, p_sys->pix_fmt, swfmt) != 0)
     {
         msg_Dbg(p_dec, "get format failed");
         goto no_reuse;
     }
     if (fmt.i_width  != p_sys->decoder_width ||
         fmt.i_height != p_sys->decoder_height)
     {
         msg_Dbg(p_dec, "mismatched dimensions %ux%u was %ux%u", fmt.i_width, fmt.i_height,
                 p_sys->decoder_width, p_sys->decoder_height);
         goto no_reuse;
     }
     if (p_context->profile != p_sys->profile || p_context->level > p_sys->level)
     {
         msg_Dbg(p_dec, "mismatched profile level %d/%d was %d/%d", p_context->profile,
                 p_context->level, p_sys->profile, p_sys->level);
         goto no_reuse;
     }

     for (size_t i = 0; pi_fmt[i] != AV_PIX_FMT_NONE; i++)
        if (pi_fmt[i] == p_sys->pix_fmt)
        {
            if (p_sys->use_hwframes)
            {
                /* When using hwframes API, ffmpeg is reseting the hardware
                 * context of the AVCodecContext before calling the
                 * `get_format` callback. Therefore, the va context need to be
                 * closed and opened again. */
                msg_Dbg(p_dec, "recreating decoder output format %d", pi_fmt[i]);
                int ret = ffmpeg_RecreateVa(p_dec, p_context, swfmt);
                if (ret != VLC_SUCCESS)
                {
                    msg_Err(p_dec, "error restarting va module");
                    skipfmt = p_sys->pix_fmt; /* Don't retry this fmt */
                    p_sys->pix_fmt = AV_PIX_FMT_NONE;
                    goto no_reuse;
                }
            }
            else
                msg_Dbg(p_dec, "reusing decoder output format %d", pi_fmt[i]);
            return p_sys->pix_fmt;
        }

no_reuse:
    if (p_sys->p_va != NULL)
    {
        msg_Err(p_dec, "existing hardware acceleration cannot be reused");
        // the decoder changes have to be handled outside of lavc so that
        // switching to a software decoder will not silently decode nothing
        // (get_format will fail to use AV_PIX_FMT_NONE)
        assert(!p_sys->b_hardware_only);
        ffmpeg_CloseVa(p_dec, p_context);
    }

    p_sys->profile = p_context->profile;
    p_sys->level = p_context->level;

    if (!can_hwaccel)
        return swfmt;

    const AVPixFmtDescriptor *src_desc = av_pix_fmt_desc_get(swfmt);

    for( size_t i = 0; hwfmts[i] != AV_PIX_FMT_NONE; i++ )
    {
        enum AVPixelFormat hwfmt = AV_PIX_FMT_NONE;
        for( size_t j = 0; hwfmt == AV_PIX_FMT_NONE && pi_fmt[j] != AV_PIX_FMT_NONE; j++ )
            if( hwfmts[i] == pi_fmt[j] )
                hwfmt = hwfmts[i];

        if (hwfmt == skipfmt)
            continue;

        vlc_decoder_device *dec_device;
        int ret = lavc_UpdateHWVideoFormat(p_dec, p_context, hwfmt, swfmt,
                                           &dec_device);
        if (ret != VLC_SUCCESS)
            continue;

        ret = ffmpeg_OpenVa(p_dec, p_context, hwfmt, src_desc, dec_device);
        if (dec_device != NULL)
            vlc_decoder_device_Release(dec_device);

        if (ret != VLC_SUCCESS)
            continue;

        return hwfmt;
    }

    /* Fallback to default behaviour */
    p_sys->pix_fmt = swfmt;
    return swfmt;
}
