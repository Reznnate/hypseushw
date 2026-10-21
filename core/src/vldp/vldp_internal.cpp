/*
 * ____ VLDP COPYRIGHT NOTICE ____
 *
 * Copyright (C) 2001 Matt Ownby
 *
 * This file is part of VLDP, a virtual laserdisc player.
 *
 * VLDP is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * VLDP is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

// contains the function for the VLDP private thread

#include <stdio.h>
#include <stdlib.h> // for malloc/free
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <SDL3/SDL.h>

#include "vldp_internal.h"
#include "vldp_nonstd.h"
#include "vldp_common.h"
#include "mpegscan.h"
#include "../video/video.h"
#include "../io/conout.h"

#include <plog/Log.h>

#include <inttypes.h>

#include <mpeg2.h>

#include "vldp_hwdec.h"
#include "vldp_mp4.h"

#ifdef VLDP_HWDEC
#include <deque>
#include <vector>
static void hw_verify_report();
#endif

#ifdef VLDP_DEBUG
#define FRAMELOG "frame_report.txt"
#endif

#ifdef __LIBRETRO__
// Shared with the game thread and the frontend (see vldp.h).
std::atomic<uint32_t> g_lr_vldp_wait_timer(LR_VLDP_NOT_WAITING);
std::atomic<uint32_t> g_lr_vldp_next_due(0);
std::atomic<int> g_lr_vldp_hwdec(0);
std::atomic<const char *> g_lr_vldp_error(nullptr);
std::atomic<int> g_lr_vldp_shown_pic(-1);
std::atomic<int> g_lr_presented_pic(-1);

// Search response, for the frontend's timing summary: real time until the
// target frame was decoded, and game time until it was shown.  The VLDP
// thread records; the frontend resets and reads.
#define LR_SEARCH_SAMPLES 4096
static float s_lr_search_ready_ms[LR_SEARCH_SAMPLES];
static float s_lr_search_shown_ms[LR_SEARCH_SAMPLES];
static std::atomic<unsigned> s_lr_search_count(0);
static std::atomic<int> s_lr_search_state(0); // 1 waiting for the target, 2 for its display
static uint64_t s_lr_search_t0_ns;
static uint32_t s_lr_search_timer0;
static float s_lr_search_ready;

void vldp_lr_reset_search_stats() { s_lr_search_count = 0; s_lr_search_state = 0; }

unsigned vldp_lr_search_stats(const float **ready_ms, const float **shown_ms)
{
    *ready_ms = s_lr_search_ready_ms;
    *shown_ms = s_lr_search_shown_ms;
    const unsigned n = s_lr_search_count;
    return n < LR_SEARCH_SAMPLES ? n : LR_SEARCH_SAMPLES;
}
#endif

// NOTICE : these variables should only be used by the private thread
// !!!!!!!!!!!!

#ifdef __LIBRETRO__
// frame number of the picture draw_planes() is handling: pictures come in
// display order, one frame each, from the picture decoding restarts at (the
// I-frame before an MPEG-2 search target, an MP4 search's target itself)
static int s_lr_pic = -1;
#endif

Uint8 s_old_req_cmdORcount = CMDORCOUNT_INITIAL; // the last value of the
                                                 // command byte we received
int s_paused         = 0; // whether the video is to be paused
int s_step_forward   = 0; // whether to step 1 frame forward
int s_blanked        = 0; // whether the mpeg video is to be blanked
int s_frames_to_skip = 0; // how many frames to skip before rendering the next
                          // frame (used for P and B frames seeking)
int s_frames_to_skip_with_inc = 0; // // how many frames to skip while
                                   // increasing the frame number (for
                                   // multi-speed playback)
int s_skip_all = 0; // if we are to skip any frame to be displayed (to avoid
                    // seeing frames we shouldn't)

// How many frames we've skipped when s_skip_all is enabled.
// (for performance tuning, we want to minimize this number)
uint32_t s_uSkipAllCount = 0;

Uint32 s_timer = 0; // FPS timer used by the blitting code to run at the right
                    // speed

// any extra delay that draw_frame() will use before drawing a frame
// (intended for laserdisc seek delay simulation)
// NOTE : this value gets reset to 0 after it has been 'used'
Uint32 s_extra_delay_ms = 0;

// how many frames must have been displayed (relative to s_timer!!!)
// before we advance to the next frame
Uint32 s_uFramesShownSinceTimer = 0;

// which frame we've skipped to (0 if there is nothing pending)
uint32_t s_uPendingSkipFrame = 0;

// multi-speed variables ...
unsigned int s_skip_per_frame = 0;  // how many frames to skip per frame (for
                                    // playing at 2X for example)
unsigned int s_stall_per_frame = 0; // how many frames to stall per frame (for
                                    // playing at 1/2X for example)

// pre-cache variables
#define MAX_PRECACHE_FILES 300 // maximum number of files that we'll pre-cache
VLDP_BOOL s_bPreCacheEnabled = VLDP_FALSE; // whether precaching is currently
                                           // enabled
unsigned int s_uCurPreCacheIdx = 0; // if pre-caching is enabled, which index is
                                    // currently being used
unsigned int s_uPreCacheIdxCount = 0; // how many files have been precached
struct precache_entry_s s_sPreCacheEntries[MAX_PRECACHE_FILES]; // struct array
                                                                // holding
                                                                // precache data

#define MAX_LDP_FRAMES 512000

static FILE *g_mpeg_handle     = NULL; // mpeg file we currently have open
static mpeg2dec_t *g_mpeg_data = NULL; // structure for libmpeg2's state
static uint64_t g_frame_position[MAX_LDP_FRAMES] = {0}; // the file position of
                                                      // each I frame
static Uint32 g_totalframes = 0; // total # of frames in the current mpeg

#define BUFFER_SIZE 262144
static Uint8 g_buffer[BUFFER_SIZE]; // buffer to hold mpeg2 file as we read it
                                    // in

#define HEADER_BUF_SIZE 200
static Uint8 g_header_buf[HEADER_BUF_SIZE];
static uint32_t g_header_buf_size = 0; // size of the header buffer

// how many frames we will stall after beginning playback (should be 1, because
// presumably before we start playing, the disc has been paused showing the same
// frame, and we want the frame to display 1 more frame before moving to the
// next one.  This behavior is somewhat arbitrary to act like we think laserdisc
// players act, but this value could also be proven to be 0 if someone does
// future research.)
#define PLAY_FRAME_STALL 1

////////////////////////////////////////////////

// this is our video thread which gets called
int idle_handler(void *surface)
{
    int done = 0;

#ifdef __LIBRETRO__
    lr_register_cpu_thread(LR_THREAD_DECODER);
#endif
    g_mpeg_data = mpeg2_init();

    // unless we are drawing video to the screen, we just sit here
    // and listen for orders from the parent thread
    while (!done) {
        // while we have received new commands to be processed
        // (so we don't go to sleep on skips)
        while (ivldp_got_new_command() && !done) {
            // examine the actual command (strip off the count)
            switch (g_req_cmdORcount & 0xF0) {
            case VLDP_REQ_QUIT:
                done = 1;
                break;
            case VLDP_REQ_OPEN:
                idle_handler_open();
                break;
            case VLDP_REQ_PRECACHE:
                idle_handler_precache();
                break;
            case VLDP_REQ_PLAY:
                idle_handler_play();
                break;
            case VLDP_REQ_SEARCH:
                idle_handler_search(0);
                break;
            case VLDP_REQ_SKIP:
                idle_handler_search(1);
                break;
            case VLDP_REQ_PAUSE: // pause command while we're already idle?
                                 // this is an error
            case VLDP_REQ_STOP:  // stop command while we're already idle? this
                                 // is an error
                g_out_info.status = STAT_ERROR;
                ivldp_ack_command();
                break;
            case VLDP_REQ_LOCK:
                ivldp_lock_handler();
                break;
            default:
                LOGW << "VLDP WARNING : Idle handler received command "
                                "which it is ignoring";
                break;
            }             // end switch
            SDL_Delay(0); // give other threads some breathing room (but not
                          // much hehe)
        }                 // end if we got a new command

        g_in_info->render_blank_frame(); // This makes sure that the video
                                         // overlay gets drawn even if there is
                                         // no video being played

        /* we need to delay here because otherwise, this idle loop will execute
         * at 100% cpu speed and really slow things down It shouldn't hurt us
         * when we get a command that requires immediate attention (such as
         * skip) because of the inner while loop above (while
         * ivldp_got_new_command)
         * NOTE : we want to delay for about 1 frame (or field) here
         */
        SDL_Delay(16); // 1 field is 16.666ms assuming 60 hz

    } // end while we have not received a quit command

    io_close();
    /*
    // if we have a file open, close it
    if (g_mpeg_handle)
    {
        fclose(g_mpeg_handle);
        g_mpeg_handle = 0;
    }
    */

    g_out_info.status = STAT_ERROR;
#ifdef VLDP_HWDEC
    hw_verify_report();
    hwdec::close();
#endif
#ifdef VLDP_MP4
    mp4::close();
#endif
    mpeg2_close(g_mpeg_data);              // shutdown libmpeg2

    // de-allocate any files that have been precached
    while (s_uPreCacheIdxCount > 0) {
        --s_uPreCacheIdxCount;
        free(s_sPreCacheEntries[s_uPreCacheIdxCount].ptrBuf);
    }

    ivldp_ack_command(); // acknowledge quit command

    return 0;
}

// returns 1 if there is a new command waiting for us or 0 otherwise
int ivldp_got_new_command()
{
    int result = 0;

    // if they are no longer equal
    if (g_req_cmdORcount != s_old_req_cmdORcount) {
        result = 1;
    }

    return result;
}

// acknowledges a command sent by the parent thread
// NOTE : We don't check to see if parent thread got our acknowledgement because
// it creates too much latency
void ivldp_ack_command()
{
    s_old_req_cmdORcount = g_req_cmdORcount;
    g_ack_count++; // here is where we acknowledge
}

void ivldp_lock_handler()
{
#ifdef VLDP_DEBUG
    LOGI << "DBG: VLDP REQ LOCK RECEIVED!!!";
#endif
    ivldp_ack_command();
    {
        VLDP_BOOL bLocked = VLDP_TRUE;

        // the user should unlock immediately after locking, so we need not
        // check for other commands
        while (bLocked == VLDP_TRUE) {
            SDL_Delay(1);
            if (ivldp_got_new_command()) {
                switch (g_req_cmdORcount & 0xF0) {
                case VLDP_REQ_UNLOCK:
#ifdef VLDP_DEBUG
                    LOGI << "DBG: VLDP REQ UNLOCK RECEIVED!!!";
#endif
                    ivldp_ack_command();
                    bLocked = VLDP_FALSE;
                    break;
                default:
                    LOGW << fmt("WARNING : lock handler received a command "
                                    "%x that wasn't to unlock it",
                            g_req_cmdORcount);
                    break;
                }
            }
        }
    }
}

// the handler we call if we're paused... called from video_out_sdl
// this function is called from within a while loop
void paused_handler()
{
    // the moment we render the still frame, we need to reset the FPS timer so
    // we don't try to catch-up
    if (g_out_info.status != STAT_PAUSED) {
        g_out_info.status = STAT_PAUSED;

        // reset these vars because otherwise draw_frame will loop
        // redundantly for no good reason
        s_timer = g_in_info->uMsTimer; // since we have just rendered the frame
                                       // we searched to, we refresh the timer
        s_uFramesShownSinceTimer = 1;  // this gives us a little breathing room

#ifdef VLDP_DEBUG
        LOGI << fmt(
            "paused_handler() : status set to STAT_PAUSED, s_timer set to %u",
            g_in_info->uMsTimer);
#endif
    }

    // if we have a new command coming in
    if (ivldp_got_new_command()) {
        // strip off the count and examine the command
        switch (g_req_cmdORcount & 0xF0) {
        case VLDP_REQ_PLAY:
            ivldp_respond_req_play();
            break;
        // if we get any of these commands, we have to skip the remaining
        // buffered frames and reset
        case VLDP_REQ_STOP:
        case VLDP_REQ_QUIT:
        case VLDP_REQ_OPEN:
        case VLDP_REQ_SEARCH:
            s_skip_all      = 1;
            s_uSkipAllCount = 0;
            // we don't acknowledge these requests here, we let the idle handler
            // take care of them
            break;
        case VLDP_REQ_STEP_FORWARD:
            // if they've also requested a step forward
            // NOTE : no need to change status, as we started paused and we'll
            // end paused
            ivldp_ack_command();
            s_step_forward = 1;
            break;
        case VLDP_REQ_LOCK:
            ivldp_lock_handler();
            break;
        default: // else if we get a pause command or another command we don't
                 // know how to handle, just ignore it
            LOGW << fmt("WARNING : pause handler received command %x that "
                            "it is ignoring",
                    g_req_cmdORcount);
            ivldp_ack_command(); // acknowledge the command
            break;
        } // end switch
    }     // end if we have a new command coming in
}

// the handler we call if we're playing
// this handler is called from within a while loop
void play_handler()
{
    // if we've received a new incoming command
    if (ivldp_got_new_command()) {
        // strip off count and examine command
        switch (g_req_cmdORcount & 0xF0) {
        case VLDP_REQ_NONE: // no incoming command
            break;
        case VLDP_REQ_PAUSE:
        case VLDP_REQ_STEP_FORWARD:
            ivldp_respond_req_pause_or_step();
            break;
        case VLDP_REQ_SPEEDCHANGE:
            ivldp_respond_req_speedchange();
            break;
        case VLDP_REQ_STOP:
        case VLDP_REQ_QUIT:
        case VLDP_REQ_OPEN:
        case VLDP_REQ_SEARCH:
        case VLDP_REQ_SKIP:
            s_skip_all = 1; // bail out, handle these requests in the idle
                            // handler
            s_uSkipAllCount = 0;
            break;
        case VLDP_REQ_LOCK:
            ivldp_lock_handler();
            break;
        default: // unknown or redundant command, just ignore
            ivldp_ack_command();
            LOGW << "WARNING : play handler received command which it "
                            "is ignoring";
            break;
        }
    } // end if we got a new command
}

// sets the framerate inside our info structure based upon the framerate code
// received
void ivldp_set_framerate(Uint8 frame_rate_code)
{
    // now to compute the framerate

    switch (frame_rate_code) {
    case 1:
        g_out_info.uFpks = 23976;
        break;
    case 2:
        g_out_info.uFpks = 24000;
        break;
    case 3:
        g_out_info.uFpks = 25000;
        break;
    case 4:
        g_out_info.uFpks = 29970;
        break;
    case 5:
        g_out_info.uFpks = 30000;
        break;
    case 6:
        g_out_info.uFpks = 50000;
        break;
    case 7:
        g_out_info.uFpks = 59940;
        break;
    case 8:
        g_out_info.uFpks = 60000;
        break;
    default:
        // else we got an invalid frame rate code
        LOGE << "ERROR : Invalid frame rate code!";
        g_out_info.uFpks = 1000; // to avoid divide by 0 error
        break;
    } // end switch

    // precalculate values that we use over and over again
    g_out_info.u2milDivFpks = 2000000 / g_out_info.uFpks;
}

// decode_mpeg2 function taken from mpeg2dec.c and optimized a bit
#ifdef __LIBRETRO__
// set while ivldp_render() feeds the decoder from the video file
static int s_in_render = 0;

// For the platform decoders (hardware MPEG-2, MP4): non-zero when they
// should stop putting out pictures because a command that ends this
// playback is waiting (the ones ivldp_render() stops for), or draw_planes()
// already skips everything for one.  They ask between pictures, and while
// they have no room for input, so a stuck decoder cannot hold up a command.
static int lr_decoder_abort()
{
    if (!s_in_render) return 0;
    if (s_skip_all) return 1;
    if (!ivldp_got_new_command()) return 0;
    switch (g_req_cmdORcount & 0xF0) {
    case VLDP_REQ_QUIT:
    case VLDP_REQ_OPEN:
    case VLDP_REQ_SEARCH:
    case VLDP_REQ_STOP:
    case VLDP_REQ_SKIP:
        return 1;
    }
    return 0;
}
#endif

#ifdef VLDP_MP4
static void idle_handler_open_mp4(const char *req_file);
#endif

#ifdef VLDP_HWDEC
// DAPHNE_LR_VERIFY_HWDEC=1: libmpeg2 decodes the same stream alongside the
// hardware decoder, and each picture the hardware path puts out is checked
// against libmpeg2's picture in the same place (development aid).
static int s_hw_verify = -1;
static std::deque<std::vector<uint8_t>> s_sw_sigs;
static unsigned long s_hw_checked, s_hw_differ, s_hw_unmatched;

// coarse luma signature: mean of a 16x9 grid of blocks
static std::vector<uint8_t> luma_signature(const uint8_t *y, int pitch, int w, int h)
{
    std::vector<uint8_t> s(16 * 9);
    for (int by = 0; by < 9; ++by)
        for (int bx = 0; bx < 16; ++bx) {
            unsigned long sum = 0, n = 0;
            for (int yy = by * h / 9; yy < (by + 1) * h / 9; yy += 2)
                for (int xx = bx * w / 16; xx < (bx + 1) * w / 16; xx += 2, ++n)
                    sum += y[(size_t)yy * pitch + xx];
            s[by * 16 + bx] = (uint8_t)(n ? sum / n : 0);
        }
    return s;
}

static int hw_verifying()
{
    if (s_hw_verify < 0) s_hw_verify = getenv("DAPHNE_LR_VERIFY_HWDEC") != NULL;
    return s_hw_verify;
}

// libmpeg2's side of the check: decode, keep signatures, show nothing
static void verify_feed_libmpeg2(uint8_t *current, uint8_t *end)
{
    const mpeg2_info_t *info = mpeg2_info(g_mpeg_data);
    mpeg2_buffer(g_mpeg_data, current, end);
    for (;;) {
        mpeg2_state_t state = mpeg2_parse(g_mpeg_data);
        if (state == STATE_BUFFER) break;
        if ((state == STATE_SLICE || state == STATE_END || state == STATE_INVALID_END) && info->display_fbuf)
            s_sw_sigs.push_back(luma_signature(info->display_fbuf->buf[0], info->sequence->width,
                                               info->sequence->picture_width, info->sequence->picture_height));
    }
}

static void hw_draw(const vldp_picture &pic)
{
    const uint8_t *y = pic.y;
    const int ypitch = pic.ypitch, placeholder = pic.placeholder;
    if (hw_verifying()) {
        if (s_sw_sigs.empty())
            ++s_hw_unmatched;
        else {
            if (!placeholder) {
                const std::vector<uint8_t> hw = luma_signature(y, ypitch, (int)g_out_info.w, (int)g_out_info.h);
                const std::vector<uint8_t> &sw = s_sw_sigs.front();
                unsigned long d = 0;
                for (size_t i = 0; i < hw.size(); ++i) d += (unsigned long)abs((int)hw[i] - (int)sw[i]);
                ++s_hw_checked;
                if (d > 3 * hw.size() && s_hw_differ++ < 20) {
                    LOGW << fmt("VLDP hardware decoder check: picture at frame %u differs from libmpeg2's "
                                "(mean %.1f)", g_out_info.current_frame, (double)d / hw.size());
                }
            }
            s_sw_sigs.pop_front();
        }
    }
    draw_planes(pic);
}

// DAPHNE_LR_VERIFY_HWDEC result so far (for the frontend's summary)
int vldp_lr_hwdec_check(unsigned long *checked, unsigned long *differ, unsigned long *unmatched)
{
    *checked = s_hw_checked;
    *differ = s_hw_differ;
    *unmatched = s_hw_unmatched;
    return hw_verifying();
}

// Logs the check's result for the file played, and starts counting afresh.
static void hw_verify_report()
{
    if (hw_verifying() && (s_hw_checked || s_hw_unmatched)) {
        LOGI << fmt("VLDP hardware decoder check: %lu pictures compared with libmpeg2, %lu differed, "
                    "%lu had no libmpeg2 picture", s_hw_checked, s_hw_differ, s_hw_unmatched);
    }
    s_hw_checked = s_hw_differ = s_hw_unmatched = 0;
    s_sw_sigs.clear();
}

// The hardware decoder gave up (and logged why): libmpeg2 decodes from the
// next picture it can, starting with the cached sequence header.
static void hw_fallback()
{
    mpeg2_reset(g_mpeg_data, 1);
    s_sw_sigs.clear();
    vldp_process_sequence_header();
}
#endif

// Resets whichever decoder the open file uses.
static void decoder_reset(int full)
{
#ifdef VLDP_HWDEC
    if (hwdec::active()) {
        hwdec::reset();
        if (hw_verifying()) {
            mpeg2_reset(g_mpeg_data, full);
            s_sw_sigs.clear();
        }
        return;
    }
#endif
    mpeg2_reset(g_mpeg_data, full);
}

static void decode_mpeg2(uint8_t *current, uint8_t *end)
{
    const mpeg2_info_t *info;
    mpeg2_state_t state;

#ifdef VLDP_HWDEC
    if (hwdec::active()) {
        if (hw_verifying()) verify_feed_libmpeg2(current, end);
        hwdec::decode(current, end);
        if (!hwdec::active()) hw_fallback();
        return;
    }
#endif

    mpeg2_buffer(g_mpeg_data, current, end);
    info = mpeg2_info(g_mpeg_data);

    // loop until we return (state is -1)
    while (1) {
        state = mpeg2_parse(g_mpeg_data);
        switch (state) {
        case STATE_BUFFER:
            return;
        case STATE_SEQUENCE:
            break;
        case STATE_SLICE:
        case STATE_END:
        case STATE_INVALID_END:
            /* draw current picture */
            /* might free frame buffer */
            if (info->display_fbuf) {
                draw_frame(info);
            }
#ifdef __LIBRETRO__
            // A search/skip/open/stop/quit arrived: the rest of this buffer
            // would only be decoded to be thrown away (the handlers reset
            // libmpeg2), while the game thread waits for our acknowledgement.
            if (s_in_render && s_skip_all) return;
#endif
            break;
        default:
            break;
        } // end switch
    }     // end endless for loop
}

/////////////////

// Pre-caches sequence header so that vldp_process_sequence_header (and thus any
// seeks) are faster
// NOTE: this does change the file position
void vldp_cache_sequence_header()
{
    uint32_t val       = 0;
    uint32_t index     = 0;

    io_seek(0);                             // start at beginning
    io_read(g_header_buf, HEADER_BUF_SIZE); // assume that we must find the
                                            // first frame in this chunk of
                                            // bytes
    // if not, we'll have to increase the number

    // go until we have found the first frame or we run out of data
    while (val != 0x000001B8) {
        val = val << 8;
        val |= g_header_buf[index]; // add newest byte to bottom of val
        index++;                    // advance the end pointer
        if (index > HEADER_BUF_SIZE) {
            LOGE << fmt("VLDP : Could not find first frame in 0x%x bytes.  "
                            "Modify source code to increase buffer!",
                    HEADER_BUF_SIZE);
            break;
        }
    }

    // subtract 4 because we stopped when we found the 4 byte header of the
    // first frame
    g_header_buf_size = index - 4;
}

// feeds libmpeg2 the beginning of the file up to the first Group of Picture
// It needs this info to get setup to play from an arbitrary position in the
// file that isn't the beginning
// In other words, this is ONLY used when we are seeking to an arbitrary frame
void vldp_process_sequence_header()
{
#ifdef VLDP_HWDEC
    // the hardware decoder gets it again with the next picture after a reset
    if (hwdec::active()) {
        if (hw_verifying()) verify_feed_libmpeg2(g_header_buf, g_header_buf + g_header_buf_size);
        return;
    }
#endif
    // decode the pre-cached sequence header
    decode_mpeg2(g_header_buf, g_header_buf + g_header_buf_size);
}

// opens a new mpeg2 file
// The file is positioned at the beginning
void idle_handler_open()
{
    int sAspect = 0;
    int dCurAspectRatio = 0;
    char req_file[STRSIZE] = {0};
    uint32_t req_idx  = g_req_idx;
    VLDP_BOOL req_precache = g_req_precache;
    VLDP_BOOL bSuccess     = VLDP_FALSE;

    // after we ack the command, this string could become clobbered at any time
    SAFE_STRCPY(req_file, g_req_file, sizeof(req_file));

    // NOTE : it is very important that we change our status to BUSY before
    // acknowledging the command, because our previous status could be
    // STAT_ERROR, which causes problems with the *_and_block commands.
    g_out_info.status = STAT_BUSY; // make us busy while opening the file
    ivldp_ack_command();           // acknowledge open command

#ifdef VLDP_HWDEC
    hw_verify_report();
    hwdec::close();
#endif
#ifdef VLDP_MP4
    if (mp4::active()) {
        mp4::close();
        g_in_info->render_blank_frame();
        g_in_info->render_blank_frame();
    }
#endif
    // reset libmpeg2 so it is prepared to begin reading from a new m2v file
    mpeg2_reset(g_mpeg_data, 1);
#ifdef __LIBRETRO__
    s_lr_pic = -1;
    g_lr_vldp_shown_pic = g_lr_presented_pic = -1;
#endif

    // if we have previously opened an mpeg, we need to close it and reset
    if (io_is_open()) {
        io_close();

        // since the overlay is double buffered, we want to blank it twice
        // before closing it.  This is to avoid a 'flicker effect' that we can
        // get when switching between mpeg files.
        g_in_info->render_blank_frame();
        g_in_info->render_blank_frame();
    }

#ifdef VLDP_MP4
    if (!req_precache && mp4::handles(req_file)) {
        idle_handler_open_mp4(req_file);
        return;
    }
#elif defined(__LIBRETRO__)
    // no MP4 decoder on this platform (Linux): say so, rather than libmpeg2's
    // complaint about a missing MPEG header
    const size_t req_len = strlen(req_file);
    if (!req_precache && req_len > 4 && strcasecmp(req_file + req_len - 4, ".mp4") == 0) {
        g_lr_vldp_error = "MP4 video (Singe 2) is not supported on this platform";
        LOGE << g_lr_vldp_error.load();
        g_out_info.status = STAT_ERROR;
        return;
    }
#endif

    // if we've been requested to open a real file ...
    if (!req_precache) {
        bSuccess = io_open(req_file);
    }
    // else we've been requested to open a precached file...
    else {
        bSuccess = io_open_precached(req_idx);
    }

    // If file was opened successfully, check to make sure it's a video stream
    // and also get framerate
    if (bSuccess) {
        Uint8 small_buf[8];
        io_read(small_buf, sizeof(small_buf)); // 1st 8 bytes reveal much

        // if we find the proper mpeg2 video header at the beginning of the file
        if (((small_buf[0] << 24) | (small_buf[1] << 16) | (small_buf[2] << 8) |
            small_buf[3]) == 0x000001B3)
        {
            g_out_info.w = (small_buf[4] << 4) | (small_buf[5] >> 4);   // get mpeg width
            g_out_info.h = ((small_buf[5] & 0x0F) << 8) | small_buf[6]; // get mpeg height

            if (get_mpeg_rate() < 0)
                ivldp_set_framerate(small_buf[7] & 0xF); // set the framerate from header
            else
                g_out_info.uFpks = get_mpeg_rate(); // force a non-standard rate

            // Look for aspect ratio meta field - thanks to walknight
            sAspect = small_buf[7] >> 4;

            switch (sAspect) {
            case 2:
               dCurAspectRatio = ASPECTSD;
               video::set_aspect_change((g_out_info.h * 4) / 3, g_out_info.h);
               break;
            case 3:
               dCurAspectRatio = ASPECTWS;
               video::set_aspect_change((g_out_info.h * 16) / 9, g_out_info.h);
               break;
            default:
               dCurAspectRatio = (int)(((double)g_out_info.w / g_out_info.h) * 100);
               break;
            }

            // Send media info to video::
            video::set_aspect_ratio(dCurAspectRatio);
            video::set_detected_height((int)g_out_info.h);
            video::set_detected_width((int)g_out_info.w);

            io_seek(0); // go back to beginning for parser's benefit

            // load/parse all the frame locations in the file for super fast
            // seeking
            if (ivldp_get_mpeg_frame_offsets(req_file)) {
                g_in_info->report_mpeg_dimensions(g_out_info.w, g_out_info.h); // this function creates the video overlay.
                // We want to make sure we do this _after_ the frame offsets are
                // loaded in because graphics are drawn to the main screen if
                // parsing needs to be done.

                vldp_cache_sequence_header(); // cache sequence header for
                                              // faster seeking
#ifdef VLDP_HWDEC
                // the hardware decoder, when chosen and the stream suits it:
                // look at the first megabyte to decide
                if (g_lr_vldp_hwdec && !g_out_info.uses_fields) {
                    const unsigned head_size = 1024 * 1024;
                    uint8_t *head = (uint8_t *)malloc(head_size);
                    if (head) {
                        io_seek(0);
                        const unsigned got = io_read(head, head_size);
                        hwdec::open(head, got, g_header_buf_size, (int)g_out_info.w, (int)g_out_info.h,
                                    hw_draw, lr_decoder_abort, req_file);
                        free(head);
                    }
                }
#endif

                io_seek(0); // seek back to beginning of file

                g_out_info.status = STAT_STOPPED; // now that the file is open,
                                                  // we're ready to play
            } else {
                io_close();
                LOGE << "VLDP PARSE ERROR : Is the video stream damaged?";
                g_out_info.status = STAT_ERROR; // change from BUSY to ERROR
            }
        } // end if a proper mpeg header was found

        // if the file had a bad header
        else {
            io_close();
            LOGE << "VLDP ERROR : Did not find expected header.  Is "
                            "this mpeg stream demultiplexed??";
            g_out_info.status = STAT_ERROR;
        }
    } // end if file exists
    else {
        LOGE << "VLDP ERROR : Could not open file!";
        g_out_info.status = STAT_ERROR;
    }
#ifdef VLDP_DEBUG
    LOGI << "idle_handler_open returning ...";
#endif
}

#ifdef VLDP_MP4
// Opens an MP4 video (Singe 2): the platform decoder replaces libmpeg2 and
// the frame index (idle_handler_open has acknowledged the command).
static void idle_handler_open_mp4(const char *req_file)
{
    mp4::stream_info info;
    if (!mp4::open(req_file, draw_planes, lr_decoder_abort, &info)) {
        g_out_info.status = STAT_ERROR;
        return;
    }
    // mp4::open() checked the size and frame rate are not zero
    g_out_info.w = (unsigned)info.w;
    g_out_info.h = (unsigned)info.h;
    g_out_info.uFpks = get_mpeg_rate() > 0 ? (uint32_t)get_mpeg_rate() : info.fpks;
    g_out_info.u2milDivFpks = 2000000 / g_out_info.uFpks;
    g_out_info.uses_fields = 0;
    g_totalframes = info.frames;

    // the shape of the picture on screen, for pixels that need not be square
    video::set_aspect_ratio((int)(((double)info.w * info.sar_num / ((double)info.h * info.sar_den)) * 100));
    video::set_detected_height(info.h);
    video::set_detected_width(info.w);
    g_in_info->report_mpeg_dimensions(info.w, info.h);

    g_out_info.status = STAT_STOPPED;
}
#endif

// gets called when the user wants to precache a file ...
void idle_handler_precache()
{
    char req_file[STRSIZE] = {0};

    SAFE_STRCPY(req_file, g_req_file,
                sizeof(req_file)); // after we ack the command, this string
                                   // could become clobbered at any time

    // always set the status before acknowledging the command so previous status
    // doesn't get through
    g_out_info.status = STAT_BUSY; // make us busy while opening the file
    ivldp_ack_command();

    // if we still have room in our array to precache ...
    if (s_uPreCacheIdxCount < MAX_PRECACHE_FILES) {
        // GET LENGTH OF ACTUAL FILE
        FILE *F = fopen(req_file, "rb");
        if (F) {
            struct stat filestats;
            fstat(fileno(F), &filestats); // get stats for file to get file
                                          // length
            s_sPreCacheEntries[s_uPreCacheIdxCount].uLength = filestats.st_size;
            s_sPreCacheEntries[s_uPreCacheIdxCount].uPos    = 0; // start at the
                                                                 // beginning

            // allocate RAM to hold file ...
            s_sPreCacheEntries[s_uPreCacheIdxCount].ptrBuf = malloc(filestats.st_size);

            // if malloc succeeded
            if (s_sPreCacheEntries[s_uPreCacheIdxCount].ptrBuf) {
                unsigned char *u8Ptr =
                    (unsigned char *)s_sPreCacheEntries[s_uPreCacheIdxCount].ptrBuf;
                unsigned int uTotalBytesRead = 0;
                const unsigned int READ_SIZE = 1048576; // how many bytes to
                                                        // read in at a time

                g_in_info->report_parse_progress(-1); // notify other thread
                                                      // that we're starting

                // load in the file ...
                for (;;) {
                    unsigned int uBytesRead   = 0;
                    unsigned int uBytesToRead = READ_SIZE;
                    unsigned int uBytesLeft   = filestats.st_size - uTotalBytesRead;

                    // don't overflow
                    if (uBytesToRead > uBytesLeft) uBytesToRead = uBytesLeft;

                    uBytesRead = (unsigned int)fread(u8Ptr + uTotalBytesRead, 1,
                                                     uBytesToRead, F);
                    uTotalBytesRead += uBytesRead;

                    // if we're done ...
                    if (uTotalBytesRead >= (unsigned int)filestats.st_size) {
                        break;
                    }

                    // update user on our precache progress
                    g_in_info->report_parse_progress(
                        (double)uTotalBytesRead /
                        s_sPreCacheEntries[s_uPreCacheIdxCount].uLength);
                }

                g_in_info->report_parse_progress(1); // notify other thread that
                                                     // we're done ...

                // notify other thread of which index we've used to precache
                // this file
                g_out_info.uLastCachedIndex = s_uPreCacheIdxCount;

                // we're done with this entry, so the count increases
                // (this must be done after we've read in the file so that the
                // index is correct for that operation)
                ++s_uPreCacheIdxCount;

                g_out_info.status = STAT_STOPPED; // success
            }
            // else malloc failed
            else {
                g_out_info.status = STAT_ERROR;
            }
            fclose(F);
        }
        // else we couldn't open the file
        else {
            g_out_info.status = STAT_ERROR;
        }
    }
    // else we're out of room, so return an error
    else {
        g_out_info.status = STAT_ERROR;
    }
}

// starts playing the mpeg from the very beginning
// This is ONLY called when a file has just been opened and no seeking has taken
// place, OR if ivldp_render() has hit EOF and rewound back to the beginning
// This ASSUMES that g_mpeg_handle is at the BEGINNING of the file and that
// mpeg2_reset() has been called
void idle_handler_play()
{
    ivldp_respond_req_play();
    // when the frame is actually blitted is when we set the status to
    // STAT_PLAYING
    ivldp_render();
}

// responds to play request
void ivldp_respond_req_play()
{
    s_timer = g_req_timer;
#ifdef VLDP_DEBUG
    LOGI << fmt("ivldp_respond_req_play() : g_req_timer is %u, and "
                    "uMstimer is %u",
            g_req_timer, g_in_info->uMsTimer);   // REMOVE ME
#endif                                           // VLDP_DEBUG
    s_uFramesShownSinceTimer = PLAY_FRAME_STALL; // we want to render the
                                                 // currently shown frame for 1
                                                 // frame before moving on
    g_out_info.status = STAT_PLAYING; // we strive for instant response (and
                                      // catch-up to maintain timing)
    ivldp_ack_command();              // acknowledge the play command
    s_paused  = 0;                    // we to not want to pause on 1 frame
    s_blanked = 0;                    // we want to see the video
    // skip no frames, just play from current position
    // this value is reset again as soon as we confirm that we are playing
    s_frames_to_skip = s_frames_to_skip_with_inc = 0;
}

// gets called if ivldp_got_new_command() returns true and the new command
// is either a pause or a step
void ivldp_respond_req_pause_or_step()
{
    // if they've also requested a step forward
    if ((g_req_cmdORcount & 0xF0) == VLDP_REQ_STEP_FORWARD) {
        s_step_forward = 1;
    }
    // NOTE : by design, our status should not change until paused_handler is
    // called, so we leave it at PLAYING for now
    ivldp_ack_command();
#ifdef VLDP_DEBUG
    LOGI << fmt("VLDP_REQ_PAUSED received when frame is %u, uMsTimer is %u",
           g_out_info.current_frame,
           g_in_info->uMsTimer); // DBG REMOVE ME!@
#endif                           // VLDP_DEBUG
    s_paused  = 1;
    s_blanked = 0;
}

// gets called if ivldp_got_new_command() returns true and the new command
//  is a speed change command
void ivldp_respond_req_speedchange()
{
    s_skip_per_frame  = g_req_skip_per_frame;
    s_stall_per_frame = g_req_stall_per_frame;
    ivldp_ack_command();
}

// displays 1 or more frames to the screen, according to the state variables.
// This function can be used to do both still frames and moving video.  Play and
// search both use this function.
void ivldp_render()
{
    Uint8 *end          = NULL;
    int render_finished = 0;

#ifdef VLDP_BENCHMARK
    Uint32 render_start_time  = SDL_GetTicks(); // keep track of when we started
    Sint32 render_start_frame = g_out_info.current_frame; // keep track of the
                                                          // frame we started
                                                          // timing stuff on
    double total_seconds = 0.0; // used to calculate FPS
    Sint32 total_frames  = 0;   // used to calculate FPS
    FILE *F              = fopen("benchmark.txt", "wt"); // create a little logfile for
                                                         // benchmark results
#endif

    s_skip_all = 0; // default value, don't skip frames unless the play or
                    // paused handler orders it
#ifdef __LIBRETRO__
    g_lr_vldp_next_due = 0;
#endif

#ifdef VLDP_DEBUG
    LOGI << fmt("s_skip_all skipped %u frames.", s_uSkipAllCount);
#endif // VLDP_DEBUG

#ifdef VLDP_MP4
    if (mp4::active()) {
        while (!render_finished) {
            s_in_render = 1;
            const bool more = mp4::decode_some();
            s_in_render = 0;
            if (!more) {
                // the end of the video: pause on the last picture, as at the
                // end of an MPEG-2 file
                g_out_info.status = STAT_STOPPED;
                render_finished = 1;
                mp4::seek(0);
                s_lr_pic = -1;
                g_out_info.current_frame = 0;
            }
            if (ivldp_got_new_command()) {
                switch (g_req_cmdORcount & 0xF0) {
                case VLDP_REQ_QUIT:
                case VLDP_REQ_OPEN:
                case VLDP_REQ_SEARCH:
                case VLDP_REQ_STOP:
                    g_out_info.status = STAT_BUSY;
                    render_finished   = 1;
                    break;
                case VLDP_REQ_SKIP:
                    render_finished = 1;
                    break;
                }
            }
        }
        return;
    }
#endif

    // check to make sure a file has been opened
    if (!io_is_open()) {
        render_finished = 1;
        LOGE << "VLDP RENDER ERROR : we tried to render an mpeg but "
                        "none was open!";
        g_out_info.status = STAT_ERROR;
    }

    // while we're not finished playing and pausing
    while (!render_finished) {
        // end = g_buffer + fread (g_buffer, 1, BUFFER_SIZE, g_mpeg_handle);
        end = g_buffer + io_read(g_buffer, BUFFER_SIZE);

        // safety check, they could be equal if we were already at EOF before we
        // tried this
        if (g_buffer != end) {
            // read chunk of video stream
#ifdef __LIBRETRO__
            s_in_render = 1;
            decode_mpeg2(g_buffer, end); // display it to the screen
            s_in_render = 0;
#else
            decode_mpeg2(g_buffer, end); // display it to the screen
#endif
        }

        // if we've read to the end of the mpeg2 file, then we can't play
        // anymore, so we pause on last frame
        if (end != (g_buffer + BUFFER_SIZE)) {
            g_out_info.status = STAT_STOPPED; // it's a toss-up between this and
                                              // STAT_PAUSED
            render_finished = 1;

#ifdef VLDP_HWDEC
            // the pictures still inside the hardware decoder
            if (hwdec::active()) {
                s_in_render = 1;
                hwdec::drain();
                s_in_render = 0;
            }
#endif
            // reset libmpeg2 so it is prepared to begin reading from the
            // beginning of the file
            decoder_reset(1);
#ifdef __LIBRETRO__
            s_lr_pic = -1;
#endif
            io_seek(0);                   // seek to the beginning of the file
            g_out_info.current_frame = 0; // set frame # to beginning of file
                                          // where it belongs
        }

        // if a new command is coming in, check to see if we need to stop
        if (ivldp_got_new_command()) {
            // check to see if we need to suddenly abort the rendering process
            switch (g_req_cmdORcount & 0xF0) {
            case VLDP_REQ_QUIT:
            case VLDP_REQ_OPEN:
            case VLDP_REQ_SEARCH:
            case VLDP_REQ_STOP:
                g_out_info.status = STAT_BUSY;
                render_finished   = 1;
                break;
            case VLDP_REQ_SKIP:
                // do not change the playing status because skips are supposed
                // to be instant
                render_finished = 1;
                break;
            } // end switch
        }     // end if they got a new command
    }         // end while

#ifdef VLDP_BENCHMARK
    fprintf(F, "Benchmarking result:\n");
    total_frames  = g_out_info.current_frame - render_start_frame;
    total_seconds = (SDL_GetTicks() - render_start_time) / 1000.0;
    fprintf(F, "VLDP displayed %u frames (%d to %d) in %f seconds (%f FPS)\n",
            total_frames, render_start_frame, g_out_info.current_frame,
            total_seconds, total_frames / total_seconds);
    fclose(F);
#endif
}

#ifdef VLDP_DEBUG
#ifdef UNIX
#include <signal.h> // to break into debugger
#endif              // UNIX
#endif              // VLDP_DEBUG

// searches to any arbitrary frame, be it I, P, or B, and renders it
// if skip is set, it will do a laserdisc skip instead of a search (ie it will
// go a frame, resume playback,
// and not adjust any timers)
void idle_handler_search(int skip)
{
    uint64_t proposed_pos = 0;
    Uint32 req_frame    = g_req_frame; // after we acknowledge the command,
                                       // g_req_frame could become clobbered
    Uint32 min_seek_ms = g_req_min_seek_ms; // g_req_min_seek_ms can be
                                            // clobbered at any time after we
                                            // acknowledge command

    // adjusted req frame is the requested frame with fields taken into account
    uint32_t uAdjustedReqFrame = 0;

    uint32_t actual_frame      = 0;
    int skipped_I              = 0;

    // status must be changed before acknowledging command, because previous
    // status could be STAT_ERROR, which causes problems with *_and_block vldp
    // API commands.
    if (!skip) g_out_info.status = STAT_BUSY;
    // else we're skipping
    // (our status is already STAT_PLAYING so we don't need to set it)
    else {
        /* Since we're skipping, we must re-calculate s_uFramesShownSinceTimer
         * because it is possible on laggy systems for this value to be behind
         * what it ought to be, and we need to be perfectly in sync with the
         * parent thread's g_in_info->uMsTimer value in order to display the
         * correct frames.
         * NOTE: this _must_ happen before we acknowledge the command, because
         *       we need g_in_info->uMsTimer to be constant while we
         *       recalculate s_uFramesShownSinceTimer
         * NOTE: we must use Uint64 here because otherwise this will overflow
         *       sometime after 2 minutes which DOES happen on Astron Belt if
         *       'infinite timer' is enabled (it doesn't do any searches, just
         *       skips)
         */
        uint32_t uNewFramesShownSinceTimer =
            (uint32_t)((((Uint64)(g_in_info->uMsTimer - s_timer)) * g_out_info.uFpks) / 1000000);

        // take into account the extra frame we did when playback started
        uNewFramesShownSinceTimer += PLAY_FRAME_STALL;

#ifdef VLDP_DEBUG
        // break into debugger if this happens so we can examine what's going on
        if (uNewFramesShownSinceTimer != s_uFramesShownSinceTimer) {
            LOGW << fmt("skip timing off: uNewFramesShownSinceTimer is %u, "
                   "s_uFramesShownSinceTimer is %u",
                   uNewFramesShownSinceTimer, s_uFramesShownSinceTimer);

            // this shouldn't ever happen
            if (s_uFramesShownSinceTimer > uNewFramesShownSinceTimer) {
                LOGE << "!!! s_uFramesShownSinceTimer > "
                                "uNewFramesShownSinceTimer, this shouldn't "
                                "happen!";
#ifdef UNIX
                raise(SIGTRAP); // we wanna know what's going on ...
#endif                          // UNIX
            }
        }
#endif // VLDP_DEBUG

        s_uFramesShownSinceTimer = uNewFramesShownSinceTimer;
    }

    ivldp_ack_command(); // acknowledge search/skip command

#ifdef __LIBRETRO__
    s_lr_search_t0_ns  = SDL_GetTicksNS();
    s_lr_search_timer0 = g_in_info->uMsTimer;
    s_lr_search_state  = 1;
#endif

#ifdef VLDP_MP4
    // MP4 video: the platform decoder seeks by time, without libmpeg2 or the
    // frame index (below)
    const bool mp4_video = mp4::active();
    if (!mp4_video) {
#endif
    // reset libmpeg2 so it is prepared to start from a new spot
    decoder_reset(0);

    vldp_process_sequence_header(); // we need to process the sequence header
                                    // before we can jump around the file for
                                    // frames
#ifdef VLDP_MP4
    }
#endif

    // if we're doing a search...
    if (!skip) {
        s_paused = 1; // we do want to pause on the frame we search to
        s_timer  = g_in_info->uMsTimer; // reset timer so framerate is correct
        /* NOTE: resetting s_timer here doesn't do much if we aren't simulating
         *       artificial seek delay, because paused_handler() will reset it
         *       again anyway. (but it is important for seek delay!)
         */

        s_uFramesShownSinceTimer = 0;
        /* NOTE: by setting this to 0, we are eliminating any potential
         *       unnecessary delays before paused_handler() gets called, which
         *       is what we want.
         */

        s_extra_delay_ms = min_seek_ms; // any extra delay we have will be when
                                        // the parent thread requested (this can
                                        // be 0)

        // if we are to blank during searches ...
        if (g_in_info->blank_during_searches) {
            g_in_info->render_blank_frame();
        }
    }

    // if we are skipping we are actually in the middle of playback so we don't
    // reset the timer
    else {
        /* NOTE: we don't want to change the status here because skipping is
         *       supposed to be instantaneous
         */

        s_paused = 0;

        // if we need to blank frame for skipping
        if (g_in_info->blank_during_skips) {
            g_in_info->render_blank_frame();
        }
    }

#ifdef VLDP_MP4
    if (mp4_video) {
        if (req_frame < g_totalframes) {
            mp4::seek(req_frame);
            s_frames_to_skip = s_frames_to_skip_with_inc = 0;
            s_lr_pic = (int)req_frame - 1;
            if (!skip) {
                g_out_info.current_frame = req_frame;
                s_uPendingSkipFrame = 0;
            } else
                s_uPendingSkipFrame = req_frame;
            s_blanked = 0;
            ivldp_render();
        } else {
            LOGE << fmt("SEARCH ERROR : frame %u was requested, but it is out of bounds", req_frame);
            g_out_info.status = STAT_ERROR;
        }
        return;
    }
#endif

    // adjusted req frame is the requested frame with fields taken into account
    uAdjustedReqFrame = req_frame;

    // if we're using fields, then the requested frame must be doubled (2 fields
    // per frame)
    if (g_out_info.uses_fields) uAdjustedReqFrame <<= 1;

    actual_frame = uAdjustedReqFrame;

    // do a bounds check
    if (uAdjustedReqFrame < g_totalframes) {
        proposed_pos = g_frame_position[uAdjustedReqFrame]; // get the proposed
                                                            // position

#ifdef VLDP_DEBUG
        LOGI << fmt("Initial proposed position is : %ld", proposed_pos);
#endif

        s_frames_to_skip = s_frames_to_skip_with_inc =
            0; // the below problem is no longer a problem

        // loop until we find which position in the file to seek to
        for (;;) {
            // if the frame we want is not an I frame, go backward until we find
            // an I frame, and increase # of frames to skip forward
            while ((proposed_pos == 0xFFFFFFFFFFFFFFFF) && (actual_frame > 0)) {
                s_frames_to_skip++;
                actual_frame--;
                proposed_pos = g_frame_position[actual_frame];
            }
            skipped_I++;

            // if we are only 2 frames away from an I frame, we will get a
            // corrupted image and need to go back to
            // the I frame before this one
            if ((skipped_I < 2) && (s_frames_to_skip < 3) && (actual_frame > 0)) {
                proposed_pos = 0xFFFFFFFFFFFFFFFF;
            } else {
                break;
            }
        }

#ifdef VLDP_DEBUG
        LOGI << fmt("frames_to_skip is %d, skipped_I is %d", s_frames_to_skip, skipped_I);
        LOGI << fmt("position in mpeg2 stream we are seeking to : %ld", proposed_pos);
#endif

        io_seek(proposed_pos);
        // go to the place in the stream where the I frame begins
        // fseek(g_mpeg_handle, proposed_pos, SEEK_SET);

#ifdef __LIBRETRO__
        s_lr_pic = (int)actual_frame - 1;
        if (g_out_info.uses_fields) s_lr_pic = -1; // not tracked for field streams
#endif
        // if we're seeking, we can change the frame right now ...
        if (!skip) {
            g_out_info.current_frame =
                req_frame; // this is no longer incremented in draw_frame
                           // due to s_paused being set
            s_uPendingSkipFrame = 0;
        }
        // if we're skipping, we have to leave the current frame alone until it
        // changes, in order to be consistent with actual laserdisc behavior.
        else {
            s_uPendingSkipFrame = req_frame;
        }

        s_blanked = 0; // we want to see the frame

        ivldp_render();
    } // end if the bounds check passed
    else {
        LOGE << fmt("SEARCH ERROR : frame %u was requested, but it is out "
                        "of bounds",
                req_frame);
        g_out_info.status = STAT_ERROR;
    }
}

#ifdef __LIBRETRO__
// Directory for .dat files the core generates itself (set by the frontend).
static char g_lr_cache_dir[1024] = "";

void vldp_lr_set_cache_dir(const char *dir)
{
    SAFE_STRCPY(g_lr_cache_dir, dir ? dir : "", sizeof(g_lr_cache_dir));
}

// Reads a finished frame index for an mpeg of 'mpeg_size' bytes.  Accepts this
// engine's own format (v3: 16-byte header, 64-bit offsets) and Daphne 1.0's
// (v2: 8-byte header, 32-bit offsets), so existing Daphne installs work as-is.
static VLDP_BOOL lr_read_dat(const char *path, uint64_t mpeg_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return VLDP_FALSE;

    uint8_t hdr[16];
    VLDP_BOOL ok = VLDP_FALSE;
    size_t got = fread(hdr, 1, sizeof(hdr), f);
    int width = 0;

    if (got >= 8 && hdr[0] == 2 && hdr[1] == 1) {
        uint32_t len = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
        if (len == (uint32_t)mpeg_size) width = 4;
    } else if (got == 16 && hdr[0] == DAT_VERSION && hdr[1] == 1) {
        uint64_t len = 0;
        for (int i = 7; i >= 0; --i) len = (len << 8) | hdr[8 + i];
        if (len == mpeg_size) width = 8;
    }

    if (width) {
        fseek(f, width == 4 ? 8 : 16, SEEK_SET);
        g_out_info.uses_fields = hdr[2];
        g_totalframes = 0;

        uint8_t b[8];
        while (g_totalframes < MAX_LDP_FRAMES && fread(b, width, 1, f) == 1) {
            uint64_t v = 0;
            for (int i = width - 1; i >= 0; --i) v = (v << 8) | b[i];
            if (width == 4 && v == 0xFFFFFFFFu) v = 0xFFFFFFFFFFFFFFFFull;
            g_frame_position[g_totalframes++] = v;
        }
        ok = VLDP_TRUE;
        LOGI << fmt("Using %s frame index %s (%u frames)", width == 4 ? "Daphne" : "VLDP",
                    path, g_totalframes);
    }

    fclose(f);
    return ok;
}

VLDP_BOOL ivldp_get_mpeg_frame_offsets(char *mpeg_name)
{
    const uint64_t mpeg_size = io_length();
    char datafilename[1024];

    // 1. the index that ships next to the video (never modified)
    SAFE_STRCPY(datafilename, mpeg_name, sizeof(datafilename));
    strcpy(&datafilename[strlen(datafilename) - 3], "dat");
    if (lr_read_dat(datafilename, mpeg_size)) return VLDP_TRUE;

    // 2. our own cache, keyed by file name and size
    const char *base = strrchr(mpeg_name, '/');
    const char *base2 = strrchr(mpeg_name, '\\');
    if (!base || (base2 && base2 > base)) base = base2;
    base = base ? base + 1 : mpeg_name;

    char stem[256];
    SAFE_STRCPY(stem, base, sizeof(stem));
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';

    if (!g_lr_cache_dir[0]) {
        LOGE << fmt("No usable frame index for %s and no cache directory", mpeg_name);
        return VLDP_FALSE;
    }
    snprintf(datafilename, sizeof(datafilename), "%s/%s-%llx.dat", g_lr_cache_dir, stem,
             (unsigned long long)mpeg_size);
    if (lr_read_dat(datafilename, mpeg_size)) return VLDP_TRUE;

    // 3. build the cache
    LOGI << fmt("Indexing %s (one time) into %s", mpeg_name, datafilename);
    if (!ivldp_parse_mpeg_frame_offsets(datafilename, mpeg_size)) return VLDP_FALSE;
    return lr_read_dat(datafilename, mpeg_size);
}
#else
// parses an mpeg video stream to get its frame offsets, or if the parsing had
// taken place earlier
VLDP_BOOL ivldp_get_mpeg_frame_offsets(char *mpeg_name)
{
    char datafilename[320]       = {0};
    VLDP_BOOL mpeg_datafile_good = VLDP_FALSE;
    FILE *data_file              = NULL;
    VLDP_BOOL result             = VLDP_TRUE;
    uint64_t mpeg_size           = 0;
    struct dat_header header;

    // GET LENGTH OF ACTUAL FILE
    mpeg_size = io_length();

    // change extension of file to be dat instead of (presumably) m2v
    SAFE_STRCPY(datafilename, mpeg_name, sizeof(datafilename));
    strcpy(&datafilename[strlen(mpeg_name) - 3], "dat");

    // loop until we get a good datafile or until we get an error
    while (!mpeg_datafile_good && result) {
        data_file = fopen(datafilename, "rb"); // check to see if datafile
                                               // exists

        // If file cannot be opened, try to create it.
        // Most likely the file cannot be opened because it doesn't exist.
        if (!data_file) {
            result = ivldp_parse_mpeg_frame_offsets(datafilename, mpeg_size);
            // we could open the file here, but there is no need to because we
            // will loop back through and open the file anyway
        }

        // else if the file has been opened
        else {
            // now that file exists and we have it open, we have to read it

            fseek(data_file, 0L, SEEK_SET);
            // read .DAT file header
            size_t sizeRead = fread(&header, sizeof(header), 1, data_file);

            // if version, file size, or finished are wrong, the dat file is no
            // good and has to be regenerated
            if ((sizeRead != 1) || (header.length != mpeg_size) ||
                (header.version != DAT_VERSION) || (header.finished != 1)) {
#ifdef VLDP_DEBUG
                LOGI << fmt("DAT version is %d", header.version);
#endif
                LOGW << fmt("MPEG data file %s is outdated and has "
                            "to be created again!", datafilename);
                fclose(data_file);
                data_file = NULL;

                // try to delete obsolete .DAT file so we can create a modern
                // one
                if (remove(datafilename) == -1) {
                    LOGE << "Couldn't delete obsolete .DAT file!";
                    result = VLDP_FALSE;
                }
            } else {
                g_out_info.uses_fields = header.uses_fields;
                mpeg_datafile_good     = VLDP_TRUE; // escape the loop
            }
        } // end if mpeg parsing was not necessary
    }     // end while we don't have a good datafile and haven't gotten an error

    // if we didn't exit the loop because of an error, then we need to read the
    // datafile
    if (result && data_file) {
        g_totalframes = 0;

#ifdef VLDP_DEBUG
        unlink(FRAMELOG);
#endif

        // read all the frame positions
        // if we don't read 8 bytes, it means we've hit the EOF and we're done
        while (fread(&g_frame_position[g_totalframes], 8, 1, data_file) == 1) {
#ifdef VLDP_DEBUG
            FILE *tmp_F = fopen(FRAMELOG, "ab");
            fprintf(tmp_F, "Frame %d has offset of %ld\n", g_totalframes,
                    g_frame_position[g_totalframes]);
            fclose(tmp_F);
#endif
            g_totalframes++;

            // safety check, it is possible to make mpegs with too many frames
            // to fit onto one CAV laserdisc
            // (in fact I did this, and it caused a lot of problems in the debug
            // stages hehe)
            if (g_totalframes >= MAX_LDP_FRAMES) {
                LOGE << fmt("ERROR : Current mpeg has a huge number of frames, "
                                "VLDP will ignore any frame above %u",
                                MAX_LDP_FRAMES);
                break;
            }
        }
#ifdef VLDP_DEBUG
        LOGI << fmt("*** g_totalframes is %u", g_totalframes);
        LOGI << fmt("And frame 0's offset is %ld", g_frame_position[0]);
#endif
    }

    // close any files that are still open
    if (data_file) {
        fclose(data_file);
    }

    return result;
}
#endif // __LIBRETRO__

VLDP_BOOL ivldp_parse_mpeg_frame_offsets(char *datafilename, uint64_t mpeg_size)
{
    VLDP_BOOL result = VLDP_TRUE;
    FILE *data_file  = fopen(datafilename, "wb"); // create file
    struct dat_header header; // header to put inside .DAT file

    // if we could create the file successfully, then we need to populate it
    if (data_file) {
        uint64_t pos     = 0; // position in the file
        int count        = 0;
        int parse_result = 0;

        header.version     = DAT_VERSION;
        header.finished    = 0;
        header.uses_fields = 0;
        header.length = mpeg_size;
        fwrite(&header, sizeof(header), 1, data_file);
        // first thing that goes in the file is the .DAT header
        // That way we can re-use the file another time with confidence that
        // it's the right one

        mpegscan::init();

        g_in_info->report_parse_progress(-1); // notify other thread that we're
                                              // starting

        // keep reading the file while there is a file left to be read
        do {
#define PARSE_CHUNK 200000

            parse_result = mpegscan::parse(data_file, PARSE_CHUNK);
            pos += PARSE_CHUNK;

            // we want to give the user updates but don't want to flood them
            if (count > 10) {
                count = 0;

                // report progress to parent thread
                g_in_info->report_parse_progress((double)pos / mpeg_size);
            }
            count++;

        } while (parse_result == mpegscan::IN_PROGRESS);

        g_in_info->report_parse_progress(1); // notify other thread that we're
                                             // done

        // if parse finished, then we have to update the header
        if (parse_result != mpegscan::ERROR) {
            header.finished    = 1;
            header.uses_fields = 0;
            if (parse_result == mpegscan::FINISHED_FIELDS) {
                header.uses_fields = 1;
            }
            fseek(data_file, 0L, SEEK_SET);
            fwrite(&header, sizeof(header), 1, data_file); // save changes
        }

        // we have to close data file because it's write-only
        // and it needs to be re-opened read-only
        fclose(data_file);
        data_file = NULL;

        // if the mpeg did not finish parsing gracefully, we've got problems
        /* NOTE: I separated this from the other if above to guarantee that the
         * file gets closed
         */
        if (parse_result == mpegscan::ERROR) {
            LOGE << "There was an error parsing the MPEG file.";
            LOGE << "Either there is a bug in the parser or the MPEG "
                            "file is corrupt.";
            LOGE << "OR the user aborted the decoding process :)";
            result = VLDP_FALSE;
            remove(datafilename);
        }
    } // end if we could create the file successfully

    // we couldn't create data file which means no write permission probably
    // this is probably a good time to shut VLDP down =]
    else {
        LOGE << fmt("Could not create file %s", datafilename);
        LOGE << "This probably means you don't have permission to "
                        "create the file";
        result = VLDP_FALSE;
    }

    return result;
}

VLDP_BOOL io_open(const char *cpszFilename)
{
    VLDP_BOOL bResult = VLDP_FALSE;

    // make sure everything is closed
    if ((!s_bPreCacheEnabled) && (!g_mpeg_handle)) {
        g_mpeg_handle = fopen(cpszFilename, "rb");
        if (g_mpeg_handle) bResult = VLDP_TRUE;
    }
    return bResult;
}

VLDP_BOOL io_open_precached(uint32_t uIdx)
{
    VLDP_BOOL bResult = VLDP_FALSE;

    // make sure everything is closed
    if ((!g_mpeg_handle) && (!s_bPreCacheEnabled)) {
        // make sure index is within range ...
        if (uIdx < s_uPreCacheIdxCount) {
            bResult                                    = VLDP_TRUE;
            s_uCurPreCacheIdx                          = uIdx;
            s_bPreCacheEnabled                         = VLDP_TRUE;
            s_sPreCacheEntries[s_uCurPreCacheIdx].uPos = 0; // rewind to
                                                            // beginning
        }
        // else out of range ...
    }
    return bResult;
}

unsigned int io_read(void *buf, unsigned int uBytesToRead)
{
    unsigned int uBytesRead = 0;

    // if we're reading from a file stream
    if (g_mpeg_handle) {
        uBytesRead = (unsigned int)fread(buf, 1, uBytesToRead, g_mpeg_handle);
    }
    // else we're reading from a precache stream
    else {
        struct precache_entry_s *entry = &s_sPreCacheEntries[s_uCurPreCacheIdx];
        unsigned int uBytesLeft        = entry->uLength - entry->uPos;

        // if we're trying to read beyond our means ...
        if (uBytesToRead > uBytesLeft) {
            uBytesToRead = uBytesLeft;
        }

        memcpy(buf, ((unsigned char *)entry->ptrBuf) + entry->uPos, uBytesToRead);
        uBytesRead = uBytesToRead;
        entry->uPos += uBytesRead;
    }

    return uBytesRead;
}

VLDP_BOOL io_seek(uint64_t uPos)
{
    VLDP_BOOL bResult = VLDP_FALSE;

    if (g_mpeg_handle) {
#if defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS == 64
        if (fseeko64(g_mpeg_handle, uPos, SEEK_SET) == 0) {
#elif defined(__WIN32)
        if (_fseeki64(g_mpeg_handle, uPos, SEEK_SET) == 0) {
#else
        if (fseek(g_mpeg_handle, uPos, SEEK_SET) == 0) {
#endif
            bResult = VLDP_TRUE;
        }
    } else {
        struct precache_entry_s *entry = &s_sPreCacheEntries[s_uCurPreCacheIdx];

        // if we're seeking within bounds ...
        if (uPos < entry->uLength) {
            entry->uPos = uPos;
            bResult     = VLDP_TRUE;
        }
    }
    return bResult;
}

void io_close()
{
    if (g_mpeg_handle) {
        fclose(g_mpeg_handle);
        g_mpeg_handle = NULL;
    } else if (s_bPreCacheEnabled) {
        s_bPreCacheEnabled = VLDP_FALSE;
    }
    // else nothing is open ...
}

VLDP_BOOL io_is_open()
{
    VLDP_BOOL bResult = VLDP_FALSE;
    if ((g_mpeg_handle) || (s_bPreCacheEnabled)) {
        bResult = VLDP_TRUE;
    }
    return bResult;
}

uint64_t io_length()
{
    uint64_t uResult = 0;

    if (g_mpeg_handle) {
        struct stat the_stat;
        fstat(fileno(g_mpeg_handle), &the_stat);
        uResult = the_stat.st_size;
    } else if (s_bPreCacheEnabled) {
        uResult = s_sPreCacheEntries[s_uCurPreCacheIdx].uLength;
    }

    return uResult;
}

void draw_frame(const mpeg2_info_t *info)
{
    vldp_picture pic = {info->display_fbuf->buf[0], info->display_fbuf->buf[1], info->display_fbuf->buf[2], NULL,
                        (int)info->sequence->width, (int)info->sequence->chroma_width, 0};
    draw_planes(pic);
}

// Shows (or skips) one decoded picture, keeping VLDP's timing.
void draw_planes(const vldp_picture &pic)
{
#ifdef __LIBRETRO__
    ++s_lr_pic;
#endif
    Sint32 correct_elapsed_ms = 0;
    Sint32 actual_elapsed_ms  = 0;
    unsigned int uStallFrames = 0;

    if (!(s_frames_to_skip | s_skip_all)) {
        do {
            VLDP_BOOL bFrameNotShownDueToCmd = VLDP_FALSE;

            Sint64 s64Ms = s_uFramesShownSinceTimer;
            s64Ms        = (s64Ms * 1000000) / g_out_info.uFpks;

            correct_elapsed_ms = (Sint32)(s64Ms) + s_extra_delay_ms;
            actual_elapsed_ms  = g_in_info->uMsTimer - s_timer;

            s_extra_delay_ms = 0;

            if (actual_elapsed_ms < (correct_elapsed_ms + (Sint32)g_out_info.u2milDivFpks)) {
                int bPrepared;
#ifdef __LIBRETRO__
                if (pic.placeholder)
                    bPrepared = 1; // nothing decoded: the surface keeps what it has
                else if (pic.uv)
                    bPrepared = g_in_info->prepare_frame_nv12(pic.y, pic.ypitch, pic.uv, pic.uvpitch);
                else
#endif
                bPrepared = g_in_info->prepare_frame(
                        (uint8_t *)pic.y, (uint8_t *)pic.u, (uint8_t *)pic.v,
                        pic.ypitch, pic.uvpitch, pic.uvpitch
                    );
#ifdef __LIBRETRO__
                if (bPrepared && !pic.placeholder && s_lr_search_state == 1) {
                    s_lr_search_ready = (float)((SDL_GetTicksNS() - s_lr_search_t0_ns) / 1e6);
                    s_lr_search_state = 2;
                }
#endif
                if (bPrepared) {
#ifndef VLDP_BENCHMARK
                    while (((Sint32)(g_in_info->uMsTimer - s_timer) < correct_elapsed_ms) &&
                           (!bFrameNotShownDueToCmd)) {
#ifdef __LIBRETRO__
                        // tell the frontend we are ahead of the emulated clock
                        g_lr_vldp_wait_timer = g_in_info->uMsTimer;
#endif
                        SDL_Delay(1);
                        if (ivldp_got_new_command()) {
                            switch (g_req_cmdORcount & 0xF0) {
                            case VLDP_REQ_PAUSE:
                            case VLDP_REQ_STEP_FORWARD:
                                ivldp_respond_req_pause_or_step();
                                break;
                            case VLDP_REQ_SPEEDCHANGE:
                                ivldp_respond_req_speedchange();
                                break;
                            case VLDP_REQ_NONE:
                                break;
                            default:
                                bFrameNotShownDueToCmd = VLDP_TRUE;
                                break;
                            }
                        }
                    }
#endif
#ifdef __LIBRETRO__
                    g_lr_vldp_wait_timer = LR_VLDP_NOT_WAITING;
#endif
                    if (!bFrameNotShownDueToCmd) {
#ifdef __LIBRETRO__
                        g_lr_vldp_shown_pic = s_lr_pic;
                        // a placeholder keeps the picture on screen: showing
                        // the back buffer would bring back an older one
                        if (!pic.placeholder)
#endif
                        g_in_info->display_frame();
#ifdef __LIBRETRO__
                        if (s_lr_search_state == 2) {
                            const unsigned i = s_lr_search_count % LR_SEARCH_SAMPLES;
                            s_lr_search_ready_ms[i] = s_lr_search_ready;
                            s_lr_search_shown_ms[i] = (float)(g_in_info->uMsTimer - s_lr_search_timer0);
                            ++s_lr_search_count;
                            s_lr_search_state = 0;
                        }
#endif
                    }
                }
            }

            if (!bFrameNotShownDueToCmd) {
                ++s_uFramesShownSinceTimer;
#ifdef __LIBRETRO__
                g_lr_vldp_next_due = s_timer + s_extra_delay_ms +
                    (uint32_t)(((Sint64)s_uFramesShownSinceTimer * 1000000) / g_out_info.uFpks);
#endif
            }

            if (s_paused) {
                paused_handler();
            } else {
                play_handler();

                if (!s_paused) {
                    if (uStallFrames == 0) {
                        if (s_uPendingSkipFrame == 0) {
                            if (!bFrameNotShownDueToCmd) {
                                ++g_out_info.current_frame;

                                if (s_stall_per_frame > 0) {
                                    uStallFrames = s_stall_per_frame;
                                }

                                if (s_skip_per_frame > 0) {
                                    s_frames_to_skip = s_frames_to_skip_with_inc =
                                        s_skip_per_frame;
                                }
                            }
                        } else {
                            g_out_info.current_frame = s_uPendingSkipFrame;
                            s_uPendingSkipFrame      = 0;
                        }
                    } else {
                        --uStallFrames;
                    }
                }
            }
        } while ((s_paused || uStallFrames > 0) && !s_skip_all && !s_step_forward);

        s_step_forward = 0;
    } else {
        if (s_frames_to_skip > 0) {
            --s_frames_to_skip;
            if (s_frames_to_skip_with_inc > 0) {
                --s_frames_to_skip_with_inc;
                ++g_out_info.current_frame;
            }
        }
    }
}
