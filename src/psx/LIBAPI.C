#include "psx/libapi.h"

#include <stdio.h>
#include "../PsyX_main.h"

long sp = 0;

int dword_300[] = { 0x20, 0xD,  0x0,  0x0 };
int dword_308[] = { 0x10, 0x20, 0x40, 0x1 };

#define CTR_RUNNING (0)
#define CTR_STOPPED (1)

#define CTR_MODE_TO_FFFF (0)
#define CTR_MODE_TO_TARG (1)

#define CTR_CLOCK_SYS (0)
#define CTR_CLOCK_PIXEL (1)
#define CTR_HORIZ_RETRACE (1)

#define CTR_CLOCK_SYS_ONE (0)
#define CTR_CLOCK_SYS_ONE_EIGHTH (1)

typedef struct
{
	unsigned int i_cycle;

	union
	{
		unsigned short cycle;
		unsigned short unk00;
	};

	unsigned int i_value;

	union
	{
		unsigned short value;
		unsigned short unk01;
	};

	unsigned int i_target;

	union
	{
		unsigned short target;
		unsigned short unk02;
	};


	unsigned int padding00;
	unsigned int padding01;
} SysCounter;

extern SysCounter counters[3];

SysCounter counters[3] = { 0 };

int SetRCnt(int spec, unsigned short target, int mode)//(F)
{
	int value = 0x48;

	spec &= 0xFFFF;
	if (spec > 2)
	{
		return 0;
	}

	counters[spec].value = 0;
	counters[spec].target = target;

	if (spec < 2)
	{
		if ((mode & 0x10))
		{
			value = 0x49;
		}
		else if ((mode & 0x1))//loc_148
		{
			value |= 0x100;
		}
	}
	else
	{
		//loc_158
		if (spec == 2 && !(mode & 1))
		{
			value = 0x248;
		}//loc_174
	}
	//loc_174
	if ((mode & 0x1000))
	{
		value |= 0x10;
	}//loc_180

	counters[spec].value = value;

	return 1;
}

int GetRCnt(int spec)//(F)
{
	spec &= 0xFFFF;

	if (spec > 2)
	{
		return 0;
	}

	return counters[spec].cycle;
}

int ResetRCnt(int spec)//(F)
{
	spec &= 0xFFFF;

	if (spec > 2)
	{
		return 0;
	}

	counters[spec].cycle = 0;

	return 1;
}

int StartRCnt(int spec)//(F)
{
	spec &= 0xFFFF;
	dword_300[1] |= dword_308[spec];
	return spec < 3 ? 1 : 0;
}

int StopRCnt(int spec)//TODO
{
	return 0;
}
#undef OpenEvent
int OpenEvent(unsigned int event, int unk01, int unk02, long(*func)())
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int CloseEvent(unsigned int event)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int EnableEvent(unsigned int event)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int DisableEvent(unsigned int event)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int WaitEvent(unsigned int event)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int TestEvent(unsigned int event)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void DeliverEvent(unsigned int ev1, int ev2)
{
	PSYX_UNIMPLEMENTED();
}

void UnDeliverEvent(unsigned int ev1, int ev2)
{
	PSYX_UNIMPLEMENTED();
}

int OpenTh(int(*func)(), unsigned int unk01, unsigned int unk02)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int CloseTh(int unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int ChangeTh(int unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

/*
int open(char* unk00, unsigned int unk01)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int close(int unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int lseek(int unk00, int unk01, int unk02)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int read(int unk00, void* unk01, int unk02)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int write(int unk00, void* unk01, int unk02)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int ioctl(int unk00, int unk01, int unk02)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

struct DIRENTRY* firstfile(char* unk00, struct DIRENTRY* unk01)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

struct DIRENTRY* nextfile(struct DIRENTRY* unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int erase(char* unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int undelete(char* unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int format(char* unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}
int rename(char* unk00, char* unk01)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int cd(char* unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}
*/

int LoadTest(char*  unk00, struct EXEC* unk01)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int Load(char * unk00, struct EXEC* unk01)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int Exec(struct EXEC * unk00, int unk01, char** unk02)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int LoadExec(char * unk00, unsigned int unk01, unsigned int unk02)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int InitPAD(char * unk00, int unk01, char* unk02, int unk03)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int StartPAD()
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void StopPAD()
{
	PSYX_UNIMPLEMENTED();
}

void EnablePAD()
{
	PSYX_UNIMPLEMENTED();
}

void DisablePAD()
{
	PSYX_UNIMPLEMENTED();
}

void FlushCache()
{
	PSYX_UNIMPLEMENTED();
}

void ReturnFromException()
{
	PSYX_UNIMPLEMENTED();
}
/*
int EnterCriticalSection()
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void ExitCriticalSection()
{
	PSYX_UNIMPLEMENTED();
}
*/
void Exception()
{
	PSYX_UNIMPLEMENTED();
}

void SwEnterCriticalSection()
{
	PSYX_UNIMPLEMENTED();
}
void SwExitCriticalSection()
{
	PSYX_UNIMPLEMENTED();
}

unsigned long SetSp(unsigned long newsp)//(F)
{
	unsigned long old_sp = sp;
	sp = newsp;
	return old_sp;
}

unsigned long GetSp()
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

unsigned long GetGp()
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

unsigned long GetCr()
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

unsigned long GetSr()
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

unsigned long GetSysSp()
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int SetConf(unsigned int unk00, unsigned int unk01, unsigned int unk02)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void GetConf(unsigned int* unk00, unsigned int* unk01, unsigned int* unk02)
{
	PSYX_UNIMPLEMENTED();
}

/*
int _get_errno(void)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int _get_error(int unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}
*/
void SystemError(char unk00, int unk01)
{
	PSYX_UNIMPLEMENTED();
}

void SetMem(int unk00)
{
	PSYX_UNIMPLEMENTED();
}

int Krom2RawAdd(unsigned int unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int Krom2RawAdd2(unsigned short unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void _96_init(void)
{
	PSYX_UNIMPLEMENTED();
}

void _96_remove(void)
{
	PSYX_UNIMPLEMENTED();
}

void _boot(void)
{
	PSYX_UNIMPLEMENTED();
}

void ChangeClearPAD(int unk00)
{
	PSYX_UNIMPLEMENTED();
}

void InitCARD(int val)
{
	PSYX_UNIMPLEMENTED();
}

int StartCARD()
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int StopCARD()
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void _bu_init()
{
	PSYX_UNIMPLEMENTED();
}

int _card_info(int chan)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int _card_clear(int chan)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int _card_load(int chan)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int _card_auto(int val)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void _new_card()
{
	PSYX_UNIMPLEMENTED();
}

int _card_status(int drv)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int _card_wait(int drv)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

unsigned int _card_chan(void)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int _card_write(int chan, int block, unsigned char *buf)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int _card_read(int chan, int block, unsigned char *buf)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int _card_format(int chan)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}
