/*****************************************************************************
 * mpeg_system.h: structures of the input used to parse MPEG-1, MPEG-2 PS
 * and TS system layers
 *****************************************************************************
 * Copyright (C) 1999, 2000 VideoLAN
 * $Id: mpeg_system.h,v 1.3 2001/03/21 13:42:33 sam Exp $
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Constants
 *****************************************************************************/
#define TS_PACKET_SIZE      188                       /* Size of a TS packet */
#define PSI_SECTION_SIZE    4096            /* Maximum size of a PSI section */

#define PAT_UNINITIALIZED    (1 << 6)
#define PMT_UNINITIALIZED    (1 << 6)

#define PSI_IS_PAT          0x00
#define PSI_IS_PMT          0x01
#define UNKNOWN_PSI         0xff

/*****************************************************************************
 * psi_section_t
 *****************************************************************************
 * Describes a PSI section. Beware, it doesn't contain pointers to the TS
 * packets that contain it as for a PES, but the data themselves
 *****************************************************************************/
typedef struct psi_section_s
{
    byte_t                  buffer[PSI_SECTION_SIZE];

    u8                      i_section_number;
    u8                      i_last_section_number;
    u8                      i_version_number;
    u16                     i_section_length;
    u16                     i_read_in_section;
    
    /* the PSI is complete */
    boolean_t               b_is_complete;
    
    /* packet missed up ? */
    boolean_t               b_trash;

    /*about sections  */ 
    boolean_t               b_section_complete;

    /* where are we currently ? */
    byte_t                * p_current;

} psi_section_t;

/*****************************************************************************
 * es_ts_data_t: extension of es_descriptor_t
 *****************************************************************************/
typedef struct es_ts_data_s
{
    boolean_t               b_psi;   /* Does the stream have to be handled by
                                      *                    the PSI decoder ? */

    int                     i_psi_type;  /* There are different types of PSI */
    
    psi_section_t *         p_psi_section;                    /* PSI packets */

    /* Markers */
    int                     i_continuity_counter;
} es_ts_data_t;

/*****************************************************************************
 * pgrm_ts_data_t: extension of pgrm_descriptor_t
 *****************************************************************************/
typedef struct pgrm_ts_data_s
{
    u16                     i_pcr_pid;             /* PCR ES, for TS streams */
    int                     i_pmt_version;
} pgrm_ts_data_t;

/*****************************************************************************
 * stream_ts_data_t: extension of stream_descriptor_t
 *****************************************************************************/
typedef struct stream_ts_data_s
{
    int i_pat_version;          /* Current version of the PAT */
} stream_ts_data_t;

/*****************************************************************************
 * stream_ps_data_t: extension of stream_descriptor_t
 *****************************************************************************/
typedef struct stream_ps_data_s
{
    boolean_t               b_has_PSM;                 /* very rare, in fact */

    u8                      i_PSM_version;
} stream_ps_data_t;

/* PSM version is 5 bits, so -1 is not a valid value */
#define EMPTY_PSM_VERSION   -1


/*****************************************************************************
 * Prototypes
 *****************************************************************************/
void input_ParsePES( struct input_thread_s *, struct es_descriptor_s * );
void input_GatherPES( struct input_thread_s *, struct data_packet_s *,
                      struct es_descriptor_s *, boolean_t, boolean_t );
es_descriptor_t * input_ParsePS( struct input_thread_s *,
                                 struct data_packet_s * );
void input_DemuxPS( struct input_thread_s *, struct data_packet_s * );
void input_DemuxTS( struct input_thread_s *, struct data_packet_s * );
void input_DemuxPSI( input_thread_t *, data_packet_t *, es_descriptor_t *, 
                     boolean_t, boolean_t );
