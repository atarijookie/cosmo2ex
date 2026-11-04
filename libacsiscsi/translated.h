// typed of devices / modules we support
#define HOSTMOD_CONFIG				1
#define HOSTMOD_LINUX_TERMINAL		2
#define HOSTMOD_TRANSLATED_DISK		3
#define HOSTMOD_NETWORK_ADAPTER		4
#define HOSTMOD_MEDIA_STREAMING		6		// video / audio player - CE_VIDEO.TTP, CE_AUDIO.TTP
// 7 = HOSTMOD_RAW_NET - answered on the Pico (ce_pico_hdd/sting_net.h)
#define HOSTMOD_BROWSER				8		// web browser - CE_BROWS.PRG

// commands for HOSTMOD_TRANSLATED_DISK
#define TRAN_CMD_IDENTIFY           0
#define TRAN_CMD_GETDATETIME        1
#define TRAN_CMD_SENDSCREENCAST     2
#define TRAN_CMD_SCREENCASTPALETTE  3
#define TRAN_CMD_SCREENSHOT_CONFIG  4

// ...other commands are just function codes from gemdos.h
