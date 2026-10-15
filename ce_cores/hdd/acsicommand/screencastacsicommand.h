#ifndef _SCREENCASTACSICOMMAND_H_
#define _SCREENCASTACSICOMMAND_H_

#include "../acsidatatrans.h"

#define SCREENCAST_BUFFER_SIZE      (64 * 1024)

class ScreencastAcsiCommand
{
public:
  	ScreencastAcsiCommand(AcsiDataTrans *dt);
  	~ScreencastAcsiCommand();
  	void processCommand(uint8_t *command);

	void sharedMemorySet(int aSharedMemFd, uint8_t* aSharedMemPointer);

	static void sharedMemoryOpen(int& aSharedMemFd, uint8_t** aSharedMemPointer);
	static void sharedMemoryClose(int& aSharedMemFd, uint8_t** aSharedMemPointer);

private:
    void readScreen(void);
    void readPalette(void);
  	AcsiDataTrans *dataTrans;
  	uint8_t *cmd;
	uint8_t	*dataBuffer;

	int sharedMemFd;
	uint8_t *sharedMemPointer;
};
#endif
