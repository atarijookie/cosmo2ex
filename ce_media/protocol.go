package main

// Values of ../ce_cores/hdd/media/media_commands.h (what the ST gets) and medialink_protocol.h (ce_cores <-> ce_media).
// Keep in sync.

const (
	// machines
	machST     = 0
	machSTE    = 1
	machTT     = 2
	machFalcon = 3

	// video modes
	vmodeNone      = 0
	vmodeSTLow     = 1
	vmodeSTHigh    = 2
	vmodeTTMidWin  = 3
	vmodeTTHighWin = 4

	// audio outputs of the ST
	aoutNone = 0
	aoutDMA  = 1
	aoutYM   = 2

	// OPEN flags
	opfLive     = 0x01
	opfGame     = 0x02 // path = name of a game on the host ("doom")
	opfHalfRate = 0x04 // sound at half the rate: YM 7877 Hz, DMA 12517 Hz - half the CPU time on a plain ST

	// OPEN data offsets (the first 16 bytes come in MLINK_OPEN)
	openMachine    = 0
	openVMode      = 1
	openAOut       = 2
	openFlags      = 3
	openLatency    = 4
	openMaxFps     = 6
	openMaxSectors = 7
	openStartMs    = 8
	openHeaderSize = 16

	// INFO block
	infoState          = 0
	infoVMode          = 1
	infoAFmt           = 2
	infoFramesPerChunk = 3
	infoFrameBytes     = 4
	infoSlotSectors    = 6
	infoClockRate      = 8
	infoDmaMode        = 12
	infoYmTimerCtrl    = 13
	infoYmTimerData    = 14
	infoFpsX1000       = 16
	infoDurationMs     = 20
	infoChunkSectors   = 24
	infoAudioMax       = 26
	infoFrameW         = 28
	infoFrameH         = 30
	infoPlanes         = 32
	infoTitle          = 64
	infoTitleSize      = 128
	infoMsg            = 192
	infoMsgSize        = 128
	infoSize           = 512

	stateStarting = 0
	stateReady    = 1
	stateError    = 2

	afmtNone = 0
	afmtDMA  = 1
	afmtYM   = 2

	// chunk header
	chunkGeneration  = 0
	chunkSeq         = 4
	chunkFlags       = 8
	chunkFrameCount  = 9
	chunkAudioStart  = 12
	chunkAudioBytes  = 16
	chunkGenStartMs  = 20
	chunkFrames      = 32
	chunkFrameEntry  = 8
	chunkPalettes    = 96
	chunkPaletteSize = 32
	chunkMaxFrames   = 8
	chunkHeaderSize  = 512

	chfEmpty = 0x01
	chfEnd   = 0x02

	ymTableEntry = 16
	ymTableSize  = 256 * ymTableEntry

	// link
	linkVersion    = 2 // 2: msgTaken
	linkHeaderSize = 5
	linkMaxPayload = 512 * 1024

	msgHello      = 0x01
	msgOpen       = 0x02
	msgSeek       = 0x03
	msgInput      = 0x04
	msgClose      = 0x05
	msgTaken      = 0x06 // generation, seq: the ST took this chunk of a live stream
	msgHelloReply = 0x81
	msgInfo       = 0x82
	msgYmTable    = 0x83
	msgChunk      = 0x84
)
