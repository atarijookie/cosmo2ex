/*
  Hatari - msa.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  MSA Disk support
*/
const char MSA_fileid[] = "Hatari msa.c : " __DATE__ " " __TIME__;

/*
#include <SDL_endian.h>

#include "main.h"
#include "file.h"
#include "floppy.h"
*/

#include "msa.h"
#include "floppyimage.h"

/*
#include "sysdeps.h"
#include "maccess.h"
*/

#define SAVE_TO_MSA_IMAGES

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../misc/utils.h"
#include "../misc/debug.h"


#define NUMBYTESPERSECTOR 512					/* All supported disk images are 512 bytes per sector */

/*
    .MSA FILE FORMAT
  --================------------------------------------------------------------

  For those interested, an MSA file is made up as follows:

  Header:

  Word  ID marker, should be $0E0F
  Word  Sectors per track
  Word  Sides (0 or 1; add 1 to this to get correct number of sides)
  Word  Starting track (0-based)
  Word  Ending track (0-based)

  Individual tracks follow the header in alternating side order, e.g. a double
  sided disk is stored as:

  TRACK 0, SIDE 0
  TRACK 0, SIDE 1
  TRACK 1, SIDE 0
  TRACK 1, SIDE 1
  TRACK 2, SIDE 0
  TRACK 2, SIDE 1

  ...and so on. Track blocks are made up as follows:

  Word  Data length
  Bytes  Data

  If the data length is equal to 512 x the sectors per track value, it is an
  uncompressed track and you can merely copy the data to the appropriate track
  of the disk. However, if the data length value is less than 512 x the sectors
  per track value it is a compressed track.

  Compressed tracks use simple a Run Length Encoding (RLE) compression method.
  You can directly copy any data bytes until you find an $E5 byte. This signals
  a compressed run, and is made up as follows:

  Byte  Marker - $E5
  Byte  Data byte
  Word  Run length

  So, if MSA found six $AA bytes in a row it would encode it as:

  $E5AA0006

  What happens if there's an actual $E5 byte on the disk? Well, logically
  enough, it is encoded as:

  $E5E50001

  This is obviously bad news if a disk consists of lots of data like
  $E500E500E500E500... but if MSA makes a track bigger when attempting to
  compress it, it just stores the uncompressed version instead.

  MSA only compresses runs of at least 4 identical bytes (after all, it would be
  wasteful to store 4 bytes for a run of only 3 identical bytes!). There is one
  exception to this rule: if a run of 2 or 3 $E5 bytes is found, that is stored
  appropriately enough as a run. Again, it would be wasteful to store 4 bytes
  for every single $E5 byte.

  The hacked release of MSA that enables the user to turn off compression
  completely simply stops MSA from trying this compression and produces MSA
  images that are completely uncompressed. This is okay because it is possible
  for MSA to produce such an image anyway, and such images are therefore 100%
  compatible with normal MSA versions (and MSA-to-ST of course).
*/

typedef struct
{
	short int ID;                 /* Word  ID marker, should be $0E0F */
	short int SectorsPerTrack;    /* Word  Sectors per track */
	short int Sides;              /* Word  Sides (0 or 1; add 1 to this to get correct number of sides) */
	short int StartingTrack;      /* Word  Starting track (0-based) */
	short int EndingTrack;        /* Word  Ending track (0-based) */
} MSAHEADERSTRUCT;

#define MSA_WORKSPACE_SIZE  (1024*1024)  /* Size of workspace to use when saving MSA files */


static uint16_t do_get_mem_word(void *a)
{
	const Uint8 *p = (const Uint8 *) a;			// read big endian word byte by byte - 'a' can be on odd address
	return (Uint16) ((p[0] << 8) | p[1]);
}

/*-----------------------------------------------------------------------*/
/**
 * Does filename end with a .MSA extension? If so, return true
 */
/*
bool MSA_FileNameIsMSA(const char *pszFileName, bool bAllowGZ)
{
	return(File_DoesFileExtensionMatch(pszFileName,".msa")
	       || (bAllowGZ && File_DoesFileExtensionMatch(pszFileName,".msa.gz")));
}
*/

/*-----------------------------------------------------------------------*/
/**
 * Uncompress .MSA data into a new buffer.
 */
Uint8 *MSA_UnCompress(Uint8 *pMSAFile, long nFileSize, long *pImageSize)
{
	MSAHEADERSTRUCT *pMSAHeader;
	Uint8 *pImageBuffer;
	Uint8 Byte,Data;
	int i,Track,Side,DataLength,NumBytesUnCompressed,RunLength;
	Uint8 *pBuffer = NULL;

	*pImageSize = 0;

	// whole file is in memory: pMSAFile .. pFileEnd (exclusive); never read beyond pFileEnd
	if (pMSAFile == NULL || nFileSize < (long) sizeof(MSAHEADERSTRUCT))
	{
		logFdd(LOG_ERROR, "MSA_UnCompress: file too short for MSA header (%ld bytes)", nFileSize);
		return NULL;
	}

	Uint8 *pFileEnd = pMSAFile + nFileSize;

	/* Is an '.msa' file?? Check header */
	pMSAHeader = (MSAHEADERSTRUCT *)pMSAFile;
	if (pMSAHeader->ID != (short int) Utils::SWAPWORD2(0x0E0F))
	{
		logFdd(LOG_ERROR, "MSA_UnCompress: not an MSA file (bad ID)");
		return NULL;
	}

	/* First swap 'header' words around to PC format - easier later on */
	int nSectorsPerTrack = (Uint16) Utils::SWAPWORD2(pMSAHeader->SectorsPerTrack);
	int nSides           = (Uint16) Utils::SWAPWORD2(pMSAHeader->Sides);
	int nStartingTrack   = (Uint16) Utils::SWAPWORD2(pMSAHeader->StartingTrack);
	int nEndingTrack     = (Uint16) Utils::SWAPWORD2(pMSAHeader->EndingTrack);

	pMSAHeader->SectorsPerTrack = nSectorsPerTrack;
	pMSAHeader->Sides           = nSides;
	pMSAHeader->StartingTrack   = nStartingTrack;
	pMSAHeader->EndingTrack     = nEndingTrack;

	/* Sanity check header, so we never allocate negative / huge buffers */
	if (nSectorsPerTrack < 1 || nSectorsPerTrack > 64 || nSides > 1 ||
	    nStartingTrack > nEndingTrack || nEndingTrack > 255)
	{
		logFdd(LOG_ERROR, "MSA_UnCompress: invalid header - sectors/track: %d, sides: %d, tracks: %d..%d",
		       nSectorsPerTrack, nSides + 1, nStartingTrack, nEndingTrack);
		return NULL;
	}

	int nBytesPerTrack = NUMBYTESPERSECTOR * nSectorsPerTrack;
	long nImageSize = (long) (nEndingTrack - nStartingTrack + 1) * (nSides + 1) * nBytesPerTrack;

	/* Create buffer */
	pBuffer = (uint8_t *) malloc(nImageSize);
	if (!pBuffer)
	{
		logFdd(LOG_ERROR, "MSA_UnCompress: failed to allocate %ld bytes", nImageSize);
		return NULL;
	}

	/* Set pointers */
	pImageBuffer = pBuffer;
	Uint8 *pMSAImageBuffer = pMSAFile + sizeof(MSAHEADERSTRUCT);

	/* Uncompress to memory as '.ST' disk image - NOTE: assumes 512 bytes
	 * per sector (use NUMBYTESPERSECTOR define)!!! */
	for (Track = nStartingTrack; Track <= nEndingTrack; Track++)
	{
		for (Side = 0; Side < (nSides+1); Side++)
		{
			/* Uncompress MSA Track, first check if is not compressed */
			if (pFileEnd - pMSAImageBuffer < 2)
			{
				logFdd(LOG_ERROR, "MSA_UnCompress: file ends before track %d side %d length -> truncated disk image?", Track, Side);
				free(pBuffer);
				return NULL;
			}

			DataLength = do_get_mem_word(pMSAImageBuffer);
			pMSAImageBuffer += sizeof(short int);

			if (DataLength > nBytesPerTrack || DataLength > pFileEnd - pMSAImageBuffer)
			{
				logFdd(LOG_ERROR, "MSA_UnCompress: track %d side %d has invalid length %d (track size %d, %ld bytes left in file) -> corrupted disk image?",
				       Track, Side, DataLength, nBytesPerTrack, (long) (pFileEnd - pMSAImageBuffer));
				free(pBuffer);
				return NULL;
			}

			Uint8 *pTrackDataEnd = pMSAImageBuffer + DataLength;	/* compressed data of this track must stay before this */

			if (DataLength == nBytesPerTrack)
			{
				/* No compression on track, simply copy and continue */
				memcpy(pImageBuffer, pMSAImageBuffer, nBytesPerTrack);
				pImageBuffer += nBytesPerTrack;
				pMSAImageBuffer += DataLength;
			}
			else
			{
				/* Uncompress track */
				NumBytesUnCompressed = 0;
				while (NumBytesUnCompressed < nBytesPerTrack)
				{
					if (pMSAImageBuffer >= pTrackDataEnd)
					{
						logFdd(LOG_ERROR, "MSA_UnCompress: track %d side %d compressed data too short -> corrupted disk image?", Track, Side);
						free(pBuffer);
						return NULL;
					}

					Byte = *pMSAImageBuffer++;
					if (Byte != 0xE5)                 /* Compressed header?? */
					{
						*pImageBuffer++ = Byte;       /* No, just copy byte */
						NumBytesUnCompressed++;
					}
					else
					{
						if (pTrackDataEnd - pMSAImageBuffer < 3)	/* need data byte + 16-bit run length */
						{
							logFdd(LOG_ERROR, "MSA_UnCompress: track %d side %d run cut off -> corrupted disk image?", Track, Side);
							free(pBuffer);
							return NULL;
						}

						Data = *pMSAImageBuffer++;    /* Byte to copy */
						RunLength = do_get_mem_word(pMSAImageBuffer);  /* For length */
						/* Limit length to size of track, incorrect images may overflow */
						if (RunLength+NumBytesUnCompressed > nBytesPerTrack)
						{
							logFdd(LOG_ERROR, "MSA_UnCompress: Illegal run length -> corrupted disk image?");
							RunLength = nBytesPerTrack - NumBytesUnCompressed;
						}
						pMSAImageBuffer += sizeof(short int);
						for (i = 0; i < RunLength; i++)
							*pImageBuffer++ = Data;   /* Copy byte */
						NumBytesUnCompressed += RunLength;
					}
				}
			}
		}
	}

	/* Set size of loaded image */
	*pImageSize = pImageBuffer - pBuffer;

	/* Return pointer to buffer, NULL if failed */
	return(pBuffer);
}


/*-----------------------------------------------------------------------*/
/**
 * Uncompress .MSA file into memory, set number bytes of the disk image and
 * return a pointer to the buffer.
 */
/*
Uint8 *MSA_ReadDisk(const char *pszFileName, long *pImageSize)
{
	Uint8 *pMsaFile;
	Uint8 *pDiskBuffer = NULL;

	*pImageSize = 0;

	// Read in file
	pMsaFile = File_Read(pszFileName, NULL, NULL);
	if (pMsaFile)
	{
		// Uncompress into disk buffer
		pDiskBuffer = MSA_UnCompress(pMsaFile, pImageSize);

		// Free MSA file we loaded
		free(pMsaFile);
	}

	// Return pointer to buffer, NULL if failed
	return pDiskBuffer;
}
*/

/*-----------------------------------------------------------------------*/
/**
 * Return number of bytes of the same byte in the passed buffer
 * If we return '0' this means no run (or end of buffer)
 */

static int MSA_FindRunOfBytes(Uint8 *pBuffer, int nBytesInBuffer)
{
	Uint8 ScannedByte;
	int nTotalRun;
	bool bMarker;
	int i;

	// Is this the marker? If so, this is at least a run of one.
	bMarker = (*pBuffer == 0xE5);

	// Do we enough for a run?
	if (nBytesInBuffer < 2)
	{
		if (nBytesInBuffer == 1 && bMarker)
			return 1;
		else
			return 0;
	}

	// OK, scan for run
	nTotalRun = 1;
	ScannedByte = *pBuffer++;

	for (i = 1; i < nBytesInBuffer; i++)
	{
		if (*pBuffer++ == ScannedByte)
			nTotalRun++;
		else
			break;
	}

	// Was this enough of a run to make a difference?
	if (nTotalRun < 4 && !bMarker)
		nTotalRun = 0;                  // Just store uncompressed

	return nTotalRun;
}


/*-----------------------------------------------------------------------*/
/**
 * Save compressed .MSA file from memory buffer. Returns true is all OK
 */

bool MSA_WriteDisk(const char *pszFileName, Uint8 *pBuffer, int ImageSize, int nTracks, int nSides, int nSectorsPerTrack, int nStartTrack)
{
	// Geometry comes from the caller (what the image was loaded with) - NOT from the boot sector, which is just data
	// the ST can change (e.g. zeroed on format), and which would make the saved file come back with different geometry
	MSAHEADERSTRUCT *pMSAHeader;
	unsigned short int *pMSADataLength;
	Uint8 *pMSAImageBuffer, *pMSABuffer, *pImageBuffer;
	int nCompressedBytes, nBytesPerTrack;		// int, not Uint16: compressed size can temporarily grow up to 4x track size
	bool nRet;
	int nBytesToGo,nBytesRun;
	int Track,Side;

	if (pBuffer == NULL || nStartTrack < 0 || nStartTrack >= nTracks || nTracks < 1 || nTracks > 256 || nSides < 1 || nSides > 2 || nSectorsPerTrack < 1 || nSectorsPerTrack > 64 ||
	    (long) nTracks * nSides * nSectorsPerTrack * NUMBYTESPERSECTOR != ImageSize)
	{
		logFdd(LOG_ERROR, "MSA_WriteDisk: invalid geometry %d tracks, %d sides, %d sectors/track for image of %d bytes - not saving",
		       nTracks, nSides, nSectorsPerTrack, ImageSize);
		return false;
	}

	nBytesPerTrack = NUMBYTESPERSECTOR * nSectorsPerTrack;

	// Allocate workspace for compressed image: each finished track takes at most 2 + nBytesPerTrack (stored uncompressed
	// if compression doesn't help), but while compressing one track it can temporarily grow up to 4x (single E5 bytes)
	long nWorkspaceSize = sizeof(MSAHEADERSTRUCT) + (long) nTracks * nSides * (2 + nBytesPerTrack) + 4L * nBytesPerTrack;
	pMSAImageBuffer = (Uint8 *)malloc(nWorkspaceSize);
	if (!pMSAImageBuffer)
	{
		logFdd(LOG_ERROR, "MSA_WriteDisk: failed to allocate %ld bytes", nWorkspaceSize);
		return false;
	}

	// Store header
	pMSAHeader = (MSAHEADERSTRUCT *)pMSAImageBuffer;
	pMSAHeader->ID = Utils::SWAPWORD2(0x0E0F);
	pMSAHeader->SectorsPerTrack = Utils::SWAPWORD2(nSectorsPerTrack);
	pMSAHeader->Sides = Utils::SWAPWORD2(nSides-1);
	pMSAHeader->StartingTrack = Utils::SWAPWORD2(nStartTrack);
	pMSAHeader->EndingTrack = Utils::SWAPWORD2(nTracks-1);

	// Compress image
	pMSABuffer = pMSAImageBuffer + sizeof(MSAHEADERSTRUCT);
	for (Track = nStartTrack; Track < nTracks; Track++)
	{
		for (Side = 0; Side < nSides; Side++)
		{
			// Get track data pointer
			pImageBuffer = pBuffer + (nBytesPerTrack*Side) + ((nBytesPerTrack*nSides)*Track);

			// Skip data length (fill in later)
			pMSADataLength = (Uint16 *)pMSABuffer;
			pMSABuffer += sizeof(Uint16);

			// Compress track
			nBytesToGo = nBytesPerTrack;
			nCompressedBytes = 0;
			while (nBytesToGo > 0)
			{
				nBytesRun = MSA_FindRunOfBytes(pImageBuffer,nBytesToGo);
				if (nBytesRun == 0)
				{
					// Just copy byte
					*pMSABuffer++ = *pImageBuffer++;
					nCompressedBytes++;
					nBytesRun = 1;
				}
				else
				{
					// Store run!
					*pMSABuffer++ = 0xE5;               // Marker
					*pMSABuffer++ = *pImageBuffer;      // Byte, and follow with 16-bit length
					Utils::storeWord(pMSABuffer, nBytesRun);
					pMSABuffer += sizeof(Uint16);
					pImageBuffer += nBytesRun;
					nCompressedBytes += 4;
				}
				nBytesToGo -= nBytesRun;
			}

			// Is compressed track smaller than the original?
			if (nCompressedBytes < nBytesPerTrack)
			{
				// Yes, store size - into the length slot before this track's data (not at the end of data)
				Utils::storeWord((Uint8 *) pMSADataLength, nCompressedBytes);
			}
			else
			{
				// No, just store uncompressed track - length goes into the length slot before this track's data
				Utils::storeWord((Uint8 *) pMSADataLength, nBytesPerTrack);
				pMSABuffer = ((Uint8 *)pMSADataLength) + 2;
				pImageBuffer = pBuffer + (nBytesPerTrack*Side) + ((nBytesPerTrack*nSides)*Track);
				memcpy(pMSABuffer,pImageBuffer, nBytesPerTrack);
				pMSABuffer += nBytesPerTrack;
			}
		}
	}

	// And save to file!
	nRet = File_Save(pszFileName,pMSAImageBuffer, pMSABuffer-pMSAImageBuffer);

	free(pMSAImageBuffer);                  // workspace is only needed for this save

	return nRet;
}

/*-----------------------------------------------------------------------*/
/**
* Save file to disk, return FALSE if errors
*/
bool File_Save(const char *pszFileName, const Uint8 *pAddress, int Size)
{
	bool bRet = false;

 #if HAVE_LIBZ
 	/* Normal file or gzipped file? */
 	if (File_DoesFileExtensionMatch(pszFileName, ".gz"))
 	{
 		gzFile hGzFile;
 		/* Create a gzipped file: */
 		hGzFile = gzopen(pszFileName, "wb");
 		if (hGzFile != NULL)
 		{
 			/* Write data, set success flag */
 			if (gzwrite(hGzFile, pAddress, Size) == (int)Size)
 				bRet = true;

 			gzclose(hGzFile);
 		}
 	}
 	else
 #endif  /* HAVE_LIBZ */
 	{
 		/* Normal file: write to temp file and rename it over the original - a crash during save can't destroy the image */
 		bRet = FloppyImage::saveFileAtomically(pszFileName, pAddress, (size_t) Size);
 	}

 	return bRet;
}
