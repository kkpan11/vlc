/*****************************************************************************
 * transforms_yuvmmx.c: MMX YUV transformation functions
 * Provides functions to perform the YUV conversion.
 *****************************************************************************
 * Copyright (C) 1999, 2000 VideoLAN
 * $Id: transforms_yuvmmx.c,v 1.4 2001/03/21 13:42:34 sam Exp $
 *
 * Authors: Samuel Hocevar <sam@zoy.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include "defs.h"

#include <math.h>                                            /* exp(), pow() */
#include <errno.h>                                                 /* ENOMEM */
#include <stdlib.h>                                                /* free() */
#include <string.h>                                            /* strerror() */

#include "config.h"
#include "common.h"
#include "threads.h"
#include "mtime.h"

#include "video.h"
#include "video_output.h"

#include "video_common.h"
#include "transforms_common.h"
#include "transforms_yuvmmx.h"

#include "intf_msg.h"

/*****************************************************************************
 * ConvertY4Gray8: grayscale YUV 4:x:x to RGB 8 bpp
 *****************************************************************************/
void ConvertY4Gray8( YUV_ARGS_8BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, grayscale, bpp = 8" );
}

/*****************************************************************************
 * ConvertYUV420RGB8: color YUV 4:2:0 to RGB 8 bpp
 *****************************************************************************/
void ConvertYUV420RGB8( YUV_ARGS_8BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, chroma = 420, bpp = 8" );
}

/*****************************************************************************
 * ConvertYUV422RGB8: color YUV 4:2:2 to RGB 8 bpp
 *****************************************************************************/
void ConvertYUV422RGB8( YUV_ARGS_8BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, chroma = 422, bpp = 8" );
}

/*****************************************************************************
 * ConvertYUV444RGB8: color YUV 4:4:4 to RGB 8 bpp
 *****************************************************************************/
void ConvertYUV444RGB8( YUV_ARGS_8BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, chroma = 444, bpp = 8" );
}

/*****************************************************************************
 * ConvertY4Gray16: color YUV 4:4:4 to RGB 2 Bpp
 *****************************************************************************/
void ConvertY4Gray16( YUV_ARGS_16BPP )
{
    boolean_t   b_horizontal_scaling;             /* horizontal scaling type */
    int         i_vertical_scaling;                 /* vertical scaling type */
    int         i_x, i_y;                 /* horizontal and vertical indexes */
    int         i_scale_count;                       /* scale modulo counter */
    int         i_chroma_width;                              /* chroma width */
    u16 *       p_pic_start;       /* beginning of the current line for copy */
    u16 *       p_buffer_start;                   /* conversion buffer start */
    u16 *       p_buffer;                       /* conversion buffer pointer */
    int *       p_offset_start;                        /* offset array start */
    int *       p_offset;                            /* offset array pointer */

    /*
     * Initialize some values  - i_pic_line_width will store the line skip
     */
    i_pic_line_width -= i_pic_width;
    i_chroma_width =    i_width / 2;
    p_buffer_start =    p_vout->yuv.p_buffer;
    p_offset_start =    p_vout->yuv.p_offset;
    SetOffset( i_width, i_height, i_pic_width, i_pic_height,
               &b_horizontal_scaling, &i_vertical_scaling, p_offset_start, 0 );

    /*
     * Perform conversion
     */
    i_scale_count = ( i_vertical_scaling == 1 ) ? i_pic_height : i_height;
    for( i_y = 0; i_y < i_height; i_y++ )
    {
        /* Mark beginnning of line for possible later line copy, and initialize
         * buffer */
        p_pic_start =   p_pic;
        p_buffer =      b_horizontal_scaling ? p_buffer_start : p_pic;

        for ( i_x = i_width / 8; i_x--; )
        {
            __asm__( MMX_INIT_16_GRAY
                     : : "r" (p_y), "r" (p_u), "r" (p_v), "r" (p_buffer) );

            __asm__( ".align 8"
                     MMX_YUV_GRAY
                     MMX_UNPACK_16_GRAY
                     : : "r" (p_y), "r" (p_u), "r" (p_v), "r" (p_buffer) );

            p_y += 8;
            p_u += 4;
            p_v += 4;
            p_buffer += 8;
        }

        SCALE_WIDTH;
        SCALE_HEIGHT( 420, 2 );
    }
}

/*****************************************************************************
 * ConvertYUV420RGB16: color YUV 4:2:0 to RGB 2 Bpp
 *****************************************************************************/
void ConvertYUV420RGB16( YUV_ARGS_16BPP )
{
    boolean_t   b_horizontal_scaling;             /* horizontal scaling type */
    int         i_vertical_scaling;                 /* vertical scaling type */
    int         i_x, i_y;                 /* horizontal and vertical indexes */
    int         i_scale_count;                       /* scale modulo counter */
    int         i_chroma_width;                              /* chroma width */
    u16 *       p_pic_start;       /* beginning of the current line for copy */
    u16 *       p_buffer_start;                   /* conversion buffer start */
    u16 *       p_buffer;                       /* conversion buffer pointer */
    int *       p_offset_start;                        /* offset array start */
    int *       p_offset;                            /* offset array pointer */

    /*
     * Initialize some values  - i_pic_line_width will store the line skip
     */
    i_pic_line_width -= i_pic_width;
    i_chroma_width =    i_width / 2;
    p_buffer_start =    p_vout->yuv.p_buffer;
    p_offset_start =    p_vout->yuv.p_offset;
    SetOffset( i_width, i_height, i_pic_width, i_pic_height,
               &b_horizontal_scaling, &i_vertical_scaling, p_offset_start, 0 );

    /*
     * Perform conversion
     */
    i_scale_count = ( i_vertical_scaling == 1 ) ? i_pic_height : i_height;
    for( i_y = 0; i_y < i_height; i_y++ )
    {
        p_pic_start =   p_pic;
        p_buffer =      b_horizontal_scaling ? p_buffer_start : p_pic;

        for ( i_x = i_width / 8; i_x--; )
        {
            __asm__( MMX_INIT_16
                     : : "r" (p_y), "r" (p_u), "r" (p_v), "r" (p_buffer) );

            __asm__( ".align 8"
                     MMX_YUV_MUL
                     MMX_YUV_ADD
                     MMX_UNPACK_16
                     : : "r" (p_y), "r" (p_u), "r" (p_v), "r" (p_buffer) );

            p_y += 8;
            p_u += 4;
            p_v += 4;
            p_buffer += 8;
        }
        SCALE_WIDTH;
        SCALE_HEIGHT( 420, 2 );
      }
}


/*****************************************************************************
 * ConvertYUV422RGB16: color YUV 4:2:2 to RGB 2 Bpp
 *****************************************************************************/
void ConvertYUV422RGB16( YUV_ARGS_16BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, chroma = 422, bpp = 16" );
}

/*****************************************************************************
 * ConvertYUV444RGB16: color YUV 4:4:4 to RGB 2 Bpp
 *****************************************************************************/
void ConvertYUV444RGB16( YUV_ARGS_16BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, chroma = 444, bpp = 16" );
}

/*****************************************************************************
 * ConvertY4Gray24: grayscale YUV 4:x:x to RGB 2 Bpp
 *****************************************************************************/
void ConvertY4Gray24( YUV_ARGS_24BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, grayscale, bpp = 24" );
}

/*****************************************************************************
 * ConvertYUV420RGB24: color YUV 4:2:0 to RGB 2 Bpp
 *****************************************************************************/
void ConvertYUV420RGB24( YUV_ARGS_24BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, chroma = 420, bpp = 24" );
}

/*****************************************************************************
 * ConvertYUV422RGB24: color YUV 4:2:2 to RGB 2 Bpp
 *****************************************************************************/
void ConvertYUV422RGB24( YUV_ARGS_24BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, chroma = 422, bpp = 24" );
}

/*****************************************************************************
 * ConvertYUV444RGB24: color YUV 4:4:4 to RGB 2 Bpp
 *****************************************************************************/
void ConvertYUV444RGB24( YUV_ARGS_24BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, chroma = 444, bpp = 24" );
}

/*****************************************************************************
 * ConvertY4Gray32: grayscale YUV 4:x:x to RGB 4 Bpp
 *****************************************************************************/
void ConvertY4Gray32( YUV_ARGS_32BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, grayscale, bpp = 32" );
}


/*****************************************************************************
 * ConvertYUV420RGB32: color YUV 4:2:0 to RGB 4 Bpp
 *****************************************************************************/
void ConvertYUV420RGB32( YUV_ARGS_32BPP )
{
    boolean_t   b_horizontal_scaling;             /* horizontal scaling type */
    int         i_vertical_scaling;                 /* vertical scaling type */
    int         i_x, i_y;                 /* horizontal and vertical indexes */
    int         i_scale_count;                       /* scale modulo counter */
    int         i_chroma_width;                              /* chroma width */
    u32 *       p_pic_start;       /* beginning of the current line for copy */
    u32 *       p_buffer_start;                   /* conversion buffer start */
    u32 *       p_buffer;                       /* conversion buffer pointer */
    int *       p_offset_start;                        /* offset array start */
    int *       p_offset;                            /* offset array pointer */

    /*
     * Initialize some values  - i_pic_line_width will store the line skip
     */
    i_pic_line_width -= i_pic_width;
    i_chroma_width =    i_width / 2;
    p_buffer_start =    p_vout->yuv.p_buffer;
    p_offset_start =    p_vout->yuv.p_offset;
    SetOffset( i_width, i_height, i_pic_width, i_pic_height,
               &b_horizontal_scaling, &i_vertical_scaling, p_offset_start, 0 );

    i_scale_count = ( i_vertical_scaling == 1 ) ? i_pic_height : i_height;
    for( i_y = 0; i_y < i_height; i_y++ )
    {
        p_pic_start =   p_pic;
        p_buffer =      b_horizontal_scaling ? p_buffer_start : p_pic;

        for ( i_x = i_width / 8; i_x--; )
        {
            __asm__( ".align 8"
                     MMX_INIT_32
                     : : "r" (p_y), "r" (p_u), "r" (p_v), "r" (p_buffer) );

            __asm__( ".align 8"
                     MMX_YUV_MUL
                     MMX_YUV_ADD
                     MMX_UNPACK_32
                     : : "r" (p_y), "r" (p_u), "r" (p_v), "r" (p_buffer) );

            p_y += 8;
            p_u += 4;
            p_v += 4;
            p_buffer += 8;
        }

        SCALE_WIDTH;
        SCALE_HEIGHT( 420, 4 );
    }

}

/*****************************************************************************
 * ConvertYUV422RGB32: color YUV 4:2:2 to RGB 4 Bpp
 *****************************************************************************/
void ConvertYUV422RGB32( YUV_ARGS_32BPP )
{
    intf_ErrMsg( "yuv error: unhandled function, chroma = 422, bpp = 32" );
}

/*****************************************************************************
 * ConvertYUV444RGB32: color YUV 4:4:4 to RGB 4 Bpp
 *****************************************************************************/
void ConvertYUV444RGB32( YUV_ARGS_32BPP )
{
    intf_ErrMsg( "yuv error: unhandled function, chroma = 444, bpp = 32" );
}

/*****************************************************************************
 * ConvertYUV420YCbr8: color YUV 4:2:0 to YCbr 8 Bpp
 *****************************************************************************/

void ConvertYUV420YCbr8    ( YUV_ARGS_8BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, chroma = 420, YCbr = 8" );
}
/*****************************************************************************
 * ConvertYUV422YCbr8: color YUV 4:2:2 to YCbr 8 Bpp
 *****************************************************************************/

void ConvertYUV422YCbr8    ( YUV_ARGS_8BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, chroma = 422, YCbr = 8" );

}
/*****************************************************************************
 * ConvertYUV444YCbr8: color YUV 4:4:4 to YCbr 8 Bpp
 *****************************************************************************/
void ConvertYUV444YCbr8    ( YUV_ARGS_8BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, chroma = 444, YCbr = 8" );

}
/*****************************************************************************
 * ConvertYUV420YCbr16: color YUV 4:2:0 to YCbr 16 Bpp
 *****************************************************************************/
void ConvertYUV420YCbr16    ( YUV_ARGS_16BPP )
{
    boolean_t   b_horizontal_scaling;             /* horizontal scaling type */
    int         i_vertical_scaling;                 /* vertical scaling type */
    int         i_x, i_y;                 /* horizontal and vertical indexes */
    int         i_scale_count;                       /* scale modulo counter */
    int         i_chroma_width;                              /* chroma width */
    u16 *       p_pic_start;       /* beginning of the current line for copy */
    u16 *       p_buffer_start;                   /* conversion buffer start */
    u16 *       p_buffer;                       /* conversion buffer pointer */
    int *       p_offset_start;                        /* offset array start */
    int *       p_offset;                            /* offset array pointer */

    i_pic_line_width -= i_pic_width;
    i_chroma_width =    i_width / 2;
    p_buffer_start =    p_vout->yuv.p_buffer;
    p_offset_start =    p_vout->yuv.p_offset;
    SetOffset( i_width, i_height, i_pic_width, i_pic_height,
               &b_horizontal_scaling, &i_vertical_scaling, p_offset_start, 0 );

    i_scale_count = ( i_vertical_scaling == 1 ) ? i_pic_height : i_height;
    for( i_y = 0; i_y < i_height; i_y++ )
    {
        p_pic_start =   p_pic;
        p_buffer =      b_horizontal_scaling ? p_buffer_start : p_pic;

        for ( i_x = i_width / 8; i_x--; )
        {
            __asm__( MMX_INIT_16
                     : : "r" (p_y), "r" (p_u), "r" (p_v), "r" (p_buffer) );

            __asm__( ".align 8"
                     MMX_YUV_YCBR_422
                     : : "r" (p_y), "r" (p_u), "r" (p_v), "r" (p_buffer) );

            p_y += 8;
            p_u += 4;
            p_v += 4;
            p_buffer += 8;
        }
        SCALE_WIDTH;
        SCALE_HEIGHT( 420, 2 );
  
      }

}
/*****************************************************************************
 * ConvertYUV422YCbr8: color YUV 4:2:2 to YCbr 16 Bpp
 *****************************************************************************/

void ConvertYUV422YCbr16    ( YUV_ARGS_16BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, chroma = 422, YCbr = 16" );

}
/*****************************************************************************
 * ConvertYUV424YCbr8: color YUV 4:4:4 to YCbr 16 Bpp
 *****************************************************************************/

void ConvertYUV444YCbr16    ( YUV_ARGS_16BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, chroma = 444, YCbr = 16" );

}
/*****************************************************************************
 * ConvertYUV420YCbr24: color YUV 4:2:0 to YCbr 24 Bpp
 *****************************************************************************/

void ConvertYUV420YCbr24    ( YUV_ARGS_24BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, chroma = 420, YCbr = 24" );

}
/*****************************************************************************
 * ConvertYUV422YCbr24: color YUV 4:2:2 to YCbr 24 Bpp
 *****************************************************************************/

void ConvertYUV422YCbr24    ( YUV_ARGS_24BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, chroma = 422, YCbr = 24" );

}
/*****************************************************************************
 * ConvertYUV444YCbr24: color YUV 4:4:4 to YCbr 24 Bpp
 *****************************************************************************/

void ConvertYUV444YCbr24    ( YUV_ARGS_24BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, chroma = 444, YCbr = 24" );

}
/*****************************************************************************
 * ConvertYUV420YCbr32: color YUV 4:2:0 to YCbr 32 Bpp
 *****************************************************************************/

void ConvertYUV420YCbr32    ( YUV_ARGS_32BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, chroma = 420, YCbr = 32" );

}
/*****************************************************************************
 * ConvertYUV422YCbr32: color YUV 4:2:2 to YCbr 32 Bpp
 *****************************************************************************/

void ConvertYUV422YCbr32    ( YUV_ARGS_32BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, chroma = 422, YCbr = 32" );

}
/*****************************************************************************
 * ConvertYUV444YCbr32: color YUV 4:4:4 to YCbr 32 Bpp
 *****************************************************************************/
void ConvertYUV444YCbr32    ( YUV_ARGS_32BPP )
{
    intf_ErrMsg( "yuvmmx error: unhandled function, chroma = 444, YCbr = 32" );

}


