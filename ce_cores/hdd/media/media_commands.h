#ifndef _MEDIA_COMMANDS_H_
#define _MEDIA_COMMANDS_H_

// Commands of the CosmosEx media module (HOSTMOD_MEDIA_STREAMING), sent by CE_VIDEO.TTP and CE_AUDIO.TTP
// (../../../ce_media_prg). The file is decoded on the host by ce_media (../../../ce_media, runs ffmpeg) and converted
// to exactly what the Atari puts in its screen memory and sound buffer - the Atari only moves the data where it
// belongs and keeps the timing. Keep in sync with the copy used by ce_media (protocol.go).
//
// The same stream format is meant for other sources than files later (a game running on the host which gets the
// keys of the ST through MED_CMD_INPUT): a stream is a sequence of chunks, each one DMA transfer.
//
// Command format, pCmd as MediaModule::processCommand() gets it (for ICD commands the 1st byte is already skipped):
//   [0] xx  [1] 'C' [2] 'E' [3] HOSTMOD_MEDIA_STREAMING [4] cmd [5..11] args
// Multi byte values are big endian. The status byte is MED_OK or MED_ERR_*.
//
// Typical session:  IDENTIFY, OPEN, GET_INFO (until state isn't STARTING), [GET_YM_TABLE], GET_CHUNK ... (until a chunk
// has MED_CHF_END), CLOSE.  SEEK any time after READY - chunks of older generations are not given out after it.

#define MEDIA_MODULE_VERSION            0x0100      // in IDENTIFY reply

// status codes
#define MED_OK                          0x00
#define MED_ERR_NOT_RUNNING             0x01        // ce_media is not connected (not installed, can't start)
#define MED_ERR_BAD_PARAM               0x02
#define MED_ERR_NOT_FOUND               0x03        // file doesn't exist, or isn't on a translated drive
#define MED_ERR_NO_STREAM               0x04        // command needs OPEN first
#define MED_ERR_INTERNAL                0x05        // data transfer with the ST failed

// commands
#define MED_CMD_IDENTIFY                0x00        // reply (512 B): "CosmosEx media module" (21 B + terminator), padding to 32,
                                                    //   version (W @ 32), flags (B @ 34, MED_IDF_*). Also starts ce_media.
#define MED_CMD_OPEN                    0x01        // data to host: 512 B, see MED_OPEN_* below. Stops the previous stream.
#define MED_CMD_GET_INFO                0x02        // reply: 512 B, see MED_INFO_* below
#define MED_CMD_GET_YM_TABLE            0x03        // reply: MED_YM_TABLE_SIZE bytes - for MED_AFMT_YM, after READY
#define MED_CMD_CLOSE                   0x04        // stop the stream (ce_media stops decoding)
#define MED_CMD_GET_CHUNK               0x10        // [5] max sectors of the reply, [6..9] generation the ST plays.
                                                    //   Reply: chunk (see MED_CHUNK_*), or just its header when no chunk
                                                    //   is ready yet (MED_CHF_EMPTY).
#define MED_CMD_SEEK                    0x11        // [5..8] position in ms from the start of the file (clamped by the host).
                                                    //   Reply (512 B): [0..3] new generation, [4..7] position it starts at (ms)
#define MED_CMD_INPUT                   0x12        // [5] count (0..6), [6..11] IKBD scancodes (bit 7 = key released) -
                                                    //   for streams which react to keys (games), files ignore them

// IDENTIFY flags
#define MED_IDF_CONNECTED               0x01        // ce_media is connected

//------------------------------------------------------------------------------------------------------------------
// OPEN data (512 B)
#define MED_OPEN_MACHINE                0           // B: MED_MACH_*
#define MED_OPEN_VMODE                  1           // B: MED_VMODE_* the ST will show (NONE = audio only)
#define MED_OPEN_AOUT                   2           // B: MED_AOUT_* the ST has
#define MED_OPEN_FLAGS                  3           // B: MED_OPF_*
#define MED_OPEN_LATENCY                4           // W: ms the ST buffers before it starts playing (the host keeps chunks for that too)
#define MED_OPEN_MAX_FPS                6           // B: 0 = default of the video mode
#define MED_OPEN_MAX_SECTORS            7           // B: biggest chunk the ST takes in one transfer (1..254)
#define MED_OPEN_START_MS               8           // DW: where to start, ms
#define MED_OPEN_PATH                   16          // zero terminated Atari path (full, with drive letter), up to the end
#define MED_OPEN_SIZE                   512
#define MED_OPEN_PATH_SIZE              (MED_OPEN_SIZE - MED_OPEN_PATH)

#define MED_MACH_ST                     0
#define MED_MACH_STE                    1           // also Mega STE
#define MED_MACH_TT                     2
#define MED_MACH_FALCON                 3

#define MED_VMODE_NONE                  0           // no picture (CE_AUDIO, or a file without video)
#define MED_VMODE_ST_LOW                1           // 320 x 200, 4 planes, whole screen - frames go straight to the screen
#define MED_VMODE_ST_HIGH               2           // 640 x 400, mono, whole screen
#define MED_VMODE_TT_MID_WIN            3           // 320 x 240, 4 planes - in the middle of TT mid 640 x 480 (half resolution)
#define MED_VMODE_TT_HIGH_WIN           4           // 640 x 480, mono - in the middle of TT high 1280 x 960

#define MED_AOUT_NONE                   0           // no sound (clock from the 200 Hz timer)
#define MED_AOUT_DMA                    1           // STE / TT / Falcon DMA sound
#define MED_AOUT_YM                     2           // plain ST - samples through the YM2149 volume registers

#define MED_OPF_LIVE                    0x01        // low latency: 1 frame per chunk, the host keeps only the newest chunks

//------------------------------------------------------------------------------------------------------------------
// GET_INFO reply (512 B)
#define MED_INFO_STATE                  0           // B: MED_STATE_*
#define MED_INFO_VMODE                  1           // B: MED_VMODE_* of the frames (NONE when the file has no video)
#define MED_INFO_AFMT                   2           // B: MED_AFMT_* of the audio (NONE when the file has no audio / ST has none)
#define MED_INFO_FRAMES_PER_CHUNK       3           // B: most frames in one chunk (0 = no video)
#define MED_INFO_FRAME_BYTES            4           // W: bytes of one frame (bitplanes, rows of FRAME_W / 16 * planes words)
#define MED_INFO_SLOT_SECTORS           6           // W: sectors one frame takes in the chunk (frame + padding)
#define MED_INFO_CLOCK_RATE             8           // DW: clock ticks per second - the audio sample rate, 200 without audio
#define MED_INFO_DMA_MODE               12          // B: value for the DMA sound mode register (0xff8921)
#define MED_INFO_YM_TIMER_CTRL          13          // B: MFP timer A control (prescaler) for MED_AFMT_YM
#define MED_INFO_YM_TIMER_DATA          14          // B: MFP timer A data
#define MED_INFO_FPS_X1000              16          // DW
#define MED_INFO_DURATION_MS            20          // DW: 0 = unknown
#define MED_INFO_CHUNK_SECTORS          24          // W: most sectors of one chunk (header + frames + audio)
#define MED_INFO_AUDIO_MAX              26          // W: most audio bytes in one chunk
#define MED_INFO_FRAME_W                28          // W: frame width in pixels (multiple of 16)
#define MED_INFO_FRAME_H                30          // W: frame height in rows
#define MED_INFO_PLANES                 32          // B
#define MED_INFO_TITLE                  64          // zero terminated: title (or file name)
#define MED_INFO_TITLE_SIZE             128
#define MED_INFO_MSG                    192         // zero terminated: error message, or what is happening while STARTING
#define MED_INFO_MSG_SIZE               128
#define MED_INFO_SIZE                   512

#define MED_STATE_STARTING              0           // ce_media is starting / looking at the file - ask again
#define MED_STATE_READY                 1           // chunks are coming
#define MED_STATE_ERROR                 2           // see MED_INFO_MSG
#define MED_STATE_NONE                  3           // no stream (no OPEN, or CLOSE)

#define MED_AFMT_NONE                   0
#define MED_AFMT_DMA_S8_MONO            1           // signed 8 bit mono, for the DMA sound
#define MED_AFMT_YM                     2           // 1 byte per sample = index to the YM table (GET_YM_TABLE)

//------------------------------------------------------------------------------------------------------------------
// Chunk: MED_CHUNK_HEADER_SIZE header, then 'frame count' frames each in SLOT_SECTORS * 512 bytes, then the audio.
// A chunk starts in a 512 B aligned buffer on the ST, so every frame in it is 256 B aligned - ST / STE show them
// right from there (Setscreen), no copying. All times are in clock ticks (MED_INFO_CLOCK_RATE) since the start of the
// generation (generation = OPEN or SEEK).
#define MED_CHUNK_GENERATION            0           // DW
#define MED_CHUNK_SEQ                   4           // DW: 0, 1, 2, ... within the generation
#define MED_CHUNK_FLAGS                 8           // B: MED_CHF_*
#define MED_CHUNK_FRAME_COUNT           9           // B: frames in this chunk
#define MED_CHUNK_AUDIO_START           12          // DW: clock tick of the 1st audio sample in this chunk
#define MED_CHUNK_AUDIO_BYTES           16          // DW: audio bytes in this chunk (even)
#define MED_CHUNK_GEN_START_MS          20          // DW: position in the file where this generation starts (ms)
#define MED_CHUNK_FRAMES                32          // frame table: per frame 8 B - [0..3] tick to show it at, [4..7] reserved
#define MED_CHUNK_FRAME_ENTRY           8
#define MED_CHUNK_PALETTES              96          // per frame 16 W: the palette registers (none in mono)
#define MED_CHUNK_PALETTE_SIZE          32
#define MED_CHUNK_MAX_FRAMES            8
#define MED_CHUNK_HEADER_SIZE           512

#define MED_CHF_EMPTY                   0x01        // no chunk ready yet - just this header, ask again later
#define MED_CHF_END                     0x02        // last chunk of the stream (with EMPTY: the stream ended before)

// Palette word formats: MED_VMODE_ST_LOW on ST - 0x0RGB 3 bits; on STE / Falcon - 0x0RGB 4 bits in the STE order
// (bit 3 is the lowest); MED_VMODE_TT_MID_WIN - TT palette registers (0xff8400..), 0x0RGB 4 bits.

// YM table: 256 entries of 16 B - three longwords to write to 0xffff8800 (register << 24 | value << 8, for the volume
// registers 8, 9, 10), then the output level (DW, 0..65535, for information).
#define MED_YM_TABLE_ENTRY              16
#define MED_YM_TABLE_SIZE               (256 * MED_YM_TABLE_ENTRY)

#endif
