#define HAVE_M_PI
#include "../PsyX_main.h"
#include "../audio/PsyX_SPUAL.h"
#include "psx/libapi.h"
#include "psx/libetc.h"

static int s_spu_EVdma = 0;
static int s_inTransfer = 0;
static int s_transferMode = SPU_TRANSFER_BY_DMA;
static SpuTransferCallbackProc s_transferCallback = NULL;

unsigned int SpuWrite(unsigned char* addr, unsigned int size)
{
	unsigned int result = PsyX_SPUAL_Write(addr, size);

	if (s_transferCallback)
		s_transferCallback();
	else
		s_inTransfer = 0;

	return result;
}

unsigned int SpuRead(unsigned char* addr, unsigned int size)
{
	return PsyX_SPUAL_Read(addr, size);
}

int SpuSetTransferMode(int mode)
{
	// TODO: handle different transfer modes?

	int mode_fix = mode == 0 ? 0 : 1;

	//trans_mode = mode;
	//transMode = mode_fix;
	s_transferMode = mode_fix;

	return mode_fix;
}

unsigned int SpuSetTransferStartAddr(unsigned int addr)
{
	return PsyX_SPUAL_SetTransferStartAddr(addr);
}

int SpuIsTransferCompleted(int flag)
{
#if 0
	int event = 0;

	if (s_transferMode == 1 || s_inTransfer == 1)
		return 1;

	event = TestEvent(s_spu_EVdma);
	if (flag == 1)
	{
		if (event != 0)
		{
			s_inTransfer = 1;
			return 1;
		}
		else
		{
			do
			{
				event = TestEvent(s_spu_EVdma);
			} while (event == 0);

			s_inTransfer = 1;
			return 1;
		}
	}

	if (event == 1)
		s_inTransfer = 1;

	return event;
#else
	return 1;
#endif
}

void SpuInit(void)
{
	ResetCallback();
#if 0
	if (s_spu_isCalled == 0)
	{
		s_spu_isCalled = 1;
		EnterCriticalSection();
		_SpuDataCallback(_spu_FiDMA);
		s_spu_EVdma = OpenEvent(HwSPU, EvSpCOMP, EvMdNOINTR, NULL);
		EnableEvent(_spu_EVdma);
		ExitCriticalSection();
	}
#endif
	PsyX_SPUAL_InitSound();
}

void SpuQuit(void)
{
	PsyX_SPUAL_ShutdownSound();
}

void SpuSetVoiceAttr(SpuVoiceAttr *arg)
{
	PsyX_SPUAL_SetVoiceAttr(arg);
}

void SpuSetKey(int on_off, unsigned int voice_bit)
{
	PsyX_SPUAL_SetKey(on_off, voice_bit);
}

int SpuGetKeyStatus(unsigned int voice_bit)
{
	return PsyX_SPUAL_GetKeyStatus(voice_bit);
}

void SpuGetAllKeysStatus(char* status)
{
	PsyX_SPUAL_GetAllKeysStatus(status);
}

void SpuSetKeyOnWithAttr(SpuVoiceAttr* attr)
{
	SpuSetVoiceAttr(attr);
	SpuSetKey(SPU_ON, attr->voice);
}

int SpuSetMute(int on_off)
{
	return PsyX_SPUAL_SetMute(on_off);
}

int SpuSetReverb(int on_off)
{
	return PsyX_SPUAL_SetReverb(on_off);
}

int SpuGetReverb(void)
{
	return PsyX_SPUAL_GetReverbState();
}

int SpuSetReverbModeParam(SpuReverbAttr* attr)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void SpuGetReverbModeParam(SpuReverbAttr* attr)
{
	PSYX_UNIMPLEMENTED();
}

int SpuSetReverbDepth(SpuReverbAttr* attr)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int SpuReserveReverbWorkArea(int on_off)
{
	return 1;
}

int SpuIsReverbWorkAreaReserved(int on_off)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

unsigned int SpuSetReverbVoice(int on_off, unsigned int voice_bit)
{
	return PsyX_SPUAL_SetReverbVoice(on_off, voice_bit);
}

unsigned int SpuGetReverbVoice(void)
{
	return PsyX_SPUAL_GetReverbVoice();
}

int SpuClearReverbWorkArea(int mode)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void SpuSetCommonAttr(SpuCommonAttr* attr)
{
	PSYX_UNIMPLEMENTED();
}

int SpuInitMalloc(int num, char* top)
{
	return PsyX_SPUAL_InitAlloc(num, top);
}

int SpuMalloc(int size)
{
	return PsyX_SPUAL_Alloc(size);
}

int SpuMallocWithStartAddr(unsigned int addr, int size)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void SpuFree(unsigned int addr)
{
	PsyX_SPUAL_Free(addr);
}

unsigned int SpuFlush(unsigned int ev)
{
	//PSYX_UNIMPLEMENTED();
	return 0;
}

void SpuSetCommonMasterVolume(short mvol_left, short mvol_right)// (F)
{
	//MasterVolume.VolumeLeft.Raw = mvol_left;
	//MasterVolume.VolumeRight.Raw = mvol_right;
	PSYX_UNIMPLEMENTED();
}

int SpuSetReverbModeType(int mode)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void SpuSetReverbModeDepth(short depth_left, short depth_right)
{
	PSYX_UNIMPLEMENTED();
}

void SpuGetVoiceVolume(int vNum, short* volL, short* volR)
{
	PsyX_SPUAL_GetVoiceVolume(vNum, volL, volR);
}

void SpuGetVoicePitch(int vNum, unsigned short* pitch)
{
	PsyX_SPUAL_GetVoicePitch(vNum, pitch);
}

#define VOICE_ATTRIB_SETTER_SHORTCUT(flag, field, value) \
	SpuVoiceAttr attr; \
	attr.voice = SPU_VOICECH(vNum); \
	attr.mask = flag; \
	attr.field = value; \
	SpuSetVoiceAttr(&attr)

void SpuSetVoiceVolume(int vNum, short volL, short volR)
{
	SpuVoiceAttr attr;

	attr.mask = SPU_VOICE_VOLL | SPU_VOICE_VOLR;
	attr.voice = SPU_VOICECH(vNum);
	attr.volume.left = volL;
	attr.volume.right = volR;

	SpuSetVoiceAttr(&attr);
}

void SpuSetVoicePitch(int vNum, unsigned short pitch)
{
	VOICE_ATTRIB_SETTER_SHORTCUT(SPU_VOICE_PITCH, pitch, pitch);
}

void SpuSetVoiceStartAddr(int vNum, unsigned int startAddr)
{
	VOICE_ATTRIB_SETTER_SHORTCUT(SPU_VOICE_WDSA, addr, startAddr);
}

void SpuSetVoiceAR(int vNum, unsigned short AR)
{
	VOICE_ATTRIB_SETTER_SHORTCUT(SPU_VOICE_ADSR_AR, ar, AR);
}

extern void SpuSetVoiceDR(int vNum, unsigned short DR)
{
	VOICE_ATTRIB_SETTER_SHORTCUT(SPU_VOICE_ADSR_DR, dr, DR);
}

extern void SpuSetVoiceSR(int vNum, unsigned short SR)
{
	VOICE_ATTRIB_SETTER_SHORTCUT(SPU_VOICE_ADSR_SR, sr, SR);
}

void SpuSetVoiceRR(int vNum, unsigned short RR)
{
	VOICE_ATTRIB_SETTER_SHORTCUT(SPU_VOICE_ADSR_RR, rr, RR);
}

extern void SpuSetVoiceSL(int vNum, unsigned short SL)
{
	VOICE_ATTRIB_SETTER_SHORTCUT(SPU_VOICE_ADSR_SL, sl, SL);
}

void SpuSetVoiceADSRAttr(int vNum,
	unsigned short AR, unsigned short DR,
	unsigned short SR, unsigned short RR,
	unsigned short SL,
	int ARmode, int SRmode, int RRmode)
{
	SpuVoiceAttr attr;

	attr.mask = SPU_VOICE_ADSR_AR | SPU_VOICE_ADSR_DR | 
				SPU_VOICE_ADSR_SR | SPU_VOICE_ADSR_RR | 
				SPU_VOICE_ADSR_SL |
				SPU_VOICE_ADSR_AMODE | SPU_VOICE_ADSR_SMODE | SPU_VOICE_ADSR_RMODE;

	attr.voice = SPU_VOICECH(vNum);
	attr.ar = AR;
	attr.dr = DR;
	attr.sr = SR;
	attr.rr = RR;
	attr.sl = SL;
	attr.a_mode = ARmode;
	attr.s_mode = SRmode;
	attr.r_mode = RRmode;

	SpuSetVoiceAttr(&attr);
}

SpuTransferCallbackProc SpuSetTransferCallback(SpuTransferCallbackProc func)
{
	SpuTransferCallbackProc oldFn = s_transferCallback;
	s_transferCallback = func;
	return oldFn;
}

int SpuReadDecodedData(SpuDecodedData * d_data, int flag)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int SpuSetIRQ(int on_off)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

unsigned int SpuSetIRQAddr(unsigned int x)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

SpuIRQCallbackProc SpuSetIRQCallback(SpuIRQCallbackProc x)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void SpuSetCommonCDMix(int cd_mix)
{
	PSYX_UNIMPLEMENTED();
}

void SpuSetCommonCDVolume(short cd_left, short cd_right)
{
	PSYX_UNIMPLEMENTED();
}

void SpuSetCommonCDReverb(int cd_reverb)
{
	PSYX_UNIMPLEMENTED();
}
