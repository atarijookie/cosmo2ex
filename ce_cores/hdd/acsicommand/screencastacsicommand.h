#ifndef _SCREENCASTACSICOMMAND_H_
#define _SCREENCASTACSICOMMAND_H_

#include "../acsidatatrans.h"

#include <stddef.h>
#include <stdint.h>

// one TRAN_CMD_SENDSCREENCAST part - ST sends up to MAXSECTORS (254) sectors in one DMA transfer, so bigger screens come in parts
#define SCREENCAST_BUFFER_SIZE      (128 * 1024)

// latest frame, served to webserver by 'get_frame' on the command socket:
//  resolution (1) + palette (SCREENCAST_PALETTE_SIZE - room for 256 TT colors, ST uses the first 32 bytes) + screen data
// the screen data size depends on the resolution - see frameSize()
#define SCREENCAST_PALETTE_SIZE     512
#define SCREENCAST_SCREEN_OFFSET    (1 + SCREENCAST_PALETTE_SIZE)
#define SCREENCAST_SCREEN_SIZE_ST   32000
#define SCREENCAST_SCREEN_SIZE_TT   153600
#define SCREENCAST_FRAME_SIZE_MAX   (SCREENCAST_SCREEN_OFFSET + SCREENCAST_SCREEN_SIZE_TT)

// the frame is sent as UDP datagrams of: offset (4 bytes, big endian) + frame size (4 bytes, big endian) +
// frame sequence number (4 bytes, big endian - increments with each frame from ST, so web UI knows when a new one arrived) + data
// payload kept under 8 KB - macOS limits UDP datagrams to 9216 bytes by default
#define SCREENCAST_CHUNK_HEADER     12
#define SCREENCAST_CHUNK_PAYLOAD    8000

// SCREENCAST_FPS setting (set via web) - how many frames per second CE_DD.PRG sends, sent to it as VBL frame skip
#define SCREENCAST_FPS_MIN          1
#define SCREENCAST_FPS_MAX          25
#define SCREENCAST_FPS_DEFAULT      3

// screencast is on while the web UI keeps retrieving live frames - CE_DD.PRG checks this every 3 seconds
#define SCREENCAST_LIVE_TIMEOUT_MS  3000

// resolution type CE_DD.PRG sends in cmd[5] (byte 6 of its ICD command) and which is stored as frame byte 0 - the full list
// (TT and Falcon resolutions too) is in ce_dd_prg/screencast.h, the web UI decodes them in remote.html
#define SCREENCAST_RES_ST_LOW       0x00    // 320 x 200, 16 colors
#define SCREENCAST_RES_ST_MID       0x01    // 640 x 200, 4 colors
#define SCREENCAST_RES_ST_HIGH      0x02    // 640 x 400, 2 colors
#define SCREENCAST_RES_TT_LOW       0x10    // 320 x 480, 256 colors
#define SCREENCAST_RES_TT_MID       0x11    // 640 x 480, 16 colors
#define SCREENCAST_RES_TT_HIGH      0x12    // 1280 x 960, 2 colors

class ScreencastAcsiCommand
{
public:
  	ScreencastAcsiCommand(AcsiDataTrans *dt);
  	~ScreencastAcsiCommand();
  	void processCommand(uint8_t *command);

	static void frameInit(void);                    // clear the frame and fill it with logo
	static size_t screenSize(uint8_t res);          // size of screen data of this SCREENCAST_RES_* resolution
	static size_t frameSize(uint8_t res);           // size of the whole frame of this resolution
	static size_t frameGet(uint8_t* out, uint32_t* seq);   // copy of the whole frame to out (SCREENCAST_FRAME_SIZE_MAX bytes) and its sequence number, taken under lock; returns its size
	static size_t frameChunk(const uint8_t* frame, size_t frameSize, uint32_t seq, size_t offset, uint8_t* out);  // chunk datagram starting at offset to out, returns its length

	static void liveRequested(void);                // web UI retrieved a live frame - keeps screencast on for SCREENCAST_LIVE_TIMEOUT_MS
	static void liveStop(void);                     // web UI stopped screencast - off right away
	static bool liveActive(void);                   // should ST send screencast frames?
	static int  frameSkipForFps(int fps);           // SCREENCAST_FPS setting to VBLs between frames, as the driver wants it

private:
    void readScreen(void);
    void readPalette(void);
  	AcsiDataTrans *dataTrans;
  	uint8_t *cmd;
	uint8_t	*dataBuffer;
};
#endif
