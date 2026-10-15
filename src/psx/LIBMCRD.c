#include "../PsyX_main.h"
#include "psx/libmcrd.h"
#include <stdio.h>
#include <string.h>

#define MC_HEADER_FRAME_INDEX (0)

#pragma pack(push,1)
typedef struct MemoryCardFrame
{
	unsigned int attr;
	unsigned int size;
	unsigned short unknown;
	char name[20];
	char padding[98];
} MemoryCardFrame_s, *MemoryCardFrame_p;
#pragma pack(pop)

int bIsInitialised = 0;
int bCanUseMemoryCardFuncs = 0;
int memoryCardStatus = -1;

FILE* memoryCards[2];
int memoryCardsNew[2];

int memoryCardCmds = -1;
int memoryCardResult = -1;
int openFrameIndex = 0;
int currentlyOpenedMemoryCard = -1;

void MemCardInit(int val)
{
	bIsInitialised = 1;
	bCanUseMemoryCardFuncs = 0;
	memoryCardStatus = -1;
	memoryCardCmds = -1;
	memoryCardResult = -1;
	memoryCardsNew[0] = 1;
	memoryCardsNew[1] = 1;
}

void MemCardEnd()
{
	if (!bCanUseMemoryCardFuncs)
		return;

}

void MemCardStart()
{
	bCanUseMemoryCardFuncs = 1;
}

void MemCardStop()
{
	if (!bCanUseMemoryCardFuncs)
		return;

	bCanUseMemoryCardFuncs = 0;
	memoryCardStatus = -1;
	memoryCardCmds = -1;
	memoryCardResult = -1;
	memoryCardsNew[0] = 1;
	memoryCardsNew[1] = 1;

	if (memoryCards[0] != NULL)
	{
		fclose(memoryCards[0]);
	}

	if (memoryCards[1] != NULL)
	{
		fclose(memoryCards[1]);
	}
}

int MemCardExist(int chan)
{
	if (!bCanUseMemoryCardFuncs)
		return 0;

	char buf[16];
	sprintf(&buf[0], "%ld.MCD", chan);
	memoryCards[chan] = fopen(&buf[0], "rb");

	memoryCardCmds = McFuncExist;

	if (memoryCards[chan] == NULL)
	{
		memoryCardStatus = -1;//CHECKME
		memoryCardResult = McErrCardNotExist;//CHECKME
		return 0;
	}
	else
	{
		fclose(memoryCards[chan]);

		if (memoryCardResult == McErrNewCard)
		{
			memoryCardResult = McErrNone;
			memoryCardStatus = 0;
		}
		else
		{
			memoryCardResult = McErrNewCard;
			memoryCardStatus = 1;
		}
	}

	
	return 1;
}

int MemCardAccept(int chan)
{
	if (!bCanUseMemoryCardFuncs)
		return 0;

	char buf[16];
	sprintf(&buf[0], "%ld.MCD", chan);
	memoryCards[chan] = fopen(&buf[0], "rb");
	memoryCardCmds = McFuncAccept;

	unsigned int fileMagic = 0;
	fread(&fileMagic, 4, 1, memoryCards[chan]);
	fclose(memoryCards[chan]);

	//Is this card formatted?
	if (fileMagic != 0x0000434D)
	{
		//If not, this is a new card!
		memoryCardResult = McErrNewCard;
		memoryCardsNew[chan] = 0;
		return 0;
	}

	memoryCardResult = 3;
	memoryCardStatus = 1;
	return 1;
}
int MemCardOpen(int chan, char* file, int flag)
{
	if (!bCanUseMemoryCardFuncs)
		return 0;

	char buf[16];
	sprintf(&buf[0], "%ld.MCD", chan);

	switch (flag)
	{
	case 1:
		memoryCards[chan] = fopen(&buf[0], "rb");
		break;
	case 2://Unchecked
		memoryCards[chan] = fopen(&buf[0], "wb");
		break;
	}
	
	fseek(memoryCards[chan], 0, SEEK_SET);
	currentlyOpenedMemoryCard = chan;

	for (int i = 0; i < 16; i++)
	{
		struct MemoryCardFrame frame;
		fread(&frame, sizeof(struct MemoryCardFrame), 1, memoryCards[chan]);

		if (i > MC_HEADER_FRAME_INDEX && frame.name[0] != '\0')
		{
			if (strcmp(&frame.name[0], file) == 0)
			{
				break;
			}

			openFrameIndex += frame.attr & 0x7;
		}
	}

	return 0;
}

void MemCardClose()
{
	openFrameIndex = -1;
	fclose(memoryCards[currentlyOpenedMemoryCard]);
}

int MemCardReadData(unsigned int* adrs, int ofs, int bytes)
{
	memoryCardCmds = McFuncReadData;
	if (bytes % 128)
	{
		return 0;
	}

	fseek(memoryCards[currentlyOpenedMemoryCard], (64 * 128) + (openFrameIndex * 16384) + ofs, SEEK_SET);
	fread(adrs, bytes, 1, memoryCards[currentlyOpenedMemoryCard]);

	return 1;
}

int MemCardReadFile(int chan, char* file, unsigned int* adrs, int ofs, int bytes)
{
	memoryCardCmds = McFuncReadFile;
	return 0;
}

int MemCardWriteData(unsigned int* adrs, int ofs, int bytes)
{
	memoryCardCmds = McFuncWriteData;
	return 0;
}

int MemCardWriteFile(int chan, char* file, unsigned int* adrs, int ofs, int bytes)
{
	memoryCardCmds = McFuncWriteFile;

	return 0;
}

int MemCardCreateFile(int chan, char* file, int blocks)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int MemCardDeleteFile(int chan, char* file)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int MemCardFormat(int chan)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int MemCardUnformat(int chan)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int MemCardSync(int mode, int* cmds, int* rslt)
{
	static int timesCalled = 0;

	//if (timesCalled++ >= 4) //Doesn't work o.o
	{
		timesCalled = 0;

		if (memoryCardCmds != -1)
		{
			*cmds = memoryCardCmds;
		}

		if (memoryCardResult != -1)
		{
			*rslt = memoryCardResult;
		}

		if (mode == 1)
		{
			return memoryCardStatus;
		}
	}

	return -1;
}

MemCB MemCardCallback(MemCB func)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int MemCardGetDirentry(int chan, char* name, struct DIRENTRY* dir, int* files, int ofs, int max)
{
	char buf[16];
	sprintf(&buf[0], "%ld.MCD", chan);
	memoryCards[chan] = fopen(&buf[0], "rb");///@FIXME potential bug, if this is called twice then we can open a card twice. Maybe add a flag for whether memcard is open or not if original SDK did this.
	fseek(memoryCards[chan], 0, SEEK_SET);

	if (strcmp(name, "*") == 0)
	{
		for (int i = 0, head = -64; i < 16; i++, head += 128)
		{
			struct MemoryCardFrame frame;
			fread(&frame, sizeof(struct MemoryCardFrame), 1, memoryCards[chan]);

			if (i > MC_HEADER_FRAME_INDEX && frame.name[0] != '\0')
			{
				memcpy(dir->name, &frame.name[0], 20);
				dir->attr = frame.attr & 0xF0;
				dir->size = frame.size;
				dir->next = (struct DIRENTRY*)9;
				dir->head = head;
				dir->system[0] = 9;
				dir++;
				files[0]++;
			}
		}
	}
	memoryCardCmds = McFuncExist;
	memoryCardResult = 0;
	memoryCardStatus = 1;

	return 0;
}
