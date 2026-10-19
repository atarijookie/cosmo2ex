#ifndef _SCREENCASTACSICOMMAND_H_
#define _SCREENCASTACSICOMMAND_H_

#include "../acsidatatrans.h"

#include <stddef.h>
#include <stdint.h>

#define SCREENCAST_BUFFER_SIZE      (64 * 1024)

// latest frame, served to webserver by 'get_frame' on the command socket: resolution (1) + palette (32) + screen data (32000)
#define SCREENCAST_FRAME_SIZE       (1 + 32 + 32000)

// the frame is sent as UDP datagrams of: offset (2 bytes, big endian) + frame size (2 bytes, big endian) + data
// payload kept under 8 KB - macOS limits UDP datagrams to 9216 bytes by default
#define SCREENCAST_CHUNK_HEADER     4
#define SCREENCAST_CHUNK_PAYLOAD    8000

class ScreencastAcsiCommand
{
public:
  	ScreencastAcsiCommand(AcsiDataTrans *dt);
  	~ScreencastAcsiCommand();
  	void processCommand(uint8_t *command);

	static void frameInit(void);                    // clear the frame and fill it with logo
	static void frameGet(uint8_t* out);             // copy of the whole frame (SCREENCAST_FRAME_SIZE bytes), taken under lock
	static size_t frameChunk(const uint8_t* frame, size_t offset, uint8_t* out);  // chunk datagram starting at offset to out, returns its length

private:
    void readScreen(void);
    void readPalette(void);
  	AcsiDataTrans *dataTrans;
  	uint8_t *cmd;
	uint8_t	*dataBuffer;
};
#endif
