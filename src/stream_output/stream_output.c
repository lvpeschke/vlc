/*****************************************************************************
 * stream_output.c : stream output module
 *****************************************************************************
 * Copyright (C) 2002-2004 the VideoLAN team
 * $Id$
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *          Laurent Aimar <fenrir@via.ecp.fr>
 *          Eric Petit <titer@videolan.org>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#include <vlc/vlc.h>

#include <stdlib.h>                                                /* free() */
#include <stdio.h>                                              /* sprintf() */
#include <string.h>                                            /* strerror() */

#include <vlc_sout.h>

#include "stream_output.h"

#include <vlc_meta.h>

#include "input/input_internal.h"

#undef DEBUG_BUFFER
/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
#define sout_stream_url_to_chain( p, s ) \
    _sout_stream_url_to_chain( VLC_OBJECT(p), s )
static char *_sout_stream_url_to_chain( vlc_object_t *, char * );

/*
 * Generic MRL parser
 *
 */

typedef struct
{
    char *psz_access;
    char *psz_way;
    char *psz_name;
} mrl_t;

/* mrl_Parse: parse psz_mrl and fill p_mrl */
static int  mrl_Parse( mrl_t *p_mrl, const char *psz_mrl );
/* mrl_Clean: clean p_mrl  after a call to mrl_Parse */
static void mrl_Clean( mrl_t *p_mrl );

/*****************************************************************************
 * sout_NewInstance: creates a new stream output instance
 *****************************************************************************/
sout_instance_t *__sout_NewInstance( vlc_object_t *p_parent, char * psz_dest )
{
    sout_instance_t *p_sout;
    vlc_value_t keep;

    if( var_Get( p_parent, "sout-keep", &keep ) < 0 )
    {
        msg_Warn( p_parent, "cannot get sout-keep value" );
        keep.b_bool = VLC_FALSE;
    }
    if( keep.b_bool )
    {
        if( ( p_sout = vlc_object_find( p_parent, VLC_OBJECT_SOUT,
                                        FIND_ANYWHERE ) ) != NULL )
        {
            if( !strcmp( p_sout->psz_sout, psz_dest ) )
            {
                msg_Dbg( p_parent, "sout keep: reusing sout" );
                msg_Dbg( p_parent, "sout keep: you probably want to use "
                          "gather stream_out" );
                vlc_object_detach( p_sout );
                vlc_object_attach( p_sout, p_parent );
                vlc_object_release( p_sout );
                return p_sout;
            }
            else
            {
                msg_Dbg( p_parent, "sout keep: destroying unusable sout" );
                vlc_object_release( p_sout );
                sout_DeleteInstance( p_sout );
            }
        }
    }
    else if( !keep.b_bool )
    {
        while( ( p_sout = vlc_object_find( p_parent, VLC_OBJECT_SOUT,
                                           FIND_PARENT ) ) != NULL )
        {
            msg_Dbg( p_parent, "sout keep: destroying old sout" );
            vlc_object_release( p_sout );
            sout_DeleteInstance( p_sout );
        }
    }

    /* *** Allocate descriptor *** */
    p_sout = vlc_object_create( p_parent, VLC_OBJECT_SOUT );
    if( p_sout == NULL )
    {
        msg_Err( p_parent, "out of memory" );
        return NULL;
    }

    /* *** init descriptor *** */
    p_sout->psz_sout    = strdup( psz_dest );
    p_sout->p_meta      = NULL;
    p_sout->i_out_pace_nocontrol = 0;
    p_sout->p_sys       = NULL;

    vlc_mutex_init( p_sout, &p_sout->lock );
    if( psz_dest && psz_dest[0] == '#' )
    {
        p_sout->psz_chain = strdup( &psz_dest[1] );
    }
    else
    {
        p_sout->psz_chain = sout_stream_url_to_chain( p_sout, psz_dest );
        msg_Dbg( p_sout, "using sout chain=`%s'", p_sout->psz_chain );
    }
    p_sout->p_stream = NULL;

    /* attach it for inherit */
    vlc_object_attach( p_sout, p_parent );

    p_sout->p_stream = sout_StreamNew( p_sout, p_sout->psz_chain );

    if( p_sout->p_stream == NULL )
    {
        msg_Err( p_sout, "stream chain failed for `%s'", p_sout->psz_chain );

        FREENULL( p_sout->psz_sout );
        FREENULL( p_sout->psz_chain );

        vlc_object_detach( p_sout );
        vlc_object_destroy( p_sout );
        return NULL;
    }

    return p_sout;
}

/*****************************************************************************
 * sout_DeleteInstance: delete a previously allocated instance
 *****************************************************************************/
void sout_DeleteInstance( sout_instance_t * p_sout )
{
    /* Unlink object */
    vlc_object_detach( p_sout );

    /* remove the stream out chain */
    sout_StreamDelete( p_sout->p_stream );

    /* *** free all string *** */
    FREENULL( p_sout->psz_sout );
    FREENULL( p_sout->psz_chain );

    /* delete meta */
    if( p_sout->p_meta )
    {
        vlc_meta_Delete( p_sout->p_meta );
    }

    vlc_mutex_destroy( &p_sout->lock );

    /* *** free structure *** */
    vlc_object_destroy( p_sout );
}

/*****************************************************************************
 * Packetizer/Input
 *****************************************************************************/
sout_packetizer_input_t *sout_InputNew( sout_instance_t *p_sout,
                                        es_format_t *p_fmt )
{
    sout_packetizer_input_t *p_input;

    msg_Dbg( p_sout, "adding a new input" );

    /* *** create a packetizer input *** */
    p_input         = malloc( sizeof( sout_packetizer_input_t ) );
    p_input->p_sout = p_sout;
    p_input->p_fmt  = p_fmt;

    if( p_fmt->i_codec == VLC_FOURCC( 'n', 'u', 'l', 'l' ) )
    {
        vlc_object_release( p_sout );
        return p_input;
    }

    /* *** add it to the stream chain */
    vlc_mutex_lock( &p_sout->lock );
    p_input->id = p_sout->p_stream->pf_add( p_sout->p_stream, p_fmt );
    vlc_mutex_unlock( &p_sout->lock );

    if( p_input->id == NULL )
    {
        free( p_input );
        return NULL;
    }

    return( p_input );
}

/*****************************************************************************
 *
 *****************************************************************************/
int sout_InputDelete( sout_packetizer_input_t *p_input )
{
    sout_instance_t     *p_sout = p_input->p_sout;

    msg_Dbg( p_sout, "removing an input" );

    if( p_input->p_fmt->i_codec != VLC_FOURCC( 'n', 'u', 'l', 'l' ) )
    {
        vlc_mutex_lock( &p_sout->lock );
        p_sout->p_stream->pf_del( p_sout->p_stream, p_input->id );
        vlc_mutex_unlock( &p_sout->lock );
    }

    free( p_input );

    return( VLC_SUCCESS);
}

/*****************************************************************************
 *
 *****************************************************************************/
int sout_InputSendBuffer( sout_packetizer_input_t *p_input,
                          block_t *p_buffer )
{
    sout_instance_t     *p_sout = p_input->p_sout;
    int                 i_ret;

    if( p_input->p_fmt->i_codec == VLC_FOURCC( 'n', 'u', 'l', 'l' ) )
    {
        block_Release( p_buffer );
        return VLC_SUCCESS;
    }
    if( p_buffer->i_dts <= 0 )
    {
        msg_Warn( p_sout, "trying to send non-dated packet to stream output!");
        block_Release( p_buffer );
        return VLC_SUCCESS;
    }

    vlc_mutex_lock( &p_sout->lock );
    i_ret = p_sout->p_stream->pf_send( p_sout->p_stream,
                                       p_input->id, p_buffer );
    vlc_mutex_unlock( &p_sout->lock );

    return i_ret;
}

/*****************************************************************************
 * sout_AccessOutNew: allocate a new access out
 *****************************************************************************/
sout_access_out_t *sout_AccessOutNew( sout_instance_t *p_sout,
                                      char *psz_access, char *psz_name )
{
    sout_access_out_t *p_access;
    char              *psz_next;

    if( !( p_access = vlc_object_create( p_sout,
                                         sizeof( sout_access_out_t ) ) ) )
    {
        msg_Err( p_sout, "out of memory" );
        return NULL;
    }

    psz_next = config_ChainCreate( &p_access->psz_access, &p_access->p_cfg,
                                   psz_access );
    if( psz_next )
    {
        free( psz_next );
    }
    p_access->psz_name   = strdup( psz_name ? psz_name : "" );
    p_access->p_sout     = p_sout;
    p_access->p_sys = NULL;
    p_access->pf_seek    = NULL;
    p_access->pf_read    = NULL;
    p_access->pf_write   = NULL;
    p_access->p_module   = NULL;

    p_access->i_writes = 0;
    p_access->i_sent_bytes = 0;

    vlc_object_attach( p_access, p_sout );

    p_access->p_module   =
        module_Need( p_access, "sout access", p_access->psz_access, VLC_TRUE );

    if( !p_access->p_module )
    {
        free( p_access->psz_access );
        free( p_access->psz_name );
        vlc_object_detach( p_access );
        vlc_object_destroy( p_access );
        return( NULL );
    }

    return p_access;
}
/*****************************************************************************
 * sout_AccessDelete: delete an access out
 *****************************************************************************/
void sout_AccessOutDelete( sout_access_out_t *p_access )
{
    vlc_object_detach( p_access );
    if( p_access->p_module )
    {
        module_Unneed( p_access, p_access->p_module );
    }
    free( p_access->psz_access );

    config_ChainDestroy( p_access->p_cfg );

    free( p_access->psz_name );

    vlc_object_destroy( p_access );
}

/*****************************************************************************
 * sout_AccessSeek:
 *****************************************************************************/
int sout_AccessOutSeek( sout_access_out_t *p_access, off_t i_pos )
{
    return p_access->pf_seek( p_access, i_pos );
}

/*****************************************************************************
 * sout_AccessRead:
 *****************************************************************************/
int sout_AccessOutRead( sout_access_out_t *p_access, block_t *p_buffer )
{
    return( p_access->pf_read ?
            p_access->pf_read( p_access, p_buffer ) : VLC_EGENERIC );
}

/*****************************************************************************
 * sout_AccessWrite:
 *****************************************************************************/
int sout_AccessOutWrite( sout_access_out_t *p_access, block_t *p_buffer )
{
    int i_total = 0;
    p_access->i_writes++;
    p_access->i_sent_bytes += p_buffer->i_buffer;
    if( p_access->p_libvlc->b_stats && p_access->i_writes % 30 == 0 )
    {
        /* Access_out -> sout_instance -> input_thread_t */
        input_thread_t *p_input =
            (input_thread_t *)vlc_object_find( p_access, VLC_OBJECT_INPUT,
                                               FIND_PARENT );
        if( p_input )
        {
            stats_UpdateInteger( p_input, p_input->p->counters.p_sout_sent_packets,
			         30, NULL );
            stats_UpdateInteger( p_input, p_input->p->counters.p_sout_sent_bytes,
                                 p_access->i_sent_bytes, &i_total );
            stats_UpdateFloat( p_input, p_input->p->counters.p_sout_send_bitrate,
			   	 (float)i_total, NULL );
            p_access->i_sent_bytes = 0;
            vlc_object_release( p_input );
        }
    }
    return p_access->pf_write( p_access, p_buffer );
}

/*****************************************************************************
 * sout_MuxNew: create a new mux
 *****************************************************************************/
sout_mux_t * sout_MuxNew( sout_instance_t *p_sout, char *psz_mux,
                          sout_access_out_t *p_access )
{
    sout_mux_t *p_mux;
    char       *psz_next;

    p_mux = vlc_object_create( p_sout, sizeof( sout_mux_t ) );
    if( p_mux == NULL )
    {
        msg_Err( p_sout, "out of memory" );
        return NULL;
    }

    p_mux->p_sout = p_sout;
    psz_next = config_ChainCreate( &p_mux->psz_mux, &p_mux->p_cfg, psz_mux );
    if( psz_next ) free( psz_next );

    p_mux->p_access     = p_access;
    p_mux->pf_control   = NULL;
    p_mux->pf_addstream = NULL;
    p_mux->pf_delstream = NULL;
    p_mux->pf_mux       = NULL;
    p_mux->i_nb_inputs  = 0;
    p_mux->pp_inputs    = NULL;

    p_mux->p_sys        = NULL;
    p_mux->p_module     = NULL;

    p_mux->b_add_stream_any_time = VLC_FALSE;
    p_mux->b_waiting_stream = VLC_TRUE;
    p_mux->i_add_stream_start = -1;

    vlc_object_attach( p_mux, p_sout );

    p_mux->p_module =
        module_Need( p_mux, "sout mux", p_mux->psz_mux, VLC_TRUE );

    if( p_mux->p_module == NULL )
    {
        FREENULL( p_mux->psz_mux );

        vlc_object_detach( p_mux );
        vlc_object_destroy( p_mux );
        return NULL;
    }

    /* *** probe mux capacity *** */
    if( p_mux->pf_control )
    {
        int b_answer = VLC_FALSE;

        if( sout_MuxControl( p_mux, MUX_CAN_ADD_STREAM_WHILE_MUXING,
                             &b_answer ) )
        {
            b_answer = VLC_FALSE;
        }

        if( b_answer )
        {
            msg_Dbg( p_sout, "muxer support adding stream at any time" );
            p_mux->b_add_stream_any_time = VLC_TRUE;
            p_mux->b_waiting_stream = VLC_FALSE;

            /* If we control the output pace then it's better to wait before
             * starting muxing (generates better streams/files). */
            if( !p_sout->i_out_pace_nocontrol )
            {
                b_answer = VLC_TRUE;
            }
            else if( sout_MuxControl( p_mux, MUX_GET_ADD_STREAM_WAIT,
                                      &b_answer ) )
            {
                b_answer = VLC_FALSE;
            }

            if( b_answer )
            {
                msg_Dbg( p_sout, "muxer prefers to wait for all ES before "
                         "starting to mux" );
                p_mux->b_waiting_stream = VLC_TRUE;
            }
        }
    }

    return p_mux;
}

/*****************************************************************************
 * sout_MuxDelete:
 *****************************************************************************/
void sout_MuxDelete( sout_mux_t *p_mux )
{
    vlc_object_detach( p_mux );
    if( p_mux->p_module )
    {
        module_Unneed( p_mux, p_mux->p_module );
    }
    free( p_mux->psz_mux );

    config_ChainDestroy( p_mux->p_cfg );

    vlc_object_destroy( p_mux );
}

/*****************************************************************************
 * sout_MuxAddStream:
 *****************************************************************************/
sout_input_t *sout_MuxAddStream( sout_mux_t *p_mux, es_format_t *p_fmt )
{
    sout_input_t *p_input;

    if( !p_mux->b_add_stream_any_time && !p_mux->b_waiting_stream )
    {
        msg_Err( p_mux, "cannot add a new stream (unsupported while muxing "
                        "to this format)" );
        return NULL;
    }

    msg_Dbg( p_mux, "adding a new input" );

    /* create a new sout input */
    p_input = malloc( sizeof( sout_input_t ) );
    p_input->p_sout = p_mux->p_sout;
    p_input->p_fmt  = p_fmt;
    p_input->p_fifo = block_FifoNew( p_mux->p_sout );
    p_input->p_sys  = NULL;

    TAB_APPEND( p_mux->i_nb_inputs, p_mux->pp_inputs, p_input );
    if( p_mux->pf_addstream( p_mux, p_input ) < 0 )
    {
            msg_Err( p_mux, "cannot add this stream" );
            TAB_REMOVE( p_mux->i_nb_inputs, p_mux->pp_inputs, p_input );
            block_FifoRelease( p_input->p_fifo );
            free( p_input );
            return NULL;
    }

    return p_input;
}

/*****************************************************************************
 * sout_MuxDeleteStream:
 *****************************************************************************/
void sout_MuxDeleteStream( sout_mux_t *p_mux, sout_input_t *p_input )
{
    int i_index;

    if( p_mux->b_waiting_stream && p_input->p_fifo->i_depth > 0 )
    {
        /* We stop waiting, and call the muxer for taking care of the data
         * before we remove this es */
        p_mux->b_waiting_stream = VLC_FALSE;
        p_mux->pf_mux( p_mux );
    }

    TAB_FIND( p_mux->i_nb_inputs, p_mux->pp_inputs, p_input, i_index );
    if( i_index >= 0 )
    {
        if( p_mux->pf_delstream( p_mux, p_input ) < 0 )
        {
            msg_Err( p_mux, "cannot delete this stream from mux" );
        }

        /* remove the entry */
        TAB_REMOVE( p_mux->i_nb_inputs, p_mux->pp_inputs, p_input );

        if( p_mux->i_nb_inputs == 0 )
        {
            msg_Warn( p_mux, "no more input streams for this mux" );
        }

        block_FifoRelease( p_input->p_fifo );
        free( p_input );
    }
}

/*****************************************************************************
 * sout_MuxSendBuffer:
 *****************************************************************************/
void sout_MuxSendBuffer( sout_mux_t *p_mux, sout_input_t *p_input,
                         block_t *p_buffer )
{
    block_FifoPut( p_input->p_fifo, p_buffer );

    if( p_mux->p_sout->i_out_pace_nocontrol )
    {
        mtime_t current_date = mdate();
        if ( current_date > p_buffer->i_dts )
            msg_Warn( p_mux, "late buffer for mux input ("I64Fd")",
                      current_date - p_buffer->i_dts );
    }

    if( p_mux->b_waiting_stream )
    {
        if( p_mux->i_add_stream_start < 0 )
        {
            p_mux->i_add_stream_start = p_buffer->i_dts;
        }

        if( p_mux->i_add_stream_start >= 0 &&
            p_mux->i_add_stream_start + I64C(1500000) < p_buffer->i_dts )
        {
            /* Wait until we have more than 1.5 seconds worth of data
             * before start muxing */
            p_mux->b_waiting_stream = VLC_FALSE;
        }
        else
        {
            return;
        }
    }
    p_mux->pf_mux( p_mux );
}

/*****************************************************************************
 *
 *****************************************************************************/
static int mrl_Parse( mrl_t *p_mrl, const char *psz_mrl )
{
    char * psz_dup = strdup( psz_mrl );
    char * psz_parser = psz_dup;
    const char * psz_access;
    const char * psz_way;
    char * psz_name;

    /* *** first parse psz_dest */
    while( *psz_parser && *psz_parser != ':' )
    {
        if( *psz_parser == '{' )
        {
            while( *psz_parser && *psz_parser != '}' )
            {
                psz_parser++;
            }
            if( *psz_parser )
            {
                psz_parser++;
            }
        }
        else
        {
            psz_parser++;
        }
    }
#if defined( WIN32 ) || defined( UNDER_CE )
    if( psz_parser - psz_dup == 1 )
    {
        /* msg_Warn( p_sout, "drive letter %c: found in source string",
                          *psz_dup ) ; */
        psz_parser = "";
    }
#endif

    if( !*psz_parser )
    {
        psz_access = psz_way = "";
        psz_name = psz_dup;
    }
    else
    {
        *psz_parser++ = '\0';

        /* let's skip '//' */
        if( psz_parser[0] == '/' && psz_parser[1] == '/' )
        {
            psz_parser += 2 ;
        }

        psz_name = psz_parser ;

        /* Come back to parse the access and mux plug-ins */
        psz_parser = psz_dup;

        if( !*psz_parser )
        {
            /* No access */
            psz_access = "";
        }
        else if( *psz_parser == '/' )
        {
            /* No access */
            psz_access = "";
            psz_parser++;
        }
        else
        {
            psz_access = psz_parser;

            while( *psz_parser && *psz_parser != '/' )
            {
                if( *psz_parser == '{' )
                {
                    while( *psz_parser && *psz_parser != '}' )
                    {
                        psz_parser++;
                    }
                    if( *psz_parser )
                    {
                        psz_parser++;
                    }
                }
                else
                {
                    psz_parser++;
                }
            }

            if( *psz_parser == '/' )
            {
                *psz_parser++ = '\0';
            }
        }

        if( !*psz_parser )
        {
            /* No mux */
            psz_way = "";
        }
        else
        {
            psz_way = psz_parser;
        }
    }

    p_mrl->psz_access = strdup( psz_access );
    p_mrl->psz_way    = strdup( psz_way );
    p_mrl->psz_name   = strdup( psz_name );

    free( psz_dup );
    return( VLC_SUCCESS );
}


/* mrl_Clean: clean p_mrl  after a call to mrl_Parse */
static void mrl_Clean( mrl_t *p_mrl )
{
    FREENULL( p_mrl->psz_access );
    FREENULL( p_mrl->psz_way );
    FREENULL( p_mrl->psz_name );
}


/****************************************************************************
 ****************************************************************************
 **
 **
 **
 ****************************************************************************
 ****************************************************************************/

/* create a complete chain */
/* chain format:
    module{option=*:option=*}[:module{option=*:...}]
 */

/*
 * parse module{options=str, option="str "}:
 *  return a pointer on the rest
 *  XXX: psz_chain is modified
 */
#define SKIPSPACE( p ) { while( *p && ( *p == ' ' || *p == '\t' ) ) p++; }
#define SKIPTRAILINGSPACE( p, e ) \
    { while( e > p && ( *(e-1) == ' ' || *(e-1) == '\t' ) ) e--; }

/* go accross " " and { } */
static char *_get_chain_end( char *str )
{
    char c, *p = str;

    SKIPSPACE( p );

    for( ;; )
    {
        if( !*p || *p == ',' || *p == '}' ) return p;

        if( *p != '{' && *p != '"' && *p != '\'' )
        {
            p++;
            continue;
        }

        if( *p == '{' ) c = '}';
        else c = *p;
        p++;

        for( ;; )
        {
            if( !*p ) return p;

            if( *p == c ) return ++p;
            else if( *p == '{' && c == '}' ) p = _get_chain_end( p );
            else p++;
        }
    }
}

/*
 * XXX name and p_cfg are used (-> do NOT free them)
 */
sout_stream_t *sout_StreamNew( sout_instance_t *p_sout, char *psz_chain )
{
    sout_stream_t *p_stream;

    if( !psz_chain )
    {
        msg_Err( p_sout, "invalid chain" );
        return NULL;
    }

    p_stream = vlc_object_create( p_sout, sizeof( sout_stream_t ) );

    if( !p_stream )
    {
        msg_Err( p_sout, "out of memory" );
        return NULL;
    }

    p_stream->p_sout   = p_sout;
    p_stream->p_sys    = NULL;

    p_stream->psz_next =
        config_ChainCreate( &p_stream->psz_name, &p_stream->p_cfg, psz_chain);

    msg_Dbg( p_sout, "stream=`%s'", p_stream->psz_name );

    vlc_object_attach( p_stream, p_sout );

    p_stream->p_module =
        module_Need( p_stream, "sout stream", p_stream->psz_name, VLC_TRUE );

    if( !p_stream->p_module )
    {
        sout_StreamDelete( p_stream );
        return NULL;
    }

    return p_stream;
}

void sout_StreamDelete( sout_stream_t *p_stream )
{
    msg_Dbg( p_stream, "destroying chain... (name=%s)", p_stream->psz_name );

    vlc_object_detach( p_stream );
    if( p_stream->p_module ) module_Unneed( p_stream, p_stream->p_module );

    FREENULL( p_stream->psz_name );
    FREENULL( p_stream->psz_next );

    config_ChainDestroy( p_stream->p_cfg );

    msg_Dbg( p_stream, "destroying chain done" );
    vlc_object_destroy( p_stream );
}

static char *_sout_stream_url_to_chain( vlc_object_t *p_this, char *psz_url )
{
    mrl_t       mrl;
    char        *psz_chain, *p;

    mrl_Parse( &mrl, psz_url );
    p = psz_chain = malloc( 500 + strlen( mrl.psz_way ) +
                                  strlen( mrl.psz_access ) +
                                  strlen( mrl.psz_name ) );


    if( config_GetInt( p_this, "sout-display" ) )
    {
        p += sprintf( p, "duplicate{dst=display,dst=std{mux=\"%s\","
                      "access=\"%s\",dst=\"%s\"}}",
                      mrl.psz_way, mrl.psz_access, mrl.psz_name );
    }
    else
    {
        p += sprintf( p, "std{mux=\"%s\",access=\"%s\",dst=\"%s\"}",
                      mrl.psz_way, mrl.psz_access, mrl.psz_name );
    }

    mrl_Clean( &mrl );
    return( psz_chain );
}
