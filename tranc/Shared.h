#include "License.h"

#ifndef _SHARED_H
#define _SHARED_H

#ifndef VOODOO_HDA_DEBUG_BUILD
#define VOODOO_HDA_DEBUG_BUILD 0
#endif

#define kVoodooHDAClassName	"VoodooHDADevice"
#ifndef SOUND_MIXER_NRDEVICES
#define SOUND_MIXER_NRDEVICES 25
#endif

enum {
    kVoodooHDAActionMethod = 0,
    kVoodooHDANumMethods
};

enum {
	kVoodooHDAActionTest = 0x1000
};

enum {
	kVoodooHDAMemoryMessageBuffer = 0x2000,
	kVoodooHDAMemoryPinDump,
	kVoodooHDAMemoryCommand = 0x3000,
	kVoodooHDAMemoryExtMessageBuffer
};
#define MAX_SLIDER_TAB_NAME_LENGTH 32

/*  Slice -> sorry AutumnRain = mixerDeviceInfo
typedef struct _sliderInfo{
	unsigned char num; //Порядковый	номер регулятора
	unsigned char value; //Значение усиления от 0 до 0x64
	char sliderName[MAX_SLIDER_TAB_NAME_LENGTH]; //Название регулятора
	unsigned char enabled; //Можно ли пользователю менять значение регулятора
}sliderInfo;

typedef struct sliders{
	char tabName[MAX_SLIDER_TAB_NAME_LENGTH]; //Название PinComplex для которого менятся значения усиления
	sliderInfo info[SOUND_MIXER_NRDEVICES];
	unsigned char size; //Число структур
	unsigned char non[3];
}sliders;
*/

enum {
	kVoodooHDAChannelNames = 0x3000
};

enum {
	kVoodooHDAActionSetMixer = 0x40,
	kVoodooHDAActionGetMixers = 0x50,
	kVoodooHDAActionSetMath = 0x60,
	kVoodooHDAActionSetDiag = 0x70,
	kVoodooHDAActionSetDebug = 0x80,
	kVoodooHDAActionGetDiagTelemetry = 0x90
};

enum {
	kVoodooHDADiagEnable             = 1U << 0,
	kVoodooHDADiagInjectMixTone      = 1U << 1,
	kVoodooHDADiagInjectDirectTone   = 1U << 2,
	kVoodooHDADiagFreezeBuffer       = 1U << 3,
	kVoodooHDADiagSkipErase          = 1U << 4,
	kVoodooHDADiagBypassProcessing   = 1U << 5,
	kVoodooHDADiagPrimeBufferOnStart = 1U << 6,
	kVoodooHDADiagSkipFramebufferELD = 1U << 7,
	kVoodooHDADiagForceAnyFramebufferELD = 1U << 8,
	kVoodooHDADiagForceATIELD        = 1U << 9,
	kVoodooHDADiagSkipAudioPipe      = 1U << 10,
	kVoodooHDADiagDumpGPUStateOnStream = 1U << 11,
	kVoodooHDADiagForceStandardHDMIPath = 1U << 12,
	kVoodooHDADiagForceATIVendorPath = 1U << 13
};

enum {
	kVoodooHDABuildSupportsDebug = 1U << 0
};

typedef union {
	struct {
		UInt8 action;
		UInt8 channel;
		UInt8 device;
		UInt8 val;
	} info;
	UInt32 value;
} actionInfo;

typedef struct _mixerDeviceInfo {
	UInt8 mixId;
	UInt8 value;
	char name[MAX_SLIDER_TAB_NAME_LENGTH];
	bool enabled;
	UInt8 non[5]; //align to 8 bytes
} mixerDeviceInfo;

typedef struct _ChannelInfo {
	char name[MAX_SLIDER_TAB_NAME_LENGTH];
	mixerDeviceInfo mixerValues[SOUND_MIXER_NRDEVICES];
	UInt8 numChannels;
	bool vectorize;
	bool useStereo;
    UInt8 noiseLevel;
	UInt8 StereoBase;
	UInt8 digital;   // PcmDevice digital type: 0=analog, 1=S/PDIF, 2=HDMI, 3=DisplayPort
	SInt8 direction; // PCMDIR_PLAY / PCMDIR_REC
	UInt16 diagnosticFlags;
	UInt8 debugLevel;
	UInt8 buildFlags;
} ChannelInfo;

enum {
	kVoodooHDADiagTelemetryMagic = 0x56484441U, /* VHDA */
	kVoodooHDADiagTelemetryVersion = 1
};

typedef struct _VoodooHDADiagTelemetry {
	UInt32 magic;
	UInt16 version;
	UInt16 size;
	UInt8 channel;
	UInt8 numChannels;
	UInt8 buildFlags;
	UInt8 debugLevel;
	UInt8 digital;
	SInt8 direction;
	UInt8 running;
	UInt8 streamActive;
	UInt16 diagnosticFlags;
	UInt16 codecVendor;
	UInt16 codecDevice;
	UInt16 codecFamily;
	UInt16 reserved0;
	UInt32 speed;
	UInt32 format;
	UInt32 streamId;
	UInt32 streamOffset;
	UInt32 numBlocks;
	UInt32 blockSize;
	UInt32 bufferSize;
	UInt32 slack;
	UInt32 linkPosition;
	UInt32 linkPositionValid;
	UInt32 currentSampleFrame;
	UInt32 numSampleFrames;
	UInt32 sampleSize;
	UInt32 clippedPosition;
	UInt32 diagnosticBufferPrimed;
	UInt32 diagnosticClipCalls;
	UInt32 diagnosticMixToneFills;
	UInt32 diagnosticDirectToneFills;
	UInt32 diagnosticEraseCalls;
	UInt32 diagnosticEraseSkips;
	UInt32 diagnosticLastFirstFrame;
	UInt32 diagnosticLastNumFrames;
	UInt32 pinNid;
	UInt32 cad;
	char channelName[MAX_SLIDER_TAB_NAME_LENGTH];
} VoodooHDADiagTelemetry;

#endif
