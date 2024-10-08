/*****************************************************************************
 * preparser.h
 *****************************************************************************
 * Copyright (C) 1999-2023 VLC authors and VideoLAN
 *
 * Authors: Samuel Hocevar <sam@zoy.org>
 *          Clément Stenac <zorglub@videolan.org>
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

#ifndef VLC_PREPARSER_H
#define VLC_PREPARSER_H 1

#include <vlc_input_item.h>

/**
 * @defgroup vlc_preparser Preparser
 * @ingroup input
 * @{
 * @file
 * VLC Preparser API
 */

/**
 * Preparser opaque structure.
 *
 * The preparser object will retrieve the meta data of any given input item in
 * an asynchronous way.
 * It will also issue art fetching requests.
 */
typedef struct vlc_preparser_t vlc_preparser_t;

typedef enum input_item_meta_request_option_t
{
    META_REQUEST_OPTION_NONE          = 0x00,
    META_REQUEST_OPTION_SCOPE_LOCAL   = 0x01,
    META_REQUEST_OPTION_SCOPE_NETWORK = 0x02,
    META_REQUEST_OPTION_SCOPE_ANY     =
        META_REQUEST_OPTION_SCOPE_LOCAL|META_REQUEST_OPTION_SCOPE_NETWORK,
    META_REQUEST_OPTION_SCOPE_FORCED  = 0x04,
    META_REQUEST_OPTION_FETCH_LOCAL   = 0x08,
    META_REQUEST_OPTION_FETCH_NETWORK = 0x10,
    META_REQUEST_OPTION_FETCH_ANY     =
        META_REQUEST_OPTION_FETCH_LOCAL|META_REQUEST_OPTION_FETCH_NETWORK,
    META_REQUEST_OPTION_DO_INTERACT   = 0x20,
    META_REQUEST_OPTION_PARSE_SUBITEMS = 0x40,
} input_item_meta_request_option_t;

/* status of the on_preparse_ended() callback */
enum input_item_preparse_status
{
    ITEM_PREPARSE_SKIPPED,
    ITEM_PREPARSE_FAILED,
    ITEM_PREPARSE_TIMEOUT,
    ITEM_PREPARSE_DONE
};

struct vlc_metadata_cbs {
    void (*on_preparse_ended)(input_item_t *, enum input_item_preparse_status status, void *userdata);
    void (*on_art_fetch_ended)(input_item_t *, bool fetched, void *userdata);
    void (*on_subtree_added)(input_item_t *, input_item_node_t *subtree, void *userdata);
    void (*on_attachments_added)(input_item_t *item,
                                 input_attachment_t *const *array,
                                 size_t count, void *userdata);
};

/**
 * This function creates the preparser object and thread.
 *
 * @param obj the parent object
 * @return a valid preparser object or NULL in case of error
 */
VLC_API vlc_preparser_t *vlc_preparser_New( vlc_object_t *obj );

/**
 * This function enqueues the provided item to be preparsed or fetched.
 *
 * The input item is retained until the preparsing is done or until the
 * preparser object is deleted.
 *
 * @param preparser the preparser object
 * @param item a valid item to preparse
 * @param option preparse flag, cf @ref input_item_meta_request_option_t
 * @param cbs callback to listen to events (can't be NULL)
 * @param cbs_userdata opaque pointer used by the callbacks
 * @param timeout maximum time allowed to preparse the item. If -1, the default
 * "preparse-timeout" option will be used as a timeout. If 0, it will wait
 * indefinitely. If > 0, the timeout will be used (in milliseconds).
 * @param id unique id provided by the caller. This is can be used to cancel
 * the request with vlc_preparser_Cancel()
 * @returns VLC_SUCCESS if the item was scheduled for preparsing, an error code
 * otherwise
 * If this returns an error, the on_preparse_ended will *not* be invoked
 */
VLC_API int vlc_preparser_Push( vlc_preparser_t *preparser, input_item_t *item,
                                input_item_meta_request_option_t option,
                                const struct vlc_metadata_cbs *cbs,
                                void *cbs_userdata,
                                int timeout, void *id );

/**
 * This function cancel all preparsing requests for a given id
 *
 * @param preparser the preparser object
 * @param id unique id given to vlc_preparser_Push()
 */
VLC_API void vlc_preparser_Cancel( vlc_preparser_t *preparser, void *id );

/**
 * This function destroys the preparser object and thread.
 *
 * @param preparser the preparser object
 * All pending input items will be released.
 */
VLC_API void vlc_preparser_Delete( vlc_preparser_t *preparser );

/**
 * This function deactivates the preparser
 *
 * All pending requests will be removed, and it will block until the currently
 * running entity has finished (if any).
 *
 * @param preparser the preparser object
 */
VLC_API void vlc_preparser_Deactivate( vlc_preparser_t *preparser );

/**
 * Recover the main preparser from libvlc
 *
 * @param libvlc valid libvlc object
 * @return the main preparser (can be NULL)
 */
VLC_API vlc_preparser_t *libvlc_GetMainPreparser(libvlc_int_t *libvlc);

/** @} vlc_preparser */

#endif

