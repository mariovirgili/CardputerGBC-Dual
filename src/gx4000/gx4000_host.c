
#include "arnold/host.h"
#include "gx4000_display.h"
#include "gx4000_input.h"
#include "gx4000_sound.h"

#include <string.h>
#include <stdio.h>

/* ── Colour format advertised to the Arnold render system ──────────────────
 * RGB 5-6-5 packed into uint16, host little-endian.
 * Arnold reads this in Render_SetDisplayFullScreen/Windowed to set up
 * ConvertedColourTable[].
 */
static GRAPHICS_BUFFER_COLOUR_FORMAT s_colourFmt = {
    .BPP   = 16,
    /* Red: 5 bits starting at bit 11 */
    .Red   = { .BPP = 5, .Mask = 0xF800, .Shift = 11 },
    /* Green: 6 bits starting at bit 5 */
    .Green = { .BPP = 6, .Mask = 0x07E0, .Shift =  5 },
    /* Blue: 5 bits starting at bit 0 */
    .Blue  = { .BPP = 5, .Mask = 0x001F, .Shift =  0 },
};

/* ── Graphics buffer info (single line) ────────────────────────────────────
 * pSurface points at the caller-allocated pScreenBase (via InitialiseRender).
 * We store the pointer provided by Host_SetGraphicsBufferSurface (see below).
 */
static GRAPHICS_BUFFER_INFO s_bufInfo = { 0 };
static unsigned long s_fpsWindowStartMs = 0;
static unsigned int s_fpsFrameCount = 0;

static void gx4000_perf_tick(void)
{
    unsigned long now = Host_GetCurrentTimeInMilliseconds();
    if (s_fpsWindowStartMs == 0) {
        s_fpsWindowStartMs = now;
    }

    s_fpsFrameCount++;
    if ((now - s_fpsWindowStartMs) >= 2000) {
        unsigned long elapsed = now - s_fpsWindowStartMs;
        unsigned long fps10 = (elapsed > 0)
            ? ((unsigned long)s_fpsFrameCount * 10000UL) / elapsed
            : 0;

        unsigned long audioDrop = gx4000_sound_take_drop_count();
        unsigned long displayDrop = gx4000_display_take_drop_count();
        EMU_LOG("[GX4-PERF] fps=%lu.%lu audio_drop=%lu display_drop=%lu\n",
                fps10 / 10UL, fps10 % 10UL,
                audioDrop, displayDrop);

        s_fpsWindowStartMs = now;
        s_fpsFrameCount = 0;
    }
}

void Host_SetGraphicsBufferSurface(unsigned char *pSurface,
                                    int width, int height, int pitch)
{
    s_bufInfo.pSurface = pSurface;
    s_bufInfo.Width    = width;
    s_bufInfo.Height   = height;
    s_bufInfo.Pitch    = pitch;
}

static SOUND_PLAYBACK_FORMAT s_soundFmt = {
    .NumberOfChannels = 1,
    .BitsPerSample    = 16,
    .Frequency        = 22050,
};

BOOL Host_SetDisplay(int Type, int Width, int Height, int Depth)
{
    (void)Type; (void)Width; (void)Height; (void)Depth;
    /* InitialiseRender() will allocate pScreenBase its own way */
    return TRUE;
}

GRAPHICS_BUFFER_COLOUR_FORMAT *Host_GetGraphicsBufferColourFormat(void)
{
    return &s_colourFmt;
}

BOOL Host_LockGraphicsBuffer(void)
{
    return TRUE;   /* single-threaded – always available */
}

GRAPHICS_BUFFER_INFO *Host_GetGraphicsBufferInfo(void)
{
    return &s_bufInfo;
}

void Host_UnlockGraphicsBuffer(void)
{
    /* no-op */
}

void Host_SwapGraphicsBuffers(void)
{
    gx4000_perf_tick();
    gx4000_display_frame_done();
}

void Host_SetPaletteEntry(int index,
                           unsigned char r,
                           unsigned char g,
                           unsigned char b)
{
    (void)index; (void)r; (void)g; (void)b;
    /* Only called in 8-bpp paletted mode, we run 16-bpp TrueColour. */
}

/* ── Audio ─────────────────────────────────────────────────────────────── */

BOOL Host_AudioPlaybackPossible(void)
{
    return TRUE;
}

SOUND_PLAYBACK_FORMAT *Host_GetSoundPlaybackFormat(void)
{
    return &s_soundFmt;
}

void Host_WriteDataToSoundBuffer(unsigned char *pData, unsigned long Length)
{
    gx4000_sound_push(pData, (unsigned int)Length);
}

BOOL Host_LockAudioBuffer(unsigned char **ppBuffer1, unsigned long *pLen1,
                           unsigned char **ppBuffer2, unsigned long *pLen2,
                           int RequestedLength)
{
    (void)ppBuffer1; (void)pLen1;
    (void)ppBuffer2; (void)pLen2;
    (void)RequestedLength;
    return FALSE;   /* use Host_WriteDataToSoundBuffer path instead */
}

void Host_UnLockAudioBuffer(void)
{
    /* no-op */
}

/* ── Input / system events ─────────────────────────────────────────────── */

BOOL Host_ProcessSystemEvents(void)
{
    static unsigned int s_pollDiv = 0;

    /* Poll full input state every 2 emulation frames.
       Keeps controls responsive while reducing per-frame overhead. */
    s_pollDiv++;
    if ((s_pollDiv & 1U) != 0U) {
        return FALSE;
    }

    /* Returns TRUE → exit emulation loop */
    return gx4000_input_poll() ? TRUE : FALSE;
}

/* ── Throttle ──────────────────────────────────────────────────────────── */

void Host_Throttle(void)
{
    /* No-op on this target: display/audio paths already pace the loop. */
}

/* ── Printer (GX4000 has none) ─────────────────────────────────────────── */

void Host_HandlePrinterOutput(void)
{
    /* no-op */
}

/* ── File access (used for snapshots – not implemented yet) ───────────── */

HOST_FILE_HANDLE Host_OpenFile(const char *Filename, int Access)
{
    (void)Filename; (void)Access;
    return (HOST_FILE_HANDLE)0;   /* invalid handle */
}

void Host_CloseFile(HOST_FILE_HANDLE handle)
{
    (void)handle;
}

int Host_GetFileSize(HOST_FILE_HANDLE handle)
{
    (void)handle;
    return 0;
}

void Host_WriteData(HOST_FILE_HANDLE handle,
                    unsigned char *pData, unsigned long Length)
{
    (void)handle; (void)pData; (void)Length;
}

void Host_ReadData(HOST_FILE_HANDLE handle,
                   unsigned char *pData, unsigned long Length)
{
    (void)handle; (void)pData; (void)Length;
}

unsigned long Host_GetCurrentTimeInMilliseconds(void)
{
    extern uint32_t millis(void);
    return (unsigned long)millis();
}
