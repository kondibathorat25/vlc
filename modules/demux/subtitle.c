/*****************************************************************************
 * subtitle.c: Demux for subtitle text files.
 *****************************************************************************
 * Copyright (C) 1999-2007 the VideoLAN team
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Derk-Jan Hartman <hartman at videolan dot org>
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

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc/vlc.h>
#include <vlc_plugin.h>
#include <vlc_input.h>

#include <errno.h>
#ifdef HAVE_SYS_TYPES_H
#   include <sys/types.h>
#endif
#include <ctype.h>

#include <vlc_demux.h>
#include <vlc_charset.h>

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open ( vlc_object_t *p_this );
static void Close( vlc_object_t *p_this );

#define SUB_DELAY_LONGTEXT \
    N_("Apply a delay to all subtitles (in 1/10s, eg 100 means 10s).")
#define SUB_FPS_LONGTEXT \
    N_("Override the normal frames per second settings. " \
    "This will only work with MicroDVD and SubRIP (SRT) subtitles.")
#define SUB_TYPE_LONGTEXT \
    N_("Force the subtiles format. Valid values are : \"microdvd\", " \
    "\"subrip\",  \"ssa1\", \"ssa2-4\", \"ass\", \"vplayer\" " \
    "\"sami\", \"dvdsubtitle\", \"mpl2\", \"aqt\", \"pjs\" and \"auto\" (meaning autodetection, this " \
    "should always work).")
static const char *ppsz_sub_type[] =
{
    "auto", "microdvd", "subrip", "subviewer", "ssa1",
    "ssa2-4", "ass", "vplayer", "sami", "dvdsubtitle", "mpl2",
    "aqt", "pjs"
};

vlc_module_begin();
    set_shortname( _("Subtitles"));
    set_description( _("Text subtitles parser") );
    set_capability( "demux", 0 );
    set_category( CAT_INPUT );
    set_subcategory( SUBCAT_INPUT_DEMUX );
    add_float( "sub-fps", 0.0, NULL,
               N_("Frames per second"),
               SUB_FPS_LONGTEXT, true );
    add_integer( "sub-delay", 0, NULL,
               N_("Subtitles delay"),
               SUB_DELAY_LONGTEXT, true );
    add_string( "sub-type", "auto", NULL, N_("Subtitles format"),
                SUB_TYPE_LONGTEXT, true );
        change_string_list( ppsz_sub_type, NULL, NULL );
    set_callbacks( Open, Close );

    add_shortcut( "subtitle" );
vlc_module_end();

/*****************************************************************************
 * Prototypes:
 *****************************************************************************/
enum
{
    SUB_TYPE_UNKNOWN = -1,
    SUB_TYPE_MICRODVD,
    SUB_TYPE_SUBRIP,
    SUB_TYPE_SSA1,
    SUB_TYPE_SSA2_4,
    SUB_TYPE_ASS,
    SUB_TYPE_VPLAYER,
    SUB_TYPE_SAMI,
    SUB_TYPE_SUBVIEWER, //SUBVIEWER 2!
    SUB_TYPE_DVDSUBTITLE,
    SUB_TYPE_MPL2,
    SUB_TYPE_AQT,
    SUB_TYPE_PJS,
    SUB_TYPE_MPSUB,
    SUB_TYPE_JACOSUB
};

typedef struct
{
    int     i_line_count;
    int     i_line;
    char    **line;
} text_t;

static int  TextLoad( text_t *, stream_t *s );
static void TextUnload( text_t * );

typedef struct
{
    int64_t i_start;
    int64_t i_stop;

    char    *psz_text;
} subtitle_t;


struct demux_sys_t
{
    int         i_type;
    text_t      txt;
    es_out_id_t *es;

    int64_t     i_next_demux_date;
    int64_t     i_microsecperframe;

    char        *psz_header;
    int         i_subtitle;
    int         i_subtitles;
    subtitle_t  *subtitle;

    int64_t     i_length;
};

static int  ParseMicroDvd   ( demux_t *, subtitle_t *, int );
static int  ParseSubRip     ( demux_t *, subtitle_t *, int );
static int  ParseSubViewer  ( demux_t *, subtitle_t *, int );
static int  ParseSSA        ( demux_t *, subtitle_t *, int );
static int  ParseVplayer    ( demux_t *, subtitle_t *, int );
static int  ParseSami       ( demux_t *, subtitle_t *, int );
static int  ParseDVDSubtitle( demux_t *, subtitle_t *, int );
static int  ParseMPL2       ( demux_t *, subtitle_t *, int );
static int  ParseAQT        ( demux_t *, subtitle_t *, int );
static int  ParsePJS        ( demux_t *, subtitle_t *, int );
static int  ParseMPSub      ( demux_t *, subtitle_t *, int );
static int  ParseJSS        ( demux_t *, subtitle_t *, int );

static struct
{
    const char *psz_type_name;
    int  i_type;
    const char *psz_name;
    int  (*pf_read)( demux_t *, subtitle_t*, int );
} sub_read_subtitle_function [] =
{
    { "microdvd",   SUB_TYPE_MICRODVD,    "MicroDVD",    ParseMicroDvd },
    { "subrip",     SUB_TYPE_SUBRIP,      "SubRIP",      ParseSubRip },
    { "subviewer",  SUB_TYPE_SUBVIEWER,   "SubViewer",   ParseSubViewer },
    { "ssa1",       SUB_TYPE_SSA1,        "SSA-1",       ParseSSA },
    { "ssa2-4",     SUB_TYPE_SSA2_4,      "SSA-2/3/4",   ParseSSA },
    { "ass",        SUB_TYPE_ASS,         "SSA/ASS",     ParseSSA },
    { "vplayer",    SUB_TYPE_VPLAYER,     "VPlayer",     ParseVplayer },
    { "sami",       SUB_TYPE_SAMI,        "SAMI",        ParseSami },
    { "dvdsubtitle",SUB_TYPE_DVDSUBTITLE, "DVDSubtitle", ParseDVDSubtitle },
    { "mpl2",       SUB_TYPE_MPL2,        "MPL2",        ParseMPL2 },
    { "aqt",        SUB_TYPE_AQT,         "AQTitle",     ParseAQT },
    { "pjs",        SUB_TYPE_PJS,         "PhoenixSub",  ParsePJS },
    { "mpsub",      SUB_TYPE_MPSUB,       "MPSub",       ParseMPSub },
    { "jacosub",    SUB_TYPE_JACOSUB,     "JacoSub",     ParseJSS },
    { NULL,         SUB_TYPE_UNKNOWN,     "Unknown",     NULL }
};

/* Missing Detect
    SubViewer 1
    JSS
    RealText
    Subrip09
   */


static int Demux( demux_t * );
static int Control( demux_t *, int, va_list );

/*static void Fix( demux_t * );*/

/*****************************************************************************
 * Module initializer
 *****************************************************************************/
static int Open ( vlc_object_t *p_this )
{
    demux_t        *p_demux = (demux_t*)p_this;
    demux_sys_t    *p_sys;
    es_format_t    fmt;
    float          f_fps;
    char           *psz_type;
    int  (*pf_read)( demux_t *, subtitle_t*, int );
    int            i, i_max;

    if( !p_demux->b_force )
    {
        msg_Dbg( p_demux, "subtitle demux discarded" );
        return VLC_EGENERIC;
    }

    p_demux->pf_demux = Demux;
    p_demux->pf_control = Control;
    p_demux->p_sys = p_sys = malloc( sizeof( demux_sys_t ) );
    p_sys->psz_header         = NULL;
    p_sys->i_subtitle         = 0;
    p_sys->i_subtitles        = 0;
    p_sys->subtitle           = NULL;
    p_sys->i_microsecperframe = 40000;

    /* Get the FPS */
    f_fps = var_CreateGetFloat( p_demux, "sub-original-fps" );
    if( f_fps >= 1.0 )
        p_sys->i_microsecperframe = (int64_t)( (float)1000000 / f_fps );

    msg_Dbg( p_demux, "Movie fps: %f", f_fps );

    /* Check for override of the fps */
    f_fps = var_CreateGetFloat( p_demux, "sub-fps" );
    if( f_fps >= 1.0 )
    {
        p_sys->i_microsecperframe = (int64_t)( (float)1000000 / f_fps );
        msg_Dbg( p_demux, "Override subtitle fps %f", f_fps );
    }

    /* Get or probe the type */
    p_sys->i_type = SUB_TYPE_UNKNOWN;
    psz_type = var_CreateGetString( p_demux, "sub-type" );
    if( *psz_type )
    {
        int i;

        for( i = 0; ; i++ )
        {
            if( sub_read_subtitle_function[i].psz_type_name == NULL )
                break;

            if( !strcmp( sub_read_subtitle_function[i].psz_type_name,
                         psz_type ) )
            {
                p_sys->i_type = sub_read_subtitle_function[i].i_type;
                break;
            }
        }
    }
    free( psz_type );

    /* Probe if unknown type */
    if( p_sys->i_type == SUB_TYPE_UNKNOWN )
    {
        int     i_try;
        char    *s = NULL;

        msg_Dbg( p_demux, "autodetecting subtitle format" );
        for( i_try = 0; i_try < 256; i_try++ )
        {
            int i_dummy;
            char p_dummy;

            if( ( s = stream_ReadLine( p_demux->s ) ) == NULL )
                break;

            if( strcasestr( s, "<SAMI>" ) )
            {
                p_sys->i_type = SUB_TYPE_SAMI;
                break;
            }
            else if( sscanf( s, "{%d}{%d}", &i_dummy, &i_dummy ) == 2 ||
                     sscanf( s, "{%d}{}", &i_dummy ) == 1)
            {
                p_sys->i_type = SUB_TYPE_MICRODVD;
                break;
            }
            else if( sscanf( s,
                             "%d:%d:%d,%d --> %d:%d:%d,%d",
                             &i_dummy,&i_dummy,&i_dummy,&i_dummy,
                             &i_dummy,&i_dummy,&i_dummy,&i_dummy ) == 8 )
            {
                p_sys->i_type = SUB_TYPE_SUBRIP;
                break;
            }
            else if( !strncasecmp( s, "!: This is a Sub Station Alpha v1", 33 ) )
            {
                p_sys->i_type = SUB_TYPE_SSA1;
                break;
            }
            else if( !strncasecmp( s, "ScriptType: v4.00+", 18 ) )
            {
                p_sys->i_type = SUB_TYPE_ASS;
                break;
            }
            else if( !strncasecmp( s, "ScriptType: v4.00", 17 ) )
            {
                p_sys->i_type = SUB_TYPE_SSA2_4;
                break;
            }
            else if( !strncasecmp( s, "Dialogue: Marked", 16  ) )
            {
                p_sys->i_type = SUB_TYPE_SSA2_4;
                break;
            }
            else if( !strncasecmp( s, "Dialogue:", 9  ) )
            {
                p_sys->i_type = SUB_TYPE_ASS;
                break;
            }
            else if( strcasestr( s, "[INFORMATION]" ) )
            {
                p_sys->i_type = SUB_TYPE_SUBVIEWER; /* I hope this will work */
                break;
            }
            else if( sscanf( s, "%d:%d:%d.%d %d:%d:%d", &i_dummy, &i_dummy, &i_dummy, &i_dummy, &i_dummy, &i_dummy, &i_dummy ) == 7 ||
                    sscanf( s, "@%d @%d", &i_dummy, &i_dummy) == 2)
            {
                p_sys->i_type = SUB_TYPE_JACOSUB;
            }
            else if( sscanf( s, "%d:%d:%d:", &i_dummy, &i_dummy, &i_dummy ) == 3 ||
                     sscanf( s, "%d:%d:%d ", &i_dummy, &i_dummy, &i_dummy ) == 3 )
            {
                p_sys->i_type = SUB_TYPE_VPLAYER;
                break;
            }
            else if( sscanf( s, "{T %d:%d:%d:%d", &i_dummy, &i_dummy,
                             &i_dummy, &i_dummy ) == 4 )
            {
                p_sys->i_type = SUB_TYPE_DVDSUBTITLE;
                break;
            }
            else if( sscanf( s, "[%d][%d]", &i_dummy, &i_dummy ) == 2 ||
                     sscanf( s, "[%d][]", &i_dummy ) == 1)
            {
                p_sys->i_type = SUB_TYPE_MPL2;
                break;
            }
            else if( sscanf (s, "FORMAT=%d", &i_dummy) == 1 ||
                     ( sscanf (s, "FORMAT=TIM%c", &p_dummy) == 1
                       && p_dummy =='E' ) )
            {
                p_sys->i_type = SUB_TYPE_MPSUB;
            }
            else if( sscanf( s, "-->> %d", &i_dummy) == 1 )
            {
                p_sys->i_type = SUB_TYPE_AQT;
            }
            else if( sscanf( s, "%d,%d,", &i_dummy, &i_dummy ) == 2 )
            {
                p_sys->i_type = SUB_TYPE_PJS;
            }

            free( s );
            s = NULL;
        }

        free( s );

        /* It will nearly always work even for non seekable stream thanks the
         * caching system, and if it fails we lose just a few sub */
        if( stream_Seek( p_demux->s, 0 ) )
        {
            msg_Warn( p_demux, "failed to rewind" );
        }
    }
    if( p_sys->i_type == SUB_TYPE_UNKNOWN )
    {
        msg_Err( p_demux, "failed to recognize subtitle type" );
        free( p_sys );
        return VLC_EGENERIC;
    }

    for( i = 0; ; i++ )
    {
        if( sub_read_subtitle_function[i].i_type == p_sys->i_type )
        {
            msg_Dbg( p_demux, "detected %s format",
                     sub_read_subtitle_function[i].psz_name );
            pf_read = sub_read_subtitle_function[i].pf_read;
            break;
        }
    }

    msg_Dbg( p_demux, "loading all subtitles..." );

    /* Load the whole file */
    TextLoad( &p_sys->txt, p_demux->s );

    /* Parse it */
    for( i_max = 0;; )
    {
        if( p_sys->i_subtitles >= i_max )
        {
            i_max += 500;
            if( !( p_sys->subtitle = realloc( p_sys->subtitle,
                                              sizeof(subtitle_t) * i_max ) ) )
            {
                msg_Err( p_demux, "out of memory");
                free( p_sys->subtitle );
                TextUnload( &p_sys->txt );
                free( p_sys );
                return VLC_ENOMEM;
            }
        }

        if( pf_read( p_demux, &p_sys->subtitle[p_sys->i_subtitles],
                     p_sys->i_subtitles ) )
            break;

        p_sys->i_subtitles++;
    }
    /* Unload */
    TextUnload( &p_sys->txt );

    msg_Dbg(p_demux, "loaded %d subtitles", p_sys->i_subtitles );

    /* Fix subtitle (order and time) *** */
    p_sys->i_subtitle = 0;
    p_sys->i_length = 0;
    if( p_sys->i_subtitles > 0 )
    {
        p_sys->i_length = p_sys->subtitle[p_sys->i_subtitles-1].i_stop;
        /* +1 to avoid 0 */
        if( p_sys->i_length <= 0 )
            p_sys->i_length = p_sys->subtitle[p_sys->i_subtitles-1].i_start+1;
    }

    /* *** add subtitle ES *** */
    if( p_sys->i_type == SUB_TYPE_SSA1 ||
             p_sys->i_type == SUB_TYPE_SSA2_4 ||
             p_sys->i_type == SUB_TYPE_ASS )
    {
        es_format_Init( &fmt, SPU_ES, VLC_FOURCC( 's','s','a',' ' ) );
    }
    else
    {
        es_format_Init( &fmt, SPU_ES, VLC_FOURCC( 's','u','b','t' ) );
    }
    if( p_sys->psz_header != NULL )
    {
        fmt.i_extra = strlen( p_sys->psz_header ) + 1;
        fmt.p_extra = strdup( p_sys->psz_header );
    }
    p_sys->es = es_out_Add( p_demux->out, &fmt );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close: Close subtitle demux
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    demux_t *p_demux = (demux_t*)p_this;
    demux_sys_t *p_sys = p_demux->p_sys;
    int i;

    for( i = 0; i < p_sys->i_subtitles; i++ )
        free( p_sys->subtitle[i].psz_text );
    free( p_sys->subtitle );

    free( p_sys );
}

/*****************************************************************************
 * Control:
 *****************************************************************************/
static int Control( demux_t *p_demux, int i_query, va_list args )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    int64_t *pi64, i64;
    double *pf, f;

    switch( i_query )
    {
        case DEMUX_GET_LENGTH:
            pi64 = (int64_t*)va_arg( args, int64_t * );
            *pi64 = p_sys->i_length;
            return VLC_SUCCESS;

        case DEMUX_GET_TIME:
            pi64 = (int64_t*)va_arg( args, int64_t * );
            if( p_sys->i_subtitle < p_sys->i_subtitles )
            {
                *pi64 = p_sys->subtitle[p_sys->i_subtitle].i_start;
                return VLC_SUCCESS;
            }
            return VLC_EGENERIC;

        case DEMUX_SET_TIME:
            i64 = (int64_t)va_arg( args, int64_t );
            p_sys->i_subtitle = 0;
            while( p_sys->i_subtitle < p_sys->i_subtitles &&
                   p_sys->subtitle[p_sys->i_subtitle].i_start < i64 )
            {
                p_sys->i_subtitle++;
            }

            if( p_sys->i_subtitle >= p_sys->i_subtitles )
                return VLC_EGENERIC;
            return VLC_SUCCESS;

        case DEMUX_GET_POSITION:
            pf = (double*)va_arg( args, double * );
            if( p_sys->i_subtitle >= p_sys->i_subtitles )
            {
                *pf = 1.0;
            }
            else if( p_sys->i_subtitles > 0 )
            {
                *pf = (double)p_sys->subtitle[p_sys->i_subtitle].i_start /
                      (double)p_sys->i_length;
            }
            else
            {
                *pf = 0.0;
            }
            return VLC_SUCCESS;

        case DEMUX_SET_POSITION:
            f = (double)va_arg( args, double );
            i64 = f * p_sys->i_length;

            p_sys->i_subtitle = 0;
            while( p_sys->i_subtitle < p_sys->i_subtitles &&
                   p_sys->subtitle[p_sys->i_subtitle].i_start < i64 )
            {
                p_sys->i_subtitle++;
            }
            if( p_sys->i_subtitle >= p_sys->i_subtitles )
                return VLC_EGENERIC;
            return VLC_SUCCESS;

        case DEMUX_SET_NEXT_DEMUX_TIME:
            p_sys->i_next_demux_date = (int64_t)va_arg( args, int64_t );
            return VLC_SUCCESS;

        case DEMUX_GET_FPS:
        case DEMUX_GET_META:
        case DEMUX_GET_ATTACHMENTS:
        case DEMUX_GET_TITLE_INFO:
        case DEMUX_HAS_UNSUPPORTED_META:
            return VLC_EGENERIC;

        default:
            msg_Err( p_demux, "unknown query %d in subtitle control", i_query );
            return VLC_EGENERIC;
    }
}

/*****************************************************************************
 * Demux: Send subtitle to decoder
 *****************************************************************************/
static int Demux( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    int64_t i_maxdate;

    if( p_sys->i_subtitle >= p_sys->i_subtitles )
        return 0;

    i_maxdate = p_sys->i_next_demux_date - var_GetTime( p_demux->p_parent, "spu-delay" );;
    if( i_maxdate <= 0 && p_sys->i_subtitle < p_sys->i_subtitles )
    {
        /* Should not happen */
        i_maxdate = p_sys->subtitle[p_sys->i_subtitle].i_start + 1;
    }

    while( p_sys->i_subtitle < p_sys->i_subtitles &&
           p_sys->subtitle[p_sys->i_subtitle].i_start < i_maxdate )
    {
        block_t *p_block;
        int i_len = strlen( p_sys->subtitle[p_sys->i_subtitle].psz_text ) + 1;

        if( i_len <= 1 )
        {
            /* empty subtitle */
            p_sys->i_subtitle++;
            continue;
        }

        if( ( p_block = block_New( p_demux, i_len ) ) == NULL )
        {
            p_sys->i_subtitle++;
            continue;
        }

        if( p_sys->subtitle[p_sys->i_subtitle].i_start < 0 )
        {
            p_sys->i_subtitle++;
            continue;
        }

        p_block->i_pts = p_sys->subtitle[p_sys->i_subtitle].i_start;
        p_block->i_dts = p_block->i_pts;
        if( p_sys->subtitle[p_sys->i_subtitle].i_stop > 0 )
        {
            p_block->i_length =
                p_sys->subtitle[p_sys->i_subtitle].i_stop - p_block->i_pts;
        }

        memcpy( p_block->p_buffer,
                p_sys->subtitle[p_sys->i_subtitle].psz_text, i_len );
        if( p_block->i_pts > 0 )
        {
            es_out_Send( p_demux->out, p_sys->es, p_block );
        }
        else
        {
            block_Release( p_block );
        }
        p_sys->i_subtitle++;
    }

    /* */
    p_sys->i_next_demux_date = 0;

    return 1;
}

/*****************************************************************************
 * Fix: fix time stamp and order of subtitle
 *****************************************************************************/
#ifdef USE_THIS_UNUSED_PIECE_OF_CODE
static void Fix( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    bool b_done;
    int     i_index;

    /* *** fix order (to be sure...) *** */
    /* We suppose that there are near in order and this durty bubble sort
     * wont take too much time
     */
    do
    {
        b_done = true;
        for( i_index = 1; i_index < p_sys->i_subtitles; i_index++ )
        {
            if( p_sys->subtitle[i_index].i_start <
                    p_sys->subtitle[i_index - 1].i_start )
            {
                subtitle_t sub_xch;
                memcpy( &sub_xch,
                        p_sys->subtitle + i_index - 1,
                        sizeof( subtitle_t ) );
                memcpy( p_sys->subtitle + i_index - 1,
                        p_sys->subtitle + i_index,
                        sizeof( subtitle_t ) );
                memcpy( p_sys->subtitle + i_index,
                        &sub_xch,
                        sizeof( subtitle_t ) );
                b_done = false;
            }
        }
    } while( !b_done );
}
#endif

static int TextLoad( text_t *txt, stream_t *s )
{
    int   i_line_max;

    /* init txt */
    i_line_max          = 500;
    txt->i_line_count   = 0;
    txt->i_line         = 0;
    txt->line           = calloc( i_line_max, sizeof( char * ) );

    /* load the complete file */
    for( ;; )
    {
        char *psz = stream_ReadLine( s );

        if( psz == NULL )
            break;

        txt->line[txt->i_line_count++] = psz;
        if( txt->i_line_count >= i_line_max )
        {
            i_line_max += 100;
            txt->line = realloc( txt->line, i_line_max * sizeof( char * ) );
        }
    }

    if( txt->i_line_count <= 0 )
    {
        free( txt->line );
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}
static void TextUnload( text_t *txt )
{
    int i;

    for( i = 0; i < txt->i_line_count; i++ )
    {
        free( txt->line[i] );
    }
    free( txt->line );
    txt->i_line       = 0;
    txt->i_line_count = 0;
}

static char *TextGetLine( text_t *txt )
{
    if( txt->i_line >= txt->i_line_count )
        return( NULL );

    return txt->line[txt->i_line++];
}
static void TextPreviousLine( text_t *txt )
{
    if( txt->i_line > 0 )
        txt->i_line--;
}

/*****************************************************************************
 * Specific Subtitle function
 *****************************************************************************/
/* ParseMicroDvd:
 *  Format:
 *      {n1}{n2}Line1|Line2|Line3....
 *  where n1 and n2 are the video frame number (n2 can be empty)
 */
static int ParseMicroDvd( demux_t *p_demux, subtitle_t *p_subtitle,
                          int i_idx )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    char *psz_text;
    int  i_start;
    int  i_stop;
    int  i;

    for( ;; )
    {
        const char *s = TextGetLine( txt );
        if( !s )
            return VLC_EGENERIC;

        psz_text = malloc( strlen(s) + 1 );
        if( !psz_text )
            return VLC_ENOMEM;

        i_start = 0;
        i_stop  = 0;
        if( sscanf( s, "{%d}{}%[^\r\n]", &i_start, psz_text ) == 2 ||
            sscanf( s, "{%d}{%d}%[^\r\n]", &i_start, &i_stop, psz_text ) == 3)
        {
            float f_fps;
            if( i_start != 1 || i_stop != 1 )
                break;

            /* We found a possible setting of the framerate "{1}{1}23.976" */
            /* Check if it's usable, and if the sub-fps is not set */
            f_fps = us_strtod( psz_text, NULL );
            if( f_fps > 0.0 && var_GetFloat( p_demux, "sub-fps" ) <= 0.0 )
                p_sys->i_microsecperframe = (int64_t)((float)1000000 / f_fps);
        }
        free( psz_text );
    }

    /* replace | by \n */
    for( i = 0; psz_text[i] != '\0'; i++ )
    {
        if( psz_text[i] == '|' )
            psz_text[i] = '\n';
    }

    /* */
    p_subtitle->i_start  = i_start * p_sys->i_microsecperframe;
    p_subtitle->i_stop   = i_stop  * p_sys->i_microsecperframe;
    p_subtitle->psz_text = psz_text;
    return VLC_SUCCESS;
}

/* ParseSubRipSubViewer
 *  Format SubRip
 *      n
 *      h1:m1:s1,d1 --> h2:m2:s2,d2
 *      Line1
 *      Line2
 *      ....
 *      [Empty line]
 *  Format SubViewer v1/v2
 *      h1:m1:s1.d1,h2:m2:s2.d2
 *      Line1[br]Line2
 *      Line3
 *      ...
 *      [empty line]
 *  We ignore line number for SubRip
 */
static int ParseSubRipSubViewer( demux_t *p_demux, subtitle_t *p_subtitle,
                                 const char *psz_fmt,
                                 bool b_replace_br )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    char    *psz_text;

    for( ;; )
    {
        const char *s = TextGetLine( txt );
        int h1, m1, s1, d1, h2, m2, s2, d2;

        if( !s )
            return VLC_EGENERIC;

        if( sscanf( s, psz_fmt,
                    &h1, &m1, &s1, &d1,
                    &h2, &m2, &s2, &d2 ) == 8 )
        {
            p_subtitle->i_start = ( (int64_t)h1 * 3600*1000 +
                                    (int64_t)m1 * 60*1000 +
                                    (int64_t)s1 * 1000 +
                                    (int64_t)d1 ) * 1000;

            p_subtitle->i_stop  = ( (int64_t)h2 * 3600*1000 +
                                    (int64_t)m2 * 60*1000 +
                                    (int64_t)s2 * 1000 +
                                    (int64_t)d2 ) * 1000;
            break;
        }
    }

    /* Now read text until an empty line */
    psz_text = strdup("");
    if( !psz_text )
        return VLC_ENOMEM;
    for( ;; )
    {
        const char *s = TextGetLine( txt );
        int i_len;
        int i_old;

        if( !s )
        {
            free( psz_text );
            return VLC_EGENERIC;
        }

        i_len = strlen( s );
        if( i_len <= 0 )
        {
            p_subtitle->psz_text = psz_text;
            return VLC_SUCCESS;
        }

        i_old = strlen( psz_text );
        psz_text = realloc( psz_text, i_old + i_len + 1 + 1 );
        if( !psz_text )
            return VLC_ENOMEM;
        strcat( psz_text, s );
        strcat( psz_text, "\n" );

        /* replace [br] by \n */
        if( b_replace_br )
        {
            char *p;
 
            while( ( p = strstr( psz_text, "[br]" ) ) )
            {
                *p++ = '\n';
                memmove( p, &p[3], strlen(&p[3])+1 );
            }
        }
    }
}
/* ParseSubRip
 */
static int  ParseSubRip( demux_t *p_demux, subtitle_t *p_subtitle,
                         int i_idx )
{
    return ParseSubRipSubViewer( p_demux, p_subtitle,
                                 "%d:%d:%d,%d --> %d:%d:%d,%d",
                                 false );
}
/* ParseSubViewer
 */
static int  ParseSubViewer( demux_t *p_demux, subtitle_t *p_subtitle,
                            int i_idx )
{
    return ParseSubRipSubViewer( p_demux, p_subtitle,
                                 "%d:%d:%d.%d,%d:%d:%d.%d",
                                 true );
}

/* ParseSSA
 */
static int  ParseSSA( demux_t *p_demux, subtitle_t *p_subtitle,
                      int i_idx )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;

    for( ;; )
    {
        const char *s = TextGetLine( txt );
        int h1, m1, s1, c1, h2, m2, s2, c2;
        char *psz_text;
        char temp[16];

        if( !s )
            return VLC_EGENERIC;

        /* We expect (SSA2-4):
         * Format: Marked, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
         * Dialogue: Marked=0,0:02:40.65,0:02:41.79,Wolf main,Cher,0000,0000,0000,,Et les enregistrements de ses ondes delta ?
         *
         * SSA-1 is similar but only has 8 commas up untill the subtitle text. Probably the Effect field is no present, but not 100 % sure.
         */

        /* For ASS:
         * Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
         * Dialogue: Layer#,0:02:40.65,0:02:41.79,Wolf main,Cher,0000,0000,0000,,Et les enregistrements de ses ondes delta ?
         */

        /* The output text is - at least, not removing numbers - 18 chars shorter than the input text. */
        psz_text = malloc( strlen(s) );
        if( !psz_text )
            return VLC_ENOMEM;

        if( sscanf( s,
                    "Dialogue: %15[^,],%d:%d:%d.%d,%d:%d:%d.%d,%[^\r\n]",
                    temp,
                    &h1, &m1, &s1, &c1,
                    &h2, &m2, &s2, &c2,
                    psz_text ) == 10 )
        {
            /* The dec expects: ReadOrder, Layer, Style, Name, MarginL, MarginR, MarginV, Effect, Text */
            /* (Layer comes from ASS specs ... it's empty for SSA.) */
            if( p_sys->i_type == SUB_TYPE_SSA1 )
            {
                /* SSA1 has only 8 commas before the text starts, not 9 */
                memmove( &psz_text[1], psz_text, strlen(psz_text)+1 );
                psz_text[0] = ',';
            }
            else
            {
                int i_layer = ( p_sys->i_type == SUB_TYPE_ASS ) ? atoi( temp ) : 0;

                /* ReadOrder, Layer, %s(rest of fields) */
                snprintf( temp, sizeof(temp), "%d,%d,", i_idx, i_layer );
                memmove( psz_text + strlen(temp), psz_text, strlen(psz_text)+1 );
                memcpy( psz_text, temp, strlen(temp) );
            }

            p_subtitle->i_start = ( (int64_t)h1 * 3600*1000 +
                                    (int64_t)m1 * 60*1000 +
                                    (int64_t)s1 * 1000 +
                                    (int64_t)c1 * 10 ) * 1000;
            p_subtitle->i_stop  = ( (int64_t)h2 * 3600*1000 +
                                    (int64_t)m2 * 60*1000 +
                                    (int64_t)s2 * 1000 +
                                    (int64_t)c2 * 10 ) * 1000;
            p_subtitle->psz_text = psz_text;
            return VLC_SUCCESS;
        }
        free( psz_text );

        /* All the other stuff we add to the header field */
        if( !p_sys->psz_header )
            p_sys->psz_header = strdup( "" );
        if( !p_sys->psz_header )
            return VLC_ENOMEM;

        p_sys->psz_header =
            realloc( p_sys->psz_header,
                     strlen( p_sys->psz_header ) + strlen( s ) + 2 );
        strcat( p_sys->psz_header,  s );
        strcat( p_sys->psz_header, "\n" );
    }
}

/* ParseVplayer
 *  Format
 *      h:m:s:Line1|Line2|Line3....
 *  or
 *      h:m:s Line1|Line2|Line3....
 */
static int  ParseVplayer( demux_t *p_demux, subtitle_t *p_subtitle,
                          int i_idx )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    char *psz_text;
    int i;

    for( ;; )
    {
        const char *s = TextGetLine( txt );
        int h1, m1, s1;

        if( !s )
            return VLC_EGENERIC;

        psz_text = malloc( strlen( s ) + 1 );
        if( !psz_text )
            return VLC_ENOMEM;

        if( sscanf( s, "%d:%d:%d%*c%[^\r\n]",
                    &h1, &m1, &s1, psz_text ) == 4 )
        {
            p_subtitle->i_start = ( (int64_t)h1 * 3600*1000 +
                                    (int64_t)m1 * 60*1000 +
                                    (int64_t)s1 * 1000 ) * 1000;
            p_subtitle->i_stop  = 0;
            break;
        }
        free( psz_text );
    }

    /* replace | by \n */
    for( i = 0; psz_text[i] != '\0'; i++ )
    {
        if( psz_text[i] == '|' )
            psz_text[i] = '\n';
    }
    p_subtitle->psz_text = psz_text;
    return VLC_SUCCESS;
}

/* ParseSami
 */
static char *ParseSamiSearch( text_t *txt,
                              char *psz_start, const char *psz_str )
{
    if( psz_start && strcasestr( psz_start, psz_str ) )
    {
        char *s = strcasestr( psz_start, psz_str );
        return &s[strlen( psz_str )];
    }

    for( ;; )
    {
        char *p = TextGetLine( txt );
        if( !p )
            return NULL;

        if( strcasestr( p, psz_str ) )
        {
            char *s = strcasestr( p, psz_str );
            return &s[strlen( psz_str )];
        }
    }
}
static int  ParseSami( demux_t *p_demux, subtitle_t *p_subtitle, int i_idx )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;

    char *s;
    int64_t i_start;

    unsigned int i_text;
    char text[8192]; /* Arbitrary but should be long enough */

    /* search "Start=" */
    if( !( s = ParseSamiSearch( txt, NULL, "Start=" ) ) )
        return VLC_EGENERIC;

    /* get start value */
    i_start = strtol( s, &s, 0 );

    /* search <P */
    if( !( s = ParseSamiSearch( txt, s, "<P" ) ) )
        return VLC_EGENERIC;

    /* search > */
    if( !( s = ParseSamiSearch( txt, s, ">" ) ) )
        return VLC_EGENERIC;

    i_text = 0;
    text[0] = '\0';
    /* now get all txt until  a "Start=" line */
    for( ;; )
    {
        char c = '\0';
        /* Search non empty line */
        while( s && *s == '\0' )
            s = TextGetLine( txt );
        if( !s )
            break;

        if( *s == '<' )
        {
            if( !strncasecmp( s, "<br", 3 ) )
            {
                c = '\n';
            }
            else if( strcasestr( s, "Start=" ) )
            {
                TextPreviousLine( txt );
                break;
            }
            s = ParseSamiSearch( txt, s, ">" );
        }
        else if( !strncmp( s, "&nbsp;", 6 ) )
        {
            c = ' ';
            s += 6;
        }
        else if( *s == '\t' )
        {
            c = ' ';
            s++;
        }
        else
        {
            c = *s;
            s++;
        }
        if( c != '\0' && i_text+1 < sizeof(text) )
        {
            text[i_text++] = c;
            text[i_text] = '\0';
        }
    }

    p_subtitle->i_start = i_start * 1000;
    p_subtitle->i_stop  = 0;
    p_subtitle->psz_text = strdup( text );

    return VLC_SUCCESS;
}

/* ParseDVDSubtitle
 *  Format
 *      {T h1:m1:s1:c1
 *      Line1
 *      Line2
 *      ...
 *      }
 * TODO it can have a header
 *      { HEAD
 *          ...
 *          CODEPAGE=...
 *          FORMAT=...
 *          LANG=English
 *      }
 *      LANG support would be cool
 *      CODEPAGE is probably mandatory FIXME
 */
static int ParseDVDSubtitle( demux_t *p_demux, subtitle_t *p_subtitle,
                             int i_idx )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    char *psz_text;

    for( ;; )
    {
        const char *s = TextGetLine( txt );
        int h1, m1, s1, c1;

        if( !s )
            return VLC_EGENERIC;

        if( sscanf( s,
                    "{T %d:%d:%d:%d",
                    &h1, &m1, &s1, &c1 ) == 4 )
        {
            p_subtitle->i_start = ( (int64_t)h1 * 3600*1000 +
                                    (int64_t)m1 * 60*1000 +
                                    (int64_t)s1 * 1000 +
                                    (int64_t)c1 * 10) * 1000;
            p_subtitle->i_stop = 0;
            break;
        }
    }

    /* Now read text until a line containing "}" */
    psz_text = strdup("");
    if( !psz_text )
        return VLC_ENOMEM;
    for( ;; )
    {
        const char *s = TextGetLine( txt );
        int i_len;
        int i_old;

        if( !s )
        {
            free( psz_text );
            return VLC_EGENERIC;
        }

        i_len = strlen( s );
        if( i_len == 1 && s[0] == '}')
        {
            p_subtitle->psz_text = psz_text;
            return VLC_SUCCESS;
        }

        i_old = strlen( psz_text );
        psz_text = realloc( psz_text, i_old + i_len + 1 + 1 );
        if( !psz_text )
            return VLC_ENOMEM;
        strcat( psz_text, s );
        strcat( psz_text, "\n" );
    }
}

/* ParseMPL2
 *  Format
 *     [n1][n2]Line1|Line2|Line3...
 *  where n1 and n2 are the video frame number (n2 can be empty)
 */
static int ParseMPL2( demux_t *p_demux, subtitle_t *p_subtitle, int i_idx )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    char *psz_text;
    int i;

    for( ;; )
    {
        const char *s = TextGetLine( txt );
        int i_start;
        int i_stop;

        if( !s )
            return VLC_EGENERIC;

        psz_text = malloc( strlen(s) + 1 );
        if( !psz_text )
            return VLC_ENOMEM;

        i_start = 0;
        i_stop  = 0;
        if( sscanf( s, "[%d][] %[^\r\n]", &i_start, psz_text ) == 2 ||
            sscanf( s, "[%d][%d] %[^\r\n]", &i_start, &i_stop, psz_text ) == 3)
        {
            p_subtitle->i_start = (int64_t)i_start * 100000;
            p_subtitle->i_stop  = (int64_t)i_stop  * 100000;
            break;
        }
        free( psz_text );
    }

    for( i = 0; psz_text[i] != '\0'; )
    {
        /* replace | by \n */
        if( psz_text[i] == '|' )
            psz_text[i] = '\n';

        /* Remove italic */
        if( psz_text[i] == '/' && ( i == 0 || psz_text[i-1] == '\n' ) )
            memmove( &psz_text[i], &psz_text[i+1], strlen(&psz_text[i+1])+1 );
        else
            i++;
    }
    p_subtitle->psz_text = psz_text;
    return VLC_SUCCESS;
}

static int ParseAQT( demux_t *p_demux, subtitle_t *p_subtitle, int i_idx )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    char *psz_text = strdup( "" );
    int i_old = 0;
    int i_firstline = 1;

    for( ;; )
    {
        int t; /* Time */

        const char *s = TextGetLine( txt );

        if( !s )
            return VLC_EGENERIC;

        /* Data Lines */
        if( sscanf (s, "-->> %d", &t) == 1)
        {
            p_subtitle->i_start = (int64_t)t; /* * FPS*/
            p_subtitle->i_stop  = 0;

            /* Starting of a subtitle */
            if( i_firstline )
            {
                i_firstline = 0;
            }
            /* We have been too far: end of the subtitle, begin of next */
            else
            {
                txt->i_line--;
                break;
            }
        }
        /* Text Lines */
        else
        {
            i_old = strlen( psz_text ) + 1;
            psz_text = realloc( psz_text, i_old + strlen( s ) + 1 );
            if( !psz_text )
                 return VLC_ENOMEM;
            strcat( psz_text, s );
            strcat( psz_text, "\n" );
            if( txt->i_line == txt->i_line_count )
                break;
        }
    }
    p_subtitle->psz_text = psz_text;
    return VLC_SUCCESS;
}

static int ParsePJS( demux_t *p_demux, subtitle_t *p_subtitle, int i_idx )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    char *psz_text;

    for( ;; )
    {
        const char *s = TextGetLine( txt );
        int t1, t2;

        if( !s )
            return VLC_EGENERIC;

        psz_text = malloc( strlen(s) + 1 );

        /* Data Lines */
        if( sscanf (s, "%d,%d,\"%[^\n\r]", &t1, &t2, psz_text ) == 3 )
        {
            /* 1/10th of second ? Frame based ? FIXME */
            p_subtitle->i_start = 10 * t1;
            p_subtitle->i_stop = 10 * t2;
            /* Remove latest " */
            psz_text[ strlen(psz_text) - 1 ] = '\0';

            break;
        }
        free( psz_text );
    }
    p_subtitle->psz_text = psz_text;
    msg_Dbg( p_demux, "%s", psz_text );
    return VLC_SUCCESS;
}

static float mpsub_total = 0.0;
static float mpsub_factor = 0.0;

static int ParseMPSub( demux_t *p_demux, subtitle_t *p_subtitle, int i_idx )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    char *psz_text = strdup( "" );

    for( ;; )
    {
        const char *s = TextGetLine( txt );
        float f1, f2;
        char p_dummy;
        char *psz_temp;

        if( !s )
            return VLC_EGENERIC;

        if( sscanf (s, "FORMAT=TIM%c", &p_dummy ) == 1 && p_dummy == 'E')
        {
            mpsub_factor = 100.0;
            break;
        }

        psz_temp = malloc( strlen(s) - 6 );
        if( sscanf( s, "FORMAT=%[^\r\n]", psz_temp ) )
        {
            float f_fps;
            f_fps = us_strtod( psz_temp, NULL );
            if( f_fps > 0.0 && var_GetFloat( p_demux, "sub-fps" ) <= 0.0 )
                var_SetFloat( p_demux, "sub-fps", f_fps );

            mpsub_factor = 1.0;
            free( psz_temp );
            break;
        }
        free( psz_temp );

        /* Data Lines */
        if( sscanf (s, "%f %f", &f1, &f2 ) == 2 )
        {
            mpsub_total += f1 * mpsub_factor;
            p_subtitle->i_start = (int64_t)(10000.0 * mpsub_total);
            mpsub_total += f2 * mpsub_factor;
            p_subtitle->i_stop = (int64_t)(10000.0 * mpsub_total);
            break;
        }
    }

    for( ;; )
    {
        const char *s = TextGetLine( txt );

        if( !s )
            return VLC_EGENERIC;

        int i_len = strlen( s );
        if( i_len == 0 )
            break;

        int i_old = strlen( psz_text );

        psz_text = realloc( psz_text, i_old + i_len + 1 + 1 );
        if( !psz_text )
             return VLC_ENOMEM;

        strcat( psz_text, s );
        strcat( psz_text, "\n" );
    }

    p_subtitle->psz_text = psz_text;
    return VLC_SUCCESS;
}

static int ParseJSS( demux_t *p_demux, subtitle_t *p_subtitle, int i_idx )
{
    demux_sys_t  *p_sys = p_demux->p_sys;
    text_t       *txt = &p_sys->txt;
    char         *psz_text, *psz_orig;
    char         *psz_text2, *psz_orig2;
    int h1, h2, m1, m2, s1, s2, f1, f2;
    static int i_comment = 0;

    static int jss_time_resolution = 30;
    static int jss_time_shift = 0;

    /* Parse the main lines */
    for( ;; )
    {
        const char *s = TextGetLine( txt );
        if( !s )
            return VLC_EGENERIC;

        psz_text = malloc( strlen( s ) + 1 );
        psz_orig = psz_text;

        if( sscanf( s, "%d:%d:%d.%d %d:%d:%d.%d %[^\n\r]",
                    &h1, &m1, &s1, &f1, &h2, &m2, &s2, &f2, psz_text ) == 9 )
        {
            p_subtitle->i_start = ( (int64_t)( h1 *3600 + m1 * 60 + s1 ) +
                (int64_t)( ( f1 +  jss_time_shift ) /  jss_time_resolution ) )
                * 1000000;
            p_subtitle->i_stop = ( (int64_t)( h2 *3600 + m2 * 60 + s2 ) +
                (int64_t)( ( f2 +  jss_time_shift ) /  jss_time_resolution ) )
                * 1000000;
        }
        else if( sscanf( s, "@%d @%d %[^\n\r]", &f1, &f2, psz_text ) == 3 )
        {
            p_subtitle->i_start = (int64_t)(
                    ( f1 + jss_time_shift ) / jss_time_resolution * 1000000.0 );
            p_subtitle->i_stop = (int64_t)(
                    ( f2 + jss_time_shift ) / jss_time_resolution * 1000000.0 );
        }
        else if( s[0] == '#' )
        {
            int h = 0, m =0, sec = 1, f = 1;
            unsigned shift = 1;
            int inv = 1;

            strcpy( psz_text, s );

            switch( toupper( psz_text[1] ) )
            {
                case 'S':
                     shift = isalpha( psz_text[2] ) ? 6 : 2 ;

                     if( sscanf( &psz_text[shift], "%d", &h ) )
                     {
                         /* Negative shifting */
                         if( h < 0 )
                         {
                             h *= -1;
                             inv = -1;
                         }

                         if( sscanf( &psz_text[shift], "%*d:%d", &m ) )
                         {
                             if( sscanf( &psz_text[shift], "%*d:%*d:%d", &sec ) )
                             {
                                 sscanf( &psz_text[shift], "%*d:%*d:%*d.%d", &f );
                             }
                             else
                             {
                                 h = 0;
                                 sscanf( &psz_text[shift], "%d:%d.%d", &m, &sec, &f );
                                 m *= inv;
                             }
                         }
                         else
                         {
                             h = m = 0;
                             sscanf( &psz_text[shift], "%d.%d", &sec, &f);
                             sec *= inv;
                         }
                         jss_time_shift = ( ( h * 3600 + m * 60 + sec )
                             * jss_time_resolution + f ) * inv;
                     }
                     break;

                case 'T':
                    shift = isalpha( psz_text[2] ) ? 8 : 2 ;

                    sscanf( &psz_text[shift], "%d", &jss_time_resolution );
                    break;
            }
            free( psz_text );
            continue;
        }
        else
            /* Unkown line */
        {
            free( psz_text );
            continue;
        }

        /* Skip the blanks */
        while( *psz_text == ' ' || *psz_text == '\t' ) psz_text++;

        /* Parse the directives */
        if( isalpha( *psz_text ) || *psz_text == '[' )
        {
            while( *psz_text != ' ' )
            { psz_text++ ;};

            /* Directives are NOT parsed yet */
            /* directive = malloc( strlen( psz_text ) + 1 );
            if( sscanf( psz_text, "%s %[^\n\r]", directive, psz_text2 ) == 2 )*/
        }

        /* Skip the blanks after directives */
        while( *psz_text == ' ' || *psz_text == '\t' ) psz_text++;

        psz_text2 = calloc( strlen( psz_text) + 1, 1 );
        psz_orig2 = psz_text2;

        for( ; *psz_text != '\0' && *psz_text != '\n' && *psz_text != '\r'; )
        {
            switch( *psz_text )
            {
            case '{':
                i_comment++;
                break;
            case '}':
                if( i_comment )
                {
                    i_comment = 0;
                    if( (*(psz_text + 1 ) ) == ' ' ) psz_text++;
                }
                break;
            case '~':
                if( !i_comment )
                {
                    *psz_text2 = ' ';
                    psz_text2++;
                }
                break;
            case ' ':
            case '\t':
                if( (*(psz_text + 1 ) ) == ' ' || (*(psz_text + 1 ) ) == '\t' )
                    break;
                if( !i_comment )
                {
                    *psz_text2 = ' ';
                    psz_text2++;
                }
                break;
            case '\\':
                if( (*(psz_text + 1 ) ) == 'n' )
                {
                    *psz_text2 = '\n';
                    psz_text++;
                    psz_text2++;
                    break;
                }
                if( ( toupper(*(psz_text + 1 ) ) == 'C' ) ||
                    ( toupper(*(psz_text + 1 ) ) == 'F' ) )
                {
                    psz_text++; psz_text++;
                    break;
                }
                if( (*(psz_text + 1 ) ) == 'B' || (*(psz_text + 1 ) ) == 'b' ||
                    (*(psz_text + 1 ) ) == 'I' || (*(psz_text + 1 ) ) == 'i' ||
                    (*(psz_text + 1 ) ) == 'U' || (*(psz_text + 1 ) ) == 'u' ||
                    (*(psz_text + 1 ) ) == 'D' || (*(psz_text + 1 ) ) == 'N' )
                {
                    psz_text++;
                    break;
                }
                if( (*(psz_text + 1 ) ) == '~' || (*(psz_text + 1 ) ) == '{' ||
                    (*(psz_text + 1 ) ) == '\\' )
                    psz_text++;
                else if( *(psz_text + 1 ) == '\r' ||  *(psz_text + 1 ) == '\n'
                         ||  *(psz_text + 1 ) == '\0' )
                {
                    char *s2 = TextGetLine( txt );
                    if( !s2 )
                        return VLC_EGENERIC;

                    while ( *s2 == ' ' ) s2++;

/*                    int i_len = strlen( psz_orig2 );


                    psz_orig2 = realloc( psz_orig2, strlen( s2 ) +  i_len + 1 );


                    */
                }
            default:
                if( !i_comment )
                {
                    *psz_text2 = *psz_text;
                    psz_text2++;
                }
            }
            psz_text++;
        }

        p_subtitle->psz_text = psz_orig2;
        free( psz_orig );
        return VLC_SUCCESS;
    }
}
