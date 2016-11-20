/*****************************************************************************
 * decklink.cpp: BlackMagic DeckLink SDI output module
 *****************************************************************************
 * Copyright (C) 2012-2013 Rafaël Carré
 * Copyright (C) 2009 Michael Niedermayer <michaelni@gmx.at>
 * Copyright (c) 2009 Baptiste Coudurier <baptiste dot coudurier at gmail dot com>
 *
 * Authors: Rafaël Carré <funman@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*
 * TODO: test non stereo audio
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_fixups.h>
#include <cinttypes>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_threads.h>

#include <vlc_vout_display.h>
#include <vlc_picture_pool.h>

#include <vlc_block.h>
#include <vlc_image.h>
#include <vlc_aout.h>
#include <arpa/inet.h>

#include <DeckLinkAPI.h>
#include <DeckLinkAPIDispatch.cpp>

#define FRAME_SIZE 1920
#define CHANNELS_MAX 6

#if 0
static const int pi_channels_maps[CHANNELS_MAX+1] =
{
    0,
    AOUT_CHAN_CENTER,
    AOUT_CHANS_STEREO,
    AOUT_CHANS_3_0,
    AOUT_CHANS_4_0,
    AOUT_CHANS_5_0,
    AOUT_CHANS_5_1,
};
#endif

#define NOSIGNAL_INDEX_TEXT N_("Timelength after which we assume there is no signal.")
#define NOSIGNAL_INDEX_LONGTEXT N_(\
    "Timelength after which we assume there is no signal.\n"\
    "After this delay we black out the video."\
    )

#define AFD_INDEX_TEXT "Active Format Descriptor"
#define AFD_INDEX_LONGTEXT AFD_INDEX_TEXT " value"

#define AR_INDEX_TEXT "Aspect Ratio"
#define AR_INDEX_LONGTEXT AR_INDEX_TEXT " of the source picture"

#define AFDLINE_INDEX_TEXT N_("Active Format Descriptor line.")
#define AFDLINE_INDEX_LONGTEXT N_("VBI line on which to output Active Format Descriptor.")

#define NOSIGNAL_IMAGE_TEXT N_("Picture to display on input signal loss.")
#define NOSIGNAL_IMAGE_LONGTEXT NOSIGNAL_IMAGE_TEXT

#define CARD_INDEX_TEXT N_("Output card")
#define CARD_INDEX_LONGTEXT N_(\
    "DeckLink output card, if multiple exist. " \
    "The cards are numbered from 0.")

#define MODE_TEXT N_("Desired output mode")
#define MODE_LONGTEXT N_(\
    "Desired output mode for DeckLink output. " \
    "This value should be a FOURCC code in textual " \
    "form, e.g. \"ntsc\".")

#define AUDIO_CONNECTION_TEXT N_("Audio connection")
#define AUDIO_CONNECTION_LONGTEXT N_(\
    "Audio connection for DeckLink output.")


#define RATE_TEXT N_("Audio samplerate (Hz)")
#define RATE_LONGTEXT N_(\
    "Audio sampling rate (in hertz) for DeckLink output. " \
    "0 disables audio output.")

#define CHANNELS_TEXT N_("Number of audio channels")
#define CHANNELS_LONGTEXT N_(\
    "Number of output channels for DeckLink output. " \
    "Must be 2, 8 or 16. 0 disables audio output.")

#define VIDEO_CONNECTION_TEXT N_("Video connection")
#define VIDEO_CONNECTION_LONGTEXT N_(\
    "Video connection for DeckLink output.")

#define VIDEO_TENBITS_TEXT N_("10 bits")
#define VIDEO_TENBITS_LONGTEXT N_(\
    "Use 10 bits per pixel for video frames.")

#define CFG_PREFIX "decklink-output-"
#define VIDEO_CFG_PREFIX "decklink-vout-"
#define AUDIO_CFG_PREFIX "decklink-aout-"



static const char *const ppsz_videoconns[] = {
    "sdi", "hdmi", "opticalsdi", "component", "composite", "svideo"
};
static const char *const ppsz_videoconns_text[] = {
    N_("SDI"), N_("HDMI"), N_("Optical SDI"), N_("Component"), N_("Composite"), N_("S-video")
};

static const int rgi_afd_values[] = {
    0, 2, 3, 4, 8, 9, 10, 11, 13, 14, 15,
};
static const char * const rgsz_afd_text[] = {
    "0:  Undefined",
    "2:  Box 16:9 (top aligned)",
    "3:  Box 14:9 (top aligned)",
    "4:  Box > 16:9 (centre aligned)",
    "8:  Same as coded frame (full frame)",
    "9:   4:3 (centre aligned)",
    "10: 16:9 (centre aligned)",
    "11: 14:9 (centre aligned)",
    "13:  4:3 (with shoot and protect 14:9 centre)",
    "14: 16:9 (with shoot and protect 14:9 centre)",
    "15: 16:9 (with shoot and protect  4:3 centre)",
};

static const int rgi_ar_values[] = {
    0, 1,
};
static const char * const rgsz_ar_text[] = {
    "0:   4:3",
    "1:  16:9",
};

struct vout_display_sys_t
{
    picture_pool_t *pool;
    bool tenbits;
    uint8_t afd, ar;
    int nosignal_delay;
    picture_t *pic_nosignal;
};

/* Only one audio output module and one video output module
 * can be used per process.
 * We use a static mutex in audio/video submodules entry points.  */
struct decklink_sys_t
{
    IDeckLinkOutput *p_output;

    /*
     * Synchronizes aout and vout modules:
     * vout module waits until aout has been initialized.
     * That means video-only output is NOT supported.
     */
    vlc_mutex_t lock;
    vlc_cond_t cond;
    uint8_t users;

    //int i_channels;
    int i_rate;

    int i_width;
    int i_height;

    BMDTimeScale timescale;
    BMDTimeValue frameduration;

    /* XXX: workaround card clock drift */
    mtime_t offset;
};

/*****************************************************************************
 * Local prototypes.
 *****************************************************************************/

static int  OpenVideo           (vlc_object_t *);
static void CloseVideo          (vlc_object_t *);
static int  OpenAudio           (vlc_object_t *);
static void CloseAudio          (vlc_object_t *);

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/

vlc_module_begin()
    set_shortname(N_("DecklinkOutput"))
    set_description(N_("output module to write to Blackmagic SDI card"))
    set_section(N_("DeckLink General Options"), NULL)
    add_integer(CFG_PREFIX "card-index", 0,
                CARD_INDEX_TEXT, CARD_INDEX_LONGTEXT, true)

    add_submodule ()
    set_description (N_("DeckLink Video Output module"))
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VOUT)
    set_capability("vout display", 0)
    set_callbacks (OpenVideo, CloseVideo)
    set_section(N_("DeckLink Video Options"), NULL)
    add_string(VIDEO_CFG_PREFIX "video-connection", "sdi",
                VIDEO_CONNECTION_TEXT, VIDEO_CONNECTION_LONGTEXT, true)
                change_string_list(ppsz_videoconns, ppsz_videoconns_text)
    add_string(VIDEO_CFG_PREFIX "mode", "",
                MODE_TEXT, MODE_LONGTEXT, true)
    add_bool(VIDEO_CFG_PREFIX "tenbits", false,
                VIDEO_TENBITS_TEXT, VIDEO_TENBITS_LONGTEXT, true)
    add_integer(VIDEO_CFG_PREFIX "nosignal-delay", 5,
                NOSIGNAL_INDEX_TEXT, NOSIGNAL_INDEX_LONGTEXT, true)
    add_integer(VIDEO_CFG_PREFIX "afd-line", 16,
                AFDLINE_INDEX_TEXT, AFDLINE_INDEX_LONGTEXT, true)
    add_integer_with_range(VIDEO_CFG_PREFIX "afd", 8, 0, 16,
                AFD_INDEX_TEXT, AFD_INDEX_LONGTEXT, true)
                change_integer_list(rgi_afd_values, rgsz_afd_text)
    add_integer_with_range(VIDEO_CFG_PREFIX "ar", 1, 0, 1,
                AR_INDEX_TEXT, AR_INDEX_LONGTEXT, true)
                change_integer_list(rgi_ar_values, rgsz_ar_text)
    add_loadfile(VIDEO_CFG_PREFIX "nosignal-image", NULL,
                NOSIGNAL_IMAGE_TEXT, NOSIGNAL_IMAGE_LONGTEXT, true)


    add_submodule ()
    set_description (N_("DeckLink Audio Output module"))
    set_category(CAT_AUDIO)
    set_subcategory(SUBCAT_AUDIO_AOUT)
    set_capability("audio output", 0)
    set_callbacks (OpenAudio, CloseAudio)
    set_section(N_("DeckLink Audio Options"), NULL)
    add_obsolete_string("audio-connection")
    add_integer(AUDIO_CFG_PREFIX "audio-rate", 48000,
                RATE_TEXT, RATE_LONGTEXT, true)
    add_integer(AUDIO_CFG_PREFIX "audio-channels", 2,
                CHANNELS_TEXT, CHANNELS_LONGTEXT, true)
vlc_module_end ()

/* Protects decklink_sys_t creation/deletion */
static vlc_mutex_t sys_lock = VLC_STATIC_MUTEX;

static struct decklink_sys_t *GetDLSys(vlc_object_t *obj)
{
    vlc_object_t *libvlc = VLC_OBJECT(obj->obj.libvlc);
    struct decklink_sys_t *sys;

    vlc_mutex_lock(&sys_lock);

    if (var_Type(libvlc, "decklink-sys") == VLC_VAR_ADDRESS)
        sys = (struct decklink_sys_t*)var_GetAddress(libvlc, "decklink-sys");
    else {
        sys = (struct decklink_sys_t*)malloc(sizeof(*sys));
        if (sys) {
            sys->p_output = NULL;
            sys->offset = 0;
            sys->users = 0;
            sys->i_rate = var_InheritInteger(obj, AUDIO_CFG_PREFIX "audio-rate");
            if(sys->i_rate > 0)
                sys->i_rate = -1;
            vlc_mutex_init(&sys->lock);
            vlc_cond_init(&sys->cond);
            var_Create(libvlc, "decklink-sys", VLC_VAR_ADDRESS);
            var_SetAddress(libvlc, "decklink-sys", (void*)sys);
        }
    }

    vlc_mutex_unlock(&sys_lock);
    return sys;
}

static void ReleaseDLSys(vlc_object_t *obj)
{
    vlc_object_t *libvlc = VLC_OBJECT(obj->obj.libvlc);

    vlc_mutex_lock(&sys_lock);

    struct decklink_sys_t *sys = (struct decklink_sys_t*)var_GetAddress(libvlc, "decklink-sys");

    if (--sys->users == 0) {
        msg_Dbg(obj, "Destroying decklink data");
        vlc_mutex_destroy(&sys->lock);
        vlc_cond_destroy(&sys->cond);

        if (sys->p_output) {
            sys->p_output->StopScheduledPlayback(0, NULL, 0);
            sys->p_output->DisableVideoOutput();
            sys->p_output->DisableAudioOutput();
            sys->p_output->Release();
        }

        free(sys);
        var_Destroy(libvlc, "decklink-sys");
    }

    vlc_mutex_unlock(&sys_lock);
}

static BMDVideoConnection getVConn(vout_display_t *vd)
{
    BMDVideoConnection conn = bmdVideoConnectionSDI;
    char *psz = var_InheritString(vd, VIDEO_CFG_PREFIX "video-connection");
    if (!psz)
        goto end;

         if (!strcmp(psz, "sdi"))
        conn = bmdVideoConnectionSDI;
    else if (!strcmp(psz, "hdmi"))
        conn = bmdVideoConnectionHDMI;
    else if (!strcmp(psz, "opticalsdi"))
        conn = bmdVideoConnectionOpticalSDI;
    else if (!strcmp(psz, "component"))
        conn = bmdVideoConnectionComponent;
    else if (!strcmp(psz, "composite"))
        conn = bmdVideoConnectionComposite;
    else if (!strcmp(psz, "svideo"))
        conn = bmdVideoConnectionSVideo;

end:
    free(psz);
    return conn;
}

/*****************************************************************************
 *
 *****************************************************************************/

static struct
{
    long i_return_code;
    const char * const psz_string;
} const errors_to_string[] = {
    { E_UNEXPECTED,  "Unexpected error" },
    { E_NOTIMPL,     "Not implemented" },
    { E_OUTOFMEMORY, "Out of memory" },
    { E_INVALIDARG,  "Invalid argument" },
    { E_NOINTERFACE, "No interface" },
    { E_POINTER,     "Invalid pointer" },
    { E_HANDLE,      "Invalid handle" },
    { E_ABORT,       "Aborted" },
    { E_FAIL,        "Failed" },
    { E_ACCESSDENIED,"Access denied" }
};

static const char * lookup_error_string(long i_code)
{
    for(size_t i=0; i<ARRAY_SIZE(errors_to_string); i++)
    {
        if(errors_to_string[i].i_return_code == i_code)
            return errors_to_string[i].psz_string;
    }
    return NULL;
}

static IDeckLinkDisplayMode * MatchDisplayMode(vout_display_t *vd,
                                               IDeckLinkOutput *output,
                                               const video_format_t *fmt,
                                               BMDDisplayMode forcedmode = bmdDisplayModeNotSupported)
{
    HRESULT result;
    IDeckLinkDisplayMode *p_selected = NULL;
    IDeckLinkDisplayModeIterator *p_iterator = NULL;

    for(int i=0; i<4 && p_selected==NULL; i++)
    {
        int i_width = (i % 2 == 0) ? fmt->i_width : fmt->i_visible_width;
        int i_height = (i % 2 == 0) ? fmt->i_height : fmt->i_visible_height;
        int i_div = (i > 2) ? 4 : 0;

        result = output->GetDisplayModeIterator(&p_iterator);
        if(result == S_OK)
        {
            IDeckLinkDisplayMode *p_mode = NULL;
            while(p_iterator->Next(&p_mode) == S_OK)
            {
                BMDDisplayMode mode_id = p_mode->GetDisplayMode();
                BMDTimeValue frameduration;
                BMDTimeScale timescale;
                const char *psz_mode_name;

                if(p_mode->GetFrameRate(&frameduration, &timescale) == S_OK &&
                        p_mode->GetName(&psz_mode_name) == S_OK)
                {
                    BMDDisplayMode modenl = htonl(mode_id);
                    if(i==0)
                    {
                        msg_Dbg(vd, "Found mode '%4.4s': %s (%ldx%ld, %.3f fps, scale %ld dur %ld)",
                                (char*)&modenl, psz_mode_name,
                                p_mode->GetWidth(), p_mode->GetHeight(),
                                double(timescale / frameduration),
                                timescale, frameduration);
                    }
                }
                else
                {
                    p_mode->Release();
                    continue;
                }

                if(forcedmode != bmdDisplayModeNotSupported && unlikely(!p_selected))
                {
                    BMDDisplayMode modenl = htonl(forcedmode);
                    msg_Dbg(vd, "Forced mode '%4.4s'", (char *)&modenl);
                    if(forcedmode == mode_id)
                        p_selected = p_mode;
                    else
                        p_mode->Release();
                    continue;
                }

                if(p_selected == NULL)
                {
                    if(i_width >> i_div == p_mode->GetWidth() >> i_div &&
                       i_height >> i_div == p_mode->GetHeight() >> i_div)
                    {
                        unsigned int num_deck, den_deck;
                        unsigned int num_stream, den_stream;
                        vlc_ureduce(&num_deck, &den_deck, timescale, frameduration, 0);
                        vlc_ureduce(&num_stream, &den_stream,
                                    fmt->i_frame_rate, fmt->i_frame_rate_base, 0);

                        if (num_deck == num_stream && den_deck == den_stream)
                        {
                            msg_Info(vd, "Matches incoming stream");
                            p_selected = p_mode;
                            continue;
                        }
                    }
                }

                p_mode->Release();
            }
            p_iterator->Release();
        }
    }
    return p_selected;
}

static struct decklink_sys_t *OpenDecklink(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;
    video_format_t *fmt = &vd->fmt;
#define CHECK(message) do { \
    if (result != S_OK) \
    { \
        const char *psz_err = lookup_error_string(result); \
        if(psz_err)\
            msg_Err(vd, message ": %s", psz_err); \
        else \
            msg_Err(vd, message ": 0x%X", result); \
        goto error; \
    } \
} while(0)

    HRESULT result;
    IDeckLinkIterator *decklink_iterator = NULL;
    IDeckLinkDisplayMode *p_display_mode = NULL;
    IDeckLinkConfiguration *p_config = NULL;
    IDeckLink *p_card = NULL;
    BMDDisplayMode wanted_mode_id = bmdDisplayModeNotSupported;

    struct decklink_sys_t *decklink_sys = GetDLSys(VLC_OBJECT(vd));
    vlc_mutex_lock(&decklink_sys->lock);
    decklink_sys->users++;

    /* wait until aout is ready */
    msg_Info(vd, "Waiting for DeckLink audio input module to start");
    while (decklink_sys->i_rate == -1)
        vlc_cond_wait(&decklink_sys->cond, &decklink_sys->lock);

    int i_card_index = var_InheritInteger(vd, CFG_PREFIX "card-index");
    BMDVideoConnection vconn = getVConn(vd);
    char *mode = var_InheritString(vd, VIDEO_CFG_PREFIX "mode");
    if(mode)
    {
        size_t len = strlen(mode);
        if (len > 4)
        {
            free(mode);
            msg_Err(vd, "Invalid mode %s", mode);
            goto error;
        }
        memset(&wanted_mode_id, ' ', 4);
        strncpy((char*)&wanted_mode_id, mode, 4);
        wanted_mode_id = ntohl(wanted_mode_id);
        free(mode);
    }

    if (i_card_index < 0)
    {
        msg_Err(vd, "Invalid card index %d", i_card_index);
        goto error;
    }

    decklink_iterator = CreateDeckLinkIteratorInstance();
    if (!decklink_iterator)
    {
        msg_Err(vd, "DeckLink drivers not found.");
        goto error;
    }

    for(int i = 0; i <= i_card_index; ++i)
    {
        if (p_card)
            p_card->Release();
        result = decklink_iterator->Next(&p_card);
        CHECK("Card not found");
    }

    const char *psz_model_name;
    result = p_card->GetModelName(&psz_model_name);
    CHECK("Unknown model name");

    msg_Dbg(vd, "Opened DeckLink PCI card %s", psz_model_name);

    result = p_card->QueryInterface(IID_IDeckLinkOutput,
        (void**)&decklink_sys->p_output);
    CHECK("No outputs");

    result = p_card->QueryInterface(IID_IDeckLinkConfiguration,
        (void**)&p_config);
    CHECK("Could not get config interface");

    if (vconn)
    {
        result = p_config->SetInt(
            bmdDeckLinkConfigVideoOutputConnection, vconn);
        CHECK("Could not set video output connection");
    }

    p_display_mode = MatchDisplayMode(vd, decklink_sys->p_output,
                                          fmt, wanted_mode_id);
    if(p_display_mode == NULL)
    {
        msg_Err(vd, "Could not negociate a compatible display mode");
        goto error;
    }
    else
    {
        BMDDisplayMode mode_id = p_display_mode->GetDisplayMode();
        BMDDisplayMode modenl = htonl(mode_id);
        msg_Dbg(vd, "Selected mode '%4.4s'", (char *) &modenl);

        BMDVideoOutputFlags flags = bmdVideoOutputVANC;
        if (mode_id == bmdModeNTSC ||
            mode_id == bmdModeNTSC2398 ||
            mode_id == bmdModePAL)
        {
            flags = bmdVideoOutputVITC;
        }

        BMDDisplayModeSupport support;
        IDeckLinkDisplayMode *resultMode;

        result = decklink_sys->p_output->DoesSupportVideoMode(mode_id,
                                                              sys->tenbits ? bmdFormat10BitYUV : bmdFormat8BitYUV,
                                                              flags, &support, &resultMode);
        CHECK("Does not support video mode");
        if (support == bmdDisplayModeNotSupported)
        {
            msg_Err(vd, "Video mode not supported");
            goto error;
        }

        decklink_sys->i_width = p_display_mode->GetWidth();
        decklink_sys->i_height = p_display_mode->GetHeight();
        if (decklink_sys->i_width <= 0 || decklink_sys->i_width & 1)
        {
             msg_Err(vd, "Unknown video mode specified.");
             goto error;
        }

        result = p_display_mode->GetFrameRate(&decklink_sys->frameduration,
                                              &decklink_sys->timescale);
        CHECK("Could not read frame rate");

        result = decklink_sys->p_output->EnableVideoOutput(mode_id, flags);
        CHECK("Could not enable video output");
    }

    if (/*decklink_sys->i_channels > 0 &&*/ decklink_sys->i_rate > 0)
    {
        result = decklink_sys->p_output->EnableAudioOutput(
            decklink_sys->i_rate,
            bmdAudioSampleType16bitInteger,
            /*decklink_sys->i_channels*/ 2,
            bmdAudioOutputStreamTimestamped);
        CHECK("Could not start audio output");
    }

    /* start */
    result = decklink_sys->p_output->StartScheduledPlayback(
        (mdate() * decklink_sys->timescale) / CLOCK_FREQ, decklink_sys->timescale, 1.0);
    CHECK("Could not start playback");

    p_config->Release();
    p_display_mode->Release();
    p_card->Release();
    decklink_iterator->Release();

    vlc_mutex_unlock(&decklink_sys->lock);

    vout_display_DeleteWindow(vd, NULL);

    return decklink_sys;

error:
    if (decklink_sys->p_output) {
        decklink_sys->p_output->Release();
        decklink_sys->p_output = NULL;
    }
    if (p_card)
        p_card->Release();
    if (p_config)
        p_config->Release();
    if (decklink_iterator)
        decklink_iterator->Release();
    if (p_display_mode)
        p_display_mode->Release();

    vlc_mutex_unlock(&decklink_sys->lock);
    ReleaseDLSys(VLC_OBJECT(vd));

    return NULL;
#undef CHECK
}

/*****************************************************************************
 * Video
 *****************************************************************************/

static picture_pool_t *PoolVideo(vout_display_t *vd, unsigned requested_count)
{
    vout_display_sys_t *sys = vd->sys;
    if (!sys->pool)
        sys->pool = picture_pool_NewFromFormat(&vd->fmt, requested_count);
    return sys->pool;
}

static inline void put_le32(uint8_t **p, uint32_t d)
{
    SetDWLE(*p, d);
    (*p) += 4;
}

static inline int clip(int a)
{
    if      (a < 4) return 4;
    else if (a > 1019) return 1019;
    else               return a;
}

static void v210_convert(void *frame_bytes, picture_t *pic, int dst_stride)
{
    int width = pic->format.i_width;
    int height = pic->format.i_height;
    int line_padding = dst_stride - ((width * 8 + 11) / 12) * 4;
    int h, w;
    uint8_t *data = (uint8_t*)frame_bytes;

    const uint16_t *y = (const uint16_t*)pic->p[0].p_pixels;
    const uint16_t *u = (const uint16_t*)pic->p[1].p_pixels;
    const uint16_t *v = (const uint16_t*)pic->p[2].p_pixels;

#define WRITE_PIXELS(a, b, c)           \
    do {                                \
        val =   clip(*a++);             \
        val |= (clip(*b++) << 10) |     \
               (clip(*c++) << 20);      \
        put_le32(&data, val);           \
    } while (0)

    for (h = 0; h < height; h++) {
        uint32_t val = 0;
        for (w = 0; w < width - 5; w += 6) {
            WRITE_PIXELS(u, y, v);
            WRITE_PIXELS(y, u, y);
            WRITE_PIXELS(v, y, u);
            WRITE_PIXELS(y, v, y);
        }
        if (w < width - 1) {
            WRITE_PIXELS(u, y, v);

            val = clip(*y++);
            if (w == width - 2)
                put_le32(&data, val);
#undef WRITE_PIXELS
        }
        if (w < width - 3) {
            val |= (clip(*u++) << 10) | (clip(*y++) << 20);
            put_le32(&data, val);

            val = clip(*v++) | (clip(*y++) << 10);
            put_le32(&data, val);
        }

        memset(data, 0, line_padding);
        data += line_padding;

        y += pic->p[0].i_pitch / 2 - width;
        u += pic->p[1].i_pitch / 2 - width / 2;
        v += pic->p[2].i_pitch / 2 - width / 2;
    }
}

static void send_AFD(uint8_t afdcode, uint8_t ar, uint8_t *buf)
{
    const size_t len = 6 /* vanc header */ + 8 /* AFD data */ + 1 /* csum */;
    const size_t s = ((len + 5) / 6) * 6; // align for v210

    uint16_t afd[s];

    afd[0] = 0x000;
    afd[1] = 0x3ff;
    afd[2] = 0x3ff;
    afd[3] = 0x41; // DID
    afd[4] = 0x05; // SDID
    afd[5] = 8; // Data Count

    int bar_data_flags = 0;
    int bar_data_val1 = 0;
    int bar_data_val2 = 0;

    afd[ 6] = ((afdcode & 0x0F) << 3) | ((ar & 0x01) << 2); /* SMPTE 2016-1 */
    afd[ 7] = 0; // reserved
    afd[ 8] = 0; // reserved
    afd[ 9] = bar_data_flags << 4;
    afd[10] = bar_data_val1 << 8;
    afd[11] = bar_data_val1 & 0xff;
    afd[12] = bar_data_val2 << 8;
    afd[13] = bar_data_val2 & 0xff;

    /* parity bit */
    for (size_t i = 3; i < len - 1; i++)
        afd[i] |= parity(afd[i]) ? 0x100 : 0x200;

    /* vanc checksum */
    uint16_t vanc_sum = 0;
    for (size_t i = 3; i < len - 1; i++) {
        vanc_sum += afd[i];
        vanc_sum &= 0x1ff;
    }

    afd[len - 1] = vanc_sum | ((~vanc_sum & 0x100) << 1);

    /* pad */
    for (size_t i = len; i < s; i++)
        afd[i] = 0x040;

    /* convert to v210 and write into VANC */
    for (size_t w = 0; w < s / 6 ; w++) {
        put_le32(&buf, afd[w*6+0] << 10);
        put_le32(&buf, afd[w*6+1] | (afd[w*6+2] << 20));
        put_le32(&buf, afd[w*6+3] << 10);
        put_le32(&buf, afd[w*6+4] | (afd[w*6+5] << 20));
    }
}

static void DisplayVideo(vout_display_t *vd, picture_t *picture, subpicture_t *)
{
    vout_display_sys_t *sys = vd->sys;
    struct decklink_sys_t *decklink_sys = GetDLSys(VLC_OBJECT(vd));
    mtime_t now = mdate();

    if (!picture)
        return;

    picture_t *orig_picture = picture;

    if (now - picture->date > sys->nosignal_delay * CLOCK_FREQ) {
        msg_Dbg(vd, "no signal");
        if (sys->pic_nosignal) {
            picture = sys->pic_nosignal;
        } else {
            if (sys->tenbits) { // I422_10L
                plane_t *y = &picture->p[0];
                memset(y->p_pixels, 0x0, y->i_lines * y->i_pitch);
                for (int i = 1; i < picture->i_planes; i++) {
                    plane_t *p = &picture->p[i];
                    size_t len = p->i_lines * p->i_pitch / 2;
                    int16_t *data = (int16_t*)p->p_pixels;
                    for (size_t j = 0; j < len; j++) // XXX: SIMD
                        data[j] = 0x200;
                }
            } else { // UYVY
                size_t len = picture->p[0].i_lines * picture->p[0].i_pitch;
                for (size_t i = 0; i < len; i+= 2) { // XXX: SIMD
                    picture->p[0].p_pixels[i+0] = 0x80;
                    picture->p[0].p_pixels[i+1] = 0;
                }
            }
        }
        picture->date = now;
    }

    HRESULT result;
    int w, h, stride, length;
    w = decklink_sys->i_width;
    h = decklink_sys->i_height;

    IDeckLinkMutableVideoFrame *pDLVideoFrame;
    result = decklink_sys->p_output->CreateVideoFrame(w, h, w*3,
        sys->tenbits ? bmdFormat10BitYUV : bmdFormat8BitYUV,
        bmdFrameFlagDefault, &pDLVideoFrame);

    if (result != S_OK) {
        msg_Err(vd, "Failed to create video frame: 0x%X", result);
        pDLVideoFrame = NULL;
        goto end;
    }

    void *frame_bytes;
    pDLVideoFrame->GetBytes((void**)&frame_bytes);
    stride = pDLVideoFrame->GetRowBytes();

    if (sys->tenbits) {
        IDeckLinkVideoFrameAncillary *vanc;
        int line;
        void *buf;

        result = decklink_sys->p_output->CreateAncillaryData(
                sys->tenbits ? bmdFormat10BitYUV : bmdFormat8BitYUV, &vanc);
        if (result != S_OK) {
            msg_Err(vd, "Failed to create vanc: %d", result);
            goto end;
        }

        line = var_InheritInteger(vd, VIDEO_CFG_PREFIX "afd-line");
        result = vanc->GetBufferForVerticalBlankingLine(line, &buf);
        if (result != S_OK) {
            msg_Err(vd, "Failed to get VBI line %d: %d", line, result);
            goto end;
        }
        send_AFD(vd->sys->afd, vd->sys->ar, (uint8_t*)buf);

        v210_convert(frame_bytes, picture, stride);

        result = pDLVideoFrame->SetAncillaryData(vanc);
        vanc->Release();
        if (result != S_OK) {
            msg_Err(vd, "Failed to set vanc: %d", result);
            goto end;
        }
    }
    else for(int y = 0; y < h; ++y) {
        uint8_t *dst = (uint8_t *)frame_bytes + stride * y;
        const uint8_t *src = (const uint8_t *)picture->p[0].p_pixels +
            picture->p[0].i_pitch * y;
        memcpy(dst, src, w * 2 /* bpp */);
    }


    // compute frame duration in CLOCK_FREQ units
    length = (decklink_sys->frameduration * CLOCK_FREQ) / decklink_sys->timescale;

    picture->date -= decklink_sys->offset;
    result = decklink_sys->p_output->ScheduleVideoFrame(pDLVideoFrame,
        picture->date, length, CLOCK_FREQ);

    if (result != S_OK) {
        msg_Err(vd, "Dropped Video frame %" PRId64 ": 0x%x",
            picture->date, result);
        goto end;
    }

    now = mdate() - decklink_sys->offset;

    BMDTimeValue decklink_now;
    double speed;
    decklink_sys->p_output->GetScheduledStreamTime (CLOCK_FREQ, &decklink_now, &speed);

    if ((now - decklink_now) > 400000) {
        /* XXX: workaround card clock drift */
        decklink_sys->offset += 50000;
        msg_Err(vd, "Delaying: offset now %" PRId64, decklink_sys->offset);
    }

end:
    if (pDLVideoFrame)
        pDLVideoFrame->Release();
    picture_Release(orig_picture);
}

static int ControlVideo(vout_display_t *vd, int query, va_list args)
{
    (void) vd; (void) query; (void) args;
    return VLC_EGENERIC;
}

static int OpenVideo(vlc_object_t *p_this)
{
    vout_display_t *vd = (vout_display_t *)p_this;
    vout_display_sys_t *sys;
    struct decklink_sys_t *decklink_sys;

    vd->sys = sys = (vout_display_sys_t*)malloc(sizeof(*sys));
    if (!sys)
        return VLC_ENOMEM;

    sys->tenbits = var_InheritBool(p_this, VIDEO_CFG_PREFIX "tenbits");
    sys->nosignal_delay = var_InheritInteger(p_this, VIDEO_CFG_PREFIX "nosignal-delay");
    sys->afd = var_InheritInteger(p_this, VIDEO_CFG_PREFIX "afd");
    sys->ar = var_InheritInteger(p_this, VIDEO_CFG_PREFIX "ar");
    sys->pic_nosignal = NULL;

    decklink_sys = OpenDecklink(vd);
    if (!decklink_sys) {
        if (sys->pic_nosignal)
            picture_Release(sys->pic_nosignal);
        free(sys);
        return VLC_EGENERIC;
    }

    sys->pool = NULL;

    vd->fmt.i_chroma = sys->tenbits
        ? VLC_CODEC_I422_10L /* we will convert to v210 */
        : VLC_CODEC_UYVY;
    //video_format_FixRgb(&(vd->fmt));

    vd->fmt.i_width = decklink_sys->i_width;
    vd->fmt.i_height = decklink_sys->i_height;

    char *pic_file = var_InheritString(p_this, VIDEO_CFG_PREFIX "nosignal-image");
    if (pic_file) {
        image_handler_t *img = image_HandlerCreate(p_this);
        if (!img) {
            msg_Err(p_this, "Could not create image converter");
        } else {
            video_format_t in, dummy;

            video_format_Init(&in, 0);
            video_format_Setup(&in, 0, vd->fmt.i_width, vd->fmt.i_height,
                    vd->fmt.i_width, vd->fmt.i_height, 1, 1);

            video_format_Init(&dummy, 0);

            picture_t *png = image_ReadUrl(img, pic_file, &dummy, &in);
            if (png) {
                msg_Err(p_this, "Converting");
                sys->pic_nosignal = image_Convert(img, png, &in, &vd->fmt);
                picture_Release(png);
            }

            image_HandlerDelete(img);
        }

        free(pic_file);
        if (!sys->pic_nosignal) {
            CloseVideo(p_this);
            msg_Err(p_this, "Could not create no signal picture");
            return VLC_EGENERIC;
        }
    }
    vd->info.has_hide_mouse = true;
    vd->pool    = PoolVideo;
    vd->prepare = NULL;
    vd->display = DisplayVideo;
    vd->control = ControlVideo;
    vd->manage  = NULL;
    vout_display_SendEventFullscreen(vd, false);

    return VLC_SUCCESS;
}

static void CloseVideo(vlc_object_t *p_this)
{
    vout_display_t *vd = (vout_display_t *)p_this;
    vout_display_sys_t *sys = vd->sys;

    if (sys->pool)
        picture_pool_Release(sys->pool);

    if (sys->pic_nosignal)
        picture_Release(sys->pic_nosignal);

    free(sys);

    ReleaseDLSys(p_this);
}

/*****************************************************************************
 * Audio
 *****************************************************************************/

static void Flush (audio_output_t *aout, bool drain)
{
    struct decklink_sys_t *decklink_sys = GetDLSys(VLC_OBJECT(aout));
    vlc_mutex_lock(&decklink_sys->lock);
    IDeckLinkOutput *p_output = decklink_sys->p_output;
    vlc_mutex_unlock(&decklink_sys->lock);
    if (!p_output)
        return;

    if (drain) {
        uint32_t samples;
        decklink_sys->p_output->GetBufferedAudioSampleFrameCount(&samples);
        msleep(CLOCK_FREQ * samples / decklink_sys->i_rate);
    } else if (decklink_sys->p_output->FlushBufferedAudioSamples() == E_FAIL)
        msg_Err(aout, "Flush failed");
}

static int TimeGet(audio_output_t *, mtime_t* restrict)
{
    /* synchronization is handled by the card */
    return -1;
}

static int Start(audio_output_t *aout, audio_sample_format_t *restrict fmt)
{
    struct decklink_sys_t *decklink_sys = GetDLSys(VLC_OBJECT(aout));

    if (decklink_sys->i_rate == 0)
        return VLC_EGENERIC;

    fmt->i_format = VLC_CODEC_S16N;
    fmt->i_channels = 2; //decklink_sys->i_channels;
    fmt->i_physical_channels = AOUT_CHANS_STEREO; //pi_channels_maps[fmt->i_channels];
    fmt->i_rate = decklink_sys->i_rate;
    fmt->i_bitspersample = 16;
    fmt->i_blockalign = fmt->i_channels * fmt->i_bitspersample /8 ;
    fmt->i_frame_length  = FRAME_SIZE;

    return VLC_SUCCESS;
}

static void PlayAudio(audio_output_t *aout, block_t *audio)
{
    struct decklink_sys_t *decklink_sys = GetDLSys(VLC_OBJECT(aout));
    vlc_mutex_lock(&decklink_sys->lock);
    IDeckLinkOutput *p_output = decklink_sys->p_output;
    vlc_mutex_unlock(&decklink_sys->lock);
    if (!p_output) {
        block_Release(audio);
        return;
    }

    audio->i_pts -= decklink_sys->offset;

    uint32_t sampleFrameCount = audio->i_buffer / (2 * 2 /*decklink_sys->i_channels*/);
    uint32_t written;
    HRESULT result = decklink_sys->p_output->ScheduleAudioSamples(
            audio->p_buffer, sampleFrameCount, audio->i_pts, CLOCK_FREQ, &written);

    if (result != S_OK)
        msg_Err(aout, "Failed to schedule audio sample: 0x%X", result);
    else if (sampleFrameCount != written)
        msg_Err(aout, "Written only %d samples out of %d", written, sampleFrameCount);

    block_Release(audio);
}

static int OpenAudio(vlc_object_t *p_this)
{
    audio_output_t *aout = (audio_output_t *)p_this;
    struct decklink_sys_t *decklink_sys = GetDLSys(VLC_OBJECT(aout));

    vlc_mutex_lock(&decklink_sys->lock);
    //decklink_sys->i_channels = var_InheritInteger(vd, AUDIO_CFG_PREFIX "audio-channels");
    decklink_sys->i_rate = var_InheritInteger(aout, AUDIO_CFG_PREFIX "audio-rate");
    decklink_sys->users++;
    vlc_cond_signal(&decklink_sys->cond);
    vlc_mutex_unlock(&decklink_sys->lock);

    aout->play      = PlayAudio;
    aout->start     = Start;
    aout->flush     = Flush;
    aout->time_get  = TimeGet;

    aout->pause     = NULL;
    aout->stop      = NULL;
    aout->mute_set  = NULL;
    aout->volume_set= NULL;

    return VLC_SUCCESS;
}

static void CloseAudio(vlc_object_t *p_this)
{
    struct decklink_sys_t *decklink_sys = GetDLSys(p_this);
    vlc_mutex_lock(&decklink_sys->lock);
    vlc_mutex_unlock(&decklink_sys->lock);
    ReleaseDLSys(p_this);
}
