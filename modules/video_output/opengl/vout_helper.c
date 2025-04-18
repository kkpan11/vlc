/*****************************************************************************
 * vout_helper.c: OpenGL and OpenGL ES output common code
 *****************************************************************************
 * Copyright (C) 2004-2016 VLC authors and VideoLAN
 * Copyright (C) 2009, 2011 Laurent Aimar
 *
 * Authors: Laurent Aimar <fenrir _AT_ videolan _DOT_ org>
 *          Ilkka Ollakka <ileoo@videolan.org>
 *          Rémi Denis-Courmont
 *          Adrien Maglo <magsoft at videolan dot org>
 *          Felix Paul Kühne <fkuehne at videolan dot org>
 *          Pierre d'Herbemont <pdherbemont at videolan dot org>
 *          Alexandre Janniaux <ajanni@videolabs.io>
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
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>
#include <math.h>

#include <vlc_common.h>
#include <vlc_configuration.h>
#include <vlc_list.h>
#include <vlc_subpicture.h>
#include <vlc_opengl.h>
#include <vlc_modules.h>
#include <vlc_vout.h>
#include <vlc_viewpoint.h>

#include "filters.h"
#include "gl_api.h"
#include "gl_util.h"
#include "vout_helper.h"
#include "renderer.h"
#include "sub_renderer.h"

struct vout_display_opengl_t {

    vlc_gl_t   *gl;
    struct vlc_gl_api api;

    struct vlc_gl_interop *interop;
    struct vlc_gl_renderer *renderer; /**< weak reference */
    struct vlc_gl_filter *renderer_filter; /**< weak reference */

    struct vlc_gl_filters *filters;

    struct vlc_gl_interop *sub_interop;
    struct vlc_gl_sub_renderer *sub_renderer;

    vlc_viewpoint_t viewpoint;
    video_projection_mode_t projection;
    vlc_stereoscopic_mode_t stereo_mode;
};

static const vlc_fourcc_t gl_subpicture_chromas[] = {
    VLC_CODEC_RGBA,
    0
};

static void
ResizeFormatToGLMaxTexSize(video_format_t *fmt, unsigned int max_tex_size)
{
    if (fmt->i_width > fmt->i_height)
    {
        unsigned int const  vis_w = fmt->i_visible_width;
        unsigned int const  vis_h = fmt->i_visible_height;
        unsigned int const  nw_w = max_tex_size;
        unsigned int const  nw_vis_w = nw_w * vis_w / fmt->i_width;

        fmt->i_height = nw_w * fmt->i_height / fmt->i_width;
        fmt->i_width = nw_w;
        fmt->i_visible_height = nw_vis_w * vis_h / vis_w;
        fmt->i_visible_width = nw_vis_w;
    }
    else
    {
        unsigned int const  vis_w = fmt->i_visible_width;
        unsigned int const  vis_h = fmt->i_visible_height;
        unsigned int const  nw_h = max_tex_size;
        unsigned int const  nw_vis_h = nw_h * vis_h / fmt->i_height;

        fmt->i_width = nw_h * fmt->i_width / fmt->i_height;
        fmt->i_height = nw_h;
        fmt->i_visible_width = nw_vis_h * vis_w / vis_h;
        fmt->i_visible_height = nw_vis_h;
    }
}

static struct vlc_gl_filters *
CreateFilters(vlc_gl_t *gl, const struct vlc_gl_api *api,
              struct vlc_gl_interop *interop,
              struct vlc_gl_filter **out_renderer)
{
    struct vlc_gl_filters *filters = vlc_gl_filters_New(gl, api, interop, gl->orientation);
    if (!filters)
    {
        msg_Err(gl, "Could not create filters");
        return NULL;
    }

    const opengl_vtable_t *vt = &api->vt;
    const video_format_t *fmt_in = &interop->fmt_in;

    int upscaler = var_InheritInteger(gl, "gl-upscaler");
    int downscaler = var_InheritInteger(gl, "gl-downscaler");
    int has_dovi = fmt_in->dovi.rpu_present && !fmt_in->dovi.el_present; /* can't handle EL yet */

    if (upscaler || downscaler || has_dovi)
    {
        char upscaler_value[12];
        char downscaler_value[12];

        snprintf(upscaler_value, sizeof(upscaler_value), "%d", upscaler);
        snprintf(downscaler_value, sizeof(downscaler_value), "%d", downscaler);
        upscaler_value[sizeof(upscaler_value) - 1] = '\0';
        downscaler_value[sizeof(downscaler_value) - 1] = '\0';

        config_chain_t cfg = {
            .psz_name = (char *) "upscaler",
            .psz_value = upscaler_value,
            .p_next = &(config_chain_t) {
                .psz_name = (char *) "downscaler",
                .psz_value = downscaler_value,
            },
        };

        struct vlc_gl_filter *scale_filter =
            vlc_gl_filters_Append(filters, "pl_scale", &cfg);
        if (!scale_filter)
        {
            if (upscaler)
                msg_Err(gl, "Could not apply upscaler filter, "
                            "ignoring --gl-upscaler=%d", upscaler);
            if (downscaler)
                msg_Err(gl, "Could not apply downscaler filter, "
                            "ignoring --gl-downscaler=%d", downscaler);
        }
    }

    struct vlc_gl_filter *renderer_filter =
        vlc_gl_filters_Append(filters, "renderer", NULL);
    if (!renderer_filter)
    {
        msg_Warn(gl, "Could not create renderer for %4.4s",
                 (const char *) &fmt_in->i_chroma);
        goto error;
    }
    GL_ASSERT_NOERROR(vt);

    int ret = vlc_gl_filters_InitFramebuffers(filters);
    if (ret != VLC_SUCCESS)
    {
        msg_Err(gl, "Could not init filters framebuffers");
        goto error;
    }

    /* The renderer is a special filter: we need its concrete instance to
     * forward SetViewpoint() */
    *out_renderer = renderer_filter;

    return filters;

error:
    vlc_gl_filters_Delete(filters);
    return NULL;
}

struct renderer_config
{
    video_projection_mode_t projection;
    vlc_stereoscopic_mode_t stereo_mode;
};

static int RestartRenderer(vout_display_opengl_t *vgl,
                           const struct renderer_config *config)
{
    config_chain_t chain_projection = {
        .psz_name = strdup("projection-mode"),
        .psz_value = strdup(config->projection == PROJECTION_MODE_RECTANGULAR ? "0" : "-1"),
    };

    config_chain_t chain_stereo = {
        .psz_name = strdup("video-stereo-mode"),
        .p_next = &chain_projection,
    };
    if (chain_projection.psz_name == NULL ||
        chain_projection.psz_value == NULL ||
        chain_stereo.psz_name == NULL ||
        asprintf(&chain_stereo.psz_value, "%d", (int)config->stereo_mode) == -1)
    {
        free(chain_projection.psz_name);
        free(chain_projection.psz_value);
        free(chain_stereo.psz_name);
        free(chain_stereo.psz_value);
        return VLC_ENOMEM;
    }

    struct vlc_gl_filter *new_renderer =
        vlc_gl_filters_Replace(vgl->filters, vgl->renderer_filter, "renderer", &chain_stereo);
    free(chain_projection.psz_name);
    free(chain_projection.psz_value);
    free(chain_stereo.psz_name);
    free(chain_stereo.psz_value);
    if (new_renderer == NULL)
    {
        msg_Err(vgl->gl, "Could not re-create renderer filter for projection=%d stereo_mode=%d",
                (int)config->projection, (int)config->stereo_mode);
        return VLC_EGENERIC;
    }

    int ret = vlc_gl_filters_InitFramebuffers(vgl->filters);
    if (ret != VLC_SUCCESS)
    {
        msg_Err(vgl->gl, "Could not init filters framebuffers");
        return VLC_EGENERIC;
    }

    vgl->renderer_filter = new_renderer;
    vgl->renderer = new_renderer->sys;

    vlc_gl_renderer_SetViewpoint(vgl->renderer, &vgl->viewpoint);
    return VLC_SUCCESS;
}

int vout_display_opengl_ChangeProjection(vout_display_opengl_t *vgl,
                                         video_projection_mode_t projection)
{
    struct renderer_config config = {
        .projection = projection,
        .stereo_mode = vgl->stereo_mode,
    };
    int ret = RestartRenderer(vgl, &config);
    if (ret != VLC_SUCCESS)
        return ret;

    vgl->projection = projection;
    msg_Dbg(vgl->gl, "Changed to projection mode %d", projection);
    return VLC_SUCCESS;
}

int vout_display_opengl_ChangeStereoMode(vout_display_opengl_t *vgl,
                                         vlc_stereoscopic_mode_t stereo_mode)
{
    struct renderer_config config = {
        .projection = vgl->projection,
        .stereo_mode = stereo_mode,
    };
    int ret = RestartRenderer(vgl, &config);
    if (ret != VLC_SUCCESS)
        return ret;

    vgl->stereo_mode = stereo_mode;
    msg_Dbg(vgl->gl, "Changed to stereo mode %d", stereo_mode);
    return VLC_SUCCESS;
}

vout_display_opengl_t *vout_display_opengl_New(video_format_t *fmt,
                                               const vlc_fourcc_t **subpicture_chromas,
                                               vlc_gl_t *gl,
                                               const vlc_viewpoint_t *viewpoint,
                                               vlc_video_context *context)
{
    vout_display_opengl_t *vgl = calloc(1, sizeof(*vgl));
    if (!vgl)
        return NULL;

    vgl->gl = gl;

    int ret = vlc_gl_api_Init(&vgl->api, gl);
    if (ret != VLC_SUCCESS)
        goto free_vgl;

    const struct vlc_gl_api *api = &vgl->api;
    const opengl_vtable_t *vt = &api->vt;

#if !defined(USE_OPENGL_ES2)
    const unsigned char *ogl_version = vt->GetString(GL_VERSION);
    bool supports_shaders = strverscmp((const char *)ogl_version, "2.0") >= 0;
    if (!supports_shaders)
    {
        msg_Err(gl, "shaders not supported, bailing out");
        goto free_vgl;
    }
#endif

    /* Resize the format if it is greater than the maximum texture size
     * supported by the hardware */
    GLint       max_tex_size;
    vt->GetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex_size);

    if ((GLint)fmt->i_width > max_tex_size ||
        (GLint)fmt->i_height > max_tex_size)
        ResizeFormatToGLMaxTexSize(fmt, max_tex_size);

    vgl->interop = vlc_gl_interop_New(gl, context, fmt);
    if (!vgl->interop)
    {
        msg_Err(gl, "Could not create interop");
        goto free_vgl;
    }
    GL_ASSERT_NOERROR(vt);

    vgl->filters = CreateFilters(gl, api, vgl->interop, &vgl->renderer_filter);
    if (!vgl->filters)
    {
        msg_Err(gl, "Could not create filters");
        goto delete_interop;
    }
    vgl->renderer = vgl->renderer_filter->sys;
    GL_ASSERT_NOERROR(vt);

    vgl->sub_interop = vlc_gl_interop_NewForSubpictures(gl);
    if (!vgl->sub_interop)
    {
        msg_Err(gl, "Could not create sub interop");
        goto delete_filters;
    }
    GL_ASSERT_NOERROR(vt);

    vgl->sub_renderer =
        vlc_gl_sub_renderer_New(gl, api, vgl->sub_interop);
    if (!vgl->sub_renderer)
    {
        msg_Err(gl, "Could not create sub renderer");
        goto delete_sub_interop;
    }

    GL_ASSERT_NOERROR(vt);

    vlc_viewpoint_init(&vgl->viewpoint);
    if (fmt->projection_mode != PROJECTION_MODE_RECTANGULAR
     && vout_display_opengl_SetViewpoint(vgl, viewpoint) != VLC_SUCCESS)
        msg_Err(gl, "Could not set viewpoint");

    int projection_mode = var_InheritInteger(gl, "projection-mode");
    if (projection_mode == PROJECTION_MODE_RECTANGULAR ||
        projection_mode == PROJECTION_MODE_EQUIRECTANGULAR ||
        projection_mode == PROJECTION_MODE_CUBEMAP_LAYOUT_STANDARD)
        vgl->projection = projection_mode;
    else
        vgl->projection = fmt->projection_mode;

    int stereo_mode = var_InheritInteger(gl, "video-stereo-mode");
    if (stereo_mode > 0 && stereo_mode <= VIDEO_STEREO_OUTPUT_MAX)
        vgl->stereo_mode = stereo_mode;
    else
        vgl->stereo_mode = VIDEO_STEREO_OUTPUT_AUTO;

    /* Forward to the core the changes to the input format requested by the
     * interop */
    video_format_Clean(fmt);
    video_format_Copy(fmt, &vgl->interop->fmt_in);

    if (subpicture_chromas) {
        *subpicture_chromas = gl_subpicture_chromas;
    }

    GL_ASSERT_NOERROR(vt);
    return vgl;

delete_sub_interop:
    vlc_gl_interop_Delete(vgl->sub_interop);
delete_filters:
    vlc_gl_filters_Delete(vgl->filters);
delete_interop:
    vlc_gl_interop_Delete(vgl->interop);
free_vgl:
    free(vgl);

    return NULL;
}

int vout_display_opengl_UpdateFormat(vout_display_opengl_t *vgl,
                                     const video_format_t *fmt,
                                     vlc_video_context *vctx)
{
    /* If the format can't be changed, the state must remain valid to accept the
     * initial format. */

    vlc_gl_t *gl = vgl->gl;
    const struct vlc_gl_api *api = &vgl->api;

    assert(!fmt->p_palette);

    struct vlc_gl_interop *interop = vlc_gl_interop_New(gl, vctx, fmt);
    if (!interop)
    {
        msg_Err(gl, "Could not create interop");
        return VLC_EGENERIC;
    }

    if ( !video_format_IsSameChroma( &interop->fmt_in, fmt ) )
    {
        msg_Warn(gl, "Could not update format, the interop changed the "
                     "requested chroma from %4.4s to %4.4s\n",
                     (char *) &fmt->i_chroma, (char *) &interop->fmt_in.i_chroma);
        vlc_gl_interop_Delete(interop);
        return VLC_EGENERIC;
    }

    struct vlc_gl_filter *renderer;
    struct vlc_gl_filters *filters = CreateFilters(gl, api, interop, &renderer);
    if (!filters)
    {
        vlc_gl_interop_Delete(interop);
        return VLC_EGENERIC;
    }

    /* We created everything necessary, it worked, now the old ones could be
     * replaced. */
    vlc_gl_filters_Delete(vgl->filters);
    vlc_gl_interop_Delete(vgl->interop);

    vgl->interop = interop;
    vgl->filters = filters;
    vgl->renderer = renderer->sys;
    vgl->renderer_filter = renderer;

    vlc_gl_renderer_SetViewpoint(vgl->renderer, &vgl->viewpoint);

    return VLC_SUCCESS;
}

void vout_display_opengl_Delete(vout_display_opengl_t *vgl)
{
    const opengl_vtable_t *vt = &vgl->api.vt;

    GL_ASSERT_NOERROR(vt);

    /* */
    vt->Finish();
    vt->Flush();

    vlc_gl_sub_renderer_Delete(vgl->sub_renderer);
    vlc_gl_interop_Delete(vgl->sub_interop);

    vlc_gl_filters_Delete(vgl->filters);
    vlc_gl_interop_Delete(vgl->interop);

    GL_ASSERT_NOERROR(vt);

    free(vgl);
}

int vout_display_opengl_SetViewpoint(vout_display_opengl_t *vgl,
                                     const vlc_viewpoint_t *p_vp)
{
    int ret = vlc_gl_renderer_SetViewpoint(vgl->renderer, p_vp);
    if (ret == VLC_SUCCESS)
        vgl->viewpoint = *p_vp;
    return ret;
}

void vout_display_opengl_SetOutputSize(vout_display_opengl_t *vgl,
                                       unsigned width, unsigned height)
{
    int ret = vlc_gl_filters_SetOutputSize(vgl->filters, width, height);
    /* The renderer, last filter in the chain, necessarily accepts the new
     * output size */
    assert(ret == VLC_SUCCESS);
    vlc_gl_sub_renderer_SetOutputSize(vgl->sub_renderer, width, height);
    (void) ret;
}

void vout_display_opengl_Viewport(vout_display_opengl_t *vgl, int x, int y,
                                  unsigned width, unsigned height)
{
    vlc_gl_filters_SetViewport(vgl->filters, x, y, width, height);
}

int vout_display_opengl_Prepare(vout_display_opengl_t *vgl,
                                picture_t *picture,
                                const vlc_render_subpicture *subpicture)
{
    GL_ASSERT_NOERROR(&vgl->api.vt);

    int ret = vlc_gl_filters_UpdatePicture(vgl->filters, picture);
    if (ret != VLC_SUCCESS)
        return ret;

    ret = vlc_gl_sub_renderer_Prepare(vgl->sub_renderer, subpicture);
    GL_ASSERT_NOERROR(&vgl->api.vt);
    return ret;
}
int vout_display_opengl_Display(vout_display_opengl_t *vgl)
{
    GL_ASSERT_NOERROR(&vgl->api.vt);

    int ret = vlc_gl_filters_Draw(vgl->filters);
    if (ret != VLC_SUCCESS)
        return ret;

    ret = vlc_gl_sub_renderer_Draw(vgl->sub_renderer);
    if (ret != VLC_SUCCESS)
        return ret;

    GL_ASSERT_NOERROR(&vgl->api.vt);

    return VLC_SUCCESS;
}
