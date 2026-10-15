/*
  Hatari - msa.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.
*/

#include <stdint.h>

#define Uint8   uint8_t
#define Uint16  uint16_t

extern bool MSA_FileNameIsMSA(const char *pszFileName, bool bAllowGZ);
extern Uint8 *MSA_UnCompress(Uint8 *pMSAFile, long nFileSize, long *pImageSize);   // nFileSize: bytes in pMSAFile, never read beyond
extern Uint8 *MSA_ReadDisk(const char *pszFileName, long *pImageSize);
extern bool MSA_WriteDisk(const char *pszFileName, Uint8 *pBuffer, int ImageSize, int nTracks, int nSides, int nSectorsPerTrack, int nStartTrack = 0);  // geometry = what image was loaded with; pBuffer holds tracks 0..nTracks-1, file gets nStartTrack..nTracks-1
bool File_Save(const char *pszFileName, const Uint8 *pAddress, int Size);
