#include "License.h"

#ifndef _PRIVATE_H
#define _PRIVATE_H

#include "Registers.h"
#include "OssCompat.h"
#include "Shared.h"

/* Miscellaneous defines */

#define HDAC_DMA_ALIGNMENT		128
#define HDAC_CODEC_MAX			16

// xxx: check what these flags were for

#define HDAC_F_DMA_NOCACHE		0x00000001
#define HDAC_F_MSI				0x00000002

#define HDAC_UNSOLQ_MAX			64
#define HDAC_UNSOLQ_READY		0
#define HDAC_UNSOLQ_BUSY		1

/* Misc constants.. */

#define HDA_AMP_VOL_DEFAULT		(-1)
#define HDA_AMP_MUTE_DEFAULT	(0xffffffff)
#define HDA_AMP_MUTE_NONE		(0)
#define HDA_AMP_MUTE_LEFT		(1 << 0)
#define HDA_AMP_MUTE_RIGHT		(1 << 1)
#define HDA_AMP_MUTE_ALL		(HDA_AMP_MUTE_LEFT | HDA_AMP_MUTE_RIGHT)

#define HDA_AMP_LEFT_MUTED(v)	((v) & (HDA_AMP_MUTE_LEFT))
#define HDA_AMP_RIGHT_MUTED(v)	(((v) & HDA_AMP_MUTE_RIGHT) >> 1)

#define HDA_ADC_MONITOR			(1 << 0)
#define HDAA_IMIX_AS_DST		(2 << 0)

#define HDA_CTL_OUT				1
#define HDA_CTL_IN				2

/*********/

#define HDA_BDL_MIN				2
#define HDA_BDL_MAX				256
/* 16 blocks of 16 KB → interrupt every ~85ms at 48kHz/2ch/16bit.
 * Smaller periods reduce FIFO underrun risk on modern Intel PCH (Alder/Raptor Lake)
 * compared to the old HDA_BDL_MIN=2 (128KB blocks, interrupt every ~1.37s). */
#define HDA_BDL_DEFAULT			16

#define HDA_BLK_MIN				HDAC_DMA_ALIGNMENT
#define HDA_BLK_ALIGN			(~(HDA_BLK_MIN - 1))

#define HDA_BUFSZ_MIN			4096
	//#define HDA_BUFSZ_MAX			65536
#define HDA_BUFSZ_MAX			262144
#define HDA_BUFSZ_DEFAULT		HDA_BUFSZ_MAX

#define HDA_GPIO_MAX    8
#define HDA_MATCH_ALL    0xffffffff
#define HDA_INVALID    0xffffffff

#define HDA_PARSE_MAXDEPTH		10

#define HDAC_UNSOLTAG_EVENT_HP	0x00

/* Helper Macros */

#define HDAC_ISDCTL(n)			(_HDAC_ISDCTL((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_ISDSTS(n)			(_HDAC_ISDSTS((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_ISDPICB(n)			(_HDAC_ISDPICB((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_ISDCBL(n)			(_HDAC_ISDCBL((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_ISDLVI(n)			(_HDAC_ISDLVI((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_ISDFIFOD(n)		(_HDAC_ISDFIFOD((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_ISDFMT(n)			(_HDAC_ISDFMT((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_ISDBDPL(n)			(_HDAC_ISDBDPL((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_ISDBDPU(n)			(_HDAC_ISDBDPU((n), mInStreamsSup, mOutStreamsSup))

#define HDAC_OSDCTL(n)			(_HDAC_OSDCTL((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_OSDSTS(n)			(_HDAC_OSDSTS((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_OSDPICB(n)			(_HDAC_OSDPICB((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_OSDCBL(n)			(_HDAC_OSDCBL((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_OSDLVI(n)			(_HDAC_OSDLVI((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_OSDFIFOD(n)		(_HDAC_OSDFIFOD((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_OSDBDPL(n)			(_HDAC_OSDBDPL((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_OSDBDPU(n)			(_HDAC_OSDBDPU((n), mInStreamsSup, mOutStreamsSup))

#define HDAC_BSDCTL(n)			(_HDAC_BSDCTL((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_BSDSTS(n)			(_HDAC_BSDSTS((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_BSDPICB(n)			(_HDAC_BSDPICB((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_BSDCBL(n)			(_HDAC_BSDCBL((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_BSDLVI(n)			(_HDAC_BSDLVI((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_BSDFIFOD(n)		(_HDAC_BSDFIFOD((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_BSDBDPL(n)			(_HDAC_BSDBDPL((n), mInStreamsSup, mOutStreamsSup))
#define HDAC_BSDBDPU(n)			(_HDAC_BSDBDPU((n), mInStreamsSup, mOutStreamsSup))

/*************************************************************************************/
/*************************************************************************************/

typedef int nid_t;

typedef struct _DmaMemory DmaMemory;

typedef struct _RirbResponse RirbResponse;
typedef struct _CommandList CommandList;
typedef struct _BdlEntry BdlEntry;

typedef struct _ChannelCaps ChannelCaps;

typedef struct _Widget Widget;
typedef struct _AudioControl AudioControl;
typedef struct _AudioAssoc AudioAssoc;
typedef struct _PcmDevice PcmDevice;
typedef struct _FunctionGroup FunctionGroup;
typedef struct _Channel Channel;
typedef struct _Codec Codec;

class IODMACommand;

typedef struct _DmaMemory {
	const char *description;
	IOBufferMemoryDescriptor *md;
	UInt64 size;
	UInt64 physAddr;
	IOVirtualAddress virtAddr;
} DmaMemory;

/* Hold a response from a verb sent to a codec received via the rirb. */
typedef struct _RirbResponse {
	UInt32 response;
	UInt32 response_ex;
} RirbResponse;

#define HDAC_RIRB_RESPONSE_EX_SDATA_IN_MASK		0x0000000f
#define HDAC_RIRB_RESPONSE_EX_SDATA_IN_OFFSET	0
#define HDAC_RIRB_RESPONSE_EX_UNSOLICITED		0x00000010

#define HDAC_RIRB_RESPONSE_EX_SDATA_IN(response_ex)					\
		(((response_ex) & HDAC_RIRB_RESPONSE_EX_SDATA_IN_MASK) >>	\
		HDAC_RIRB_RESPONSE_EX_SDATA_IN_OFFSET)

/* This structure holds the list of verbs that are to be sent to the codec
 * via the corb and the responses received via the rirb. It's allocated by
 * the codec driver and is owned by it. */
typedef struct _CommandList {
	int numCommands;
	UInt32 *verbs;
	UInt32 *responses;
} CommandList;

typedef struct _BdlEntry {
	volatile UInt32 addrl;
	volatile UInt32 addrh;
	volatile UInt32 len;
	volatile UInt32 ioc;
} __attribute__((__packed__)) BdlEntry;

#define HDA_MAX_CONNS	32
#define HDA_MAX_NAMELEN	40

#define TRACE_DIR_NONE	0
#define TRACE_DIR_IN	1
#define TRACE_DIR_OUT	2
#define TRACE_DIR_INOUT	3

typedef struct _Widget {
	nid_t nid;
	int type;
	int enable;
	int nconns, selconn, connsenabled;
	int waspin;
	UInt32 pflags;
	int bindAssoc;
	int bindSeqMask;
	int ossdev;
	int sense;
	UInt8 alcNameIdx;
	UInt32 ossmask;
	nid_t conns[HDA_MAX_CONNS];
	UInt8 connsenable[HDA_MAX_CONNS];
	char name[HDA_MAX_NAMELEN];
  uint8_t  *eld;
  int  eld_len;
	FunctionGroup *funcGroup;
	UInt8 traceDir; 
	UInt8 stripecap;
	nid_t favoritDAC;
	struct {
		UInt32 widgetCap;
		UInt32 outAmpCap;
		UInt32 inAmpCap;
		UInt32 supStreamFormats;
		UInt32 supPcmSizeRates;
		UInt32 eapdBtl;
	} params;
	struct {
		UInt32 config;
		UInt32 cap;
		UInt32 ctrl;
	} pin; /* wclass */
} Widget;

typedef struct _AudioControl {
	Widget *widget, *childWidget;
	int enable;
	int index, dir, ndir;
	int mute, step, size, offset;
	int left, right, forcemute;
	UInt32 muted;
	UInt32 ossmask, possmask;
} AudioControl;

typedef struct _NidForSwitch {
	nid_t mainNid;
	nid_t nextNid;
	int connNum;
}NidForSwitch;

/* Association is a group of pins bound for some special function. */
typedef struct _AudioAssoc {
	UInt8 enable;
	UInt8 index;
	UInt8 dir;
	UInt8 pincnt;
	UInt8 fakeredir;
	UInt8 digital;
	UInt16 pinset;
	nid_t hpredir;
	nid_t pins[16];
	nid_t dacs[16];
	nid_t activeNid;
	int chan;
	int dirty;
	//AutumnRain
	NidForSwitch nidForSwitch[16];
	SInt8 defaultPin;
	SInt8 jackPin;
} AudioAssoc;

typedef struct _PcmDevice {
	FunctionGroup *funcGroup;
	int index;
	bool registered;
	int playChanId, recChanId;
	UInt8 left[SOUND_MIXER_NRDEVICES];
	UInt8 right[SOUND_MIXER_NRDEVICES];
	UInt32 chanSize;
	UInt32 chanNumBlocks;
	UInt8 digital;
	UInt32 recDevMask, devMask;
} PcmDevice;

typedef struct _FunctionGroup {
	UInt8 nodeType;
	nid_t nid;
	nid_t startNode, endNode;
	int numNodes;
	bool mSwitchEnable;
	Codec *codec;
	Widget *widgets;
	struct {
		UInt32 outAmpCap;
		UInt32 inAmpCap;
		UInt32 supStreamFormats;
		UInt32 supPcmSizeRates;
		int numControls, numAssocs;
		AudioControl *controls;
		AudioAssoc *assocs;
		UInt32 quirks;
		UInt32 gpio;
		PcmDevice *pcmDevices;
		int numPcmDevices;
	} audio; /* function */
	/* XXX undefined: modem, hdmi. */
} FunctionGroup;

#define HDAC_CHN_RUNNING	0x00000001
#define HDAC_CHN_SUSPEND	0x00000002

typedef struct _ChannelCaps {
	UInt32 minSpeed, maxSpeed;
	UInt32 const *formats;
	UInt32 caps;
	UInt32 channels;
} ChannelCaps;

typedef struct _Channel {
	ChannelCaps caps;
	FunctionGroup *funcGroup;
	PcmDevice *pcmDevice;
	UInt32 speed, format;
	UInt32 formats[12], pcmRates[16];
	UInt32 supStreamFormats, supPcmSizeRates;
	UInt32 numBlocks, blockSize;
	UInt32 *dmaPos;
	UInt32 flags;
	int direction;
	int off;
	int streamId;
	int bit16, bit32;
	int assocNum;
	nid_t io[16]; // adc/dac nids
	UInt8 stripecap;
	UInt8 stripectl;
	//Math
	bool vectorize;
	bool useStereo;
    UInt8 noiseLevel;	
	UInt8 StereoBase;
	UInt16 diagnosticFlags;
	bool diagnosticBufferPrimed;
	UInt8 diagnosticReserved;
	UInt32 diagnosticPhase[8];
	UInt32 diagnosticClipCalls;
	UInt32 diagnosticMixToneFills;
	UInt32 diagnosticDirectToneFills;
	UInt32 diagnosticEraseCalls;
	UInt32 diagnosticEraseSkips;
	UInt32 diagnosticLastFirstFrame;
	UInt32 diagnosticLastNumFrames;
	
	UInt16 slack;
	DmaMemory *bdlMem;
	DmaMemory *buffer;
} Channel;

#define CODEC_ID(codec) ((((UInt32) (codec)->vendorId & 0xffff) << 16) | \
		((UInt32) (codec)->deviceId & 0xffff))

typedef struct _Codec {
	int	numVerbsSent;
	int	numRespReceived;
	nid_t cad;
	UInt16 vendorId;
	UInt16 deviceId;
	UInt8 revisionId;
	UInt8 steppingId;
	/*
	 * PCI device id of the HDA function on the host (AMD GPUs commonly
	 * report codec id 0x1002:0xaa01 from the HDA verb across generations,
	 * while the actual generation is encoded only in the PCI device id of
	 * the HDA function).  AppleGFXHDA dispatches the family by this value,
	 * not by the codec id — see appleGfxHdaAmdCodecFamily() below.
	 */
	UInt16 hdaPciDeviceId;
	CommandList *commands;
	FunctionGroup *funcGroups;
	int	numFuncGroups;
} Codec;

/* ATI HDMI codec: vendor ID 0x1002 (all known ATI/AMD HDMI codec IDs) */
static inline bool isAtiHdmiCodec(Codec *codec) {
	return codec->vendorId == 0x1002;
}

/*
 * AppleGFXHDA does not treat all AMD HDMI codecs as one generic family.
 * The decompiled function-group factory routes the same codec id 0x1002:0xaa01
 * to RS710 / RS730 / RS780 / Park / Broadway / Tahiti subclasses based on the
 * **PCI device id of the HDA function on the AMD GPU**, not on the codec id
 * read from the HDA verb.  Modern AMD GPUs (Tahiti, Polaris, Vega...) report
 * the same 0xaa01 from the codec verb but expose distinct PCI device ids on
 * the audio function (0xaaf0/0xaaf8/0xabf8/0xab2x/0xab38), and Apple maps
 * those to the Tahiti family.  See createAppleHDAFunctionGroup at
 * docs/audio_stack_decompiled/AppleGFXHDA_decompiled.c:48312-48400.
 */
enum AppleGFXHDAAmdCodecFamily {
	kAppleGFXHDAAmdFamilyUnknown = 0,
	kAppleGFXHDAAmdFamilyRS710,
	kAppleGFXHDAAmdFamilyRS730,
	kAppleGFXHDAAmdFamilyRS780,
	kAppleGFXHDAAmdFamilyPark,
	kAppleGFXHDAAmdFamilyBroadway,
	kAppleGFXHDAAmdFamilyTahiti
};

/*
 * Apple-1:1 family dispatch.  Argument is the PCI device id of the HDA
 * function on the AMD GPU (Codec::hdaPciDeviceId, lower 16 bits of mDeviceId).
 *
 * Reproduces AppleGFXHDAFunctionGroupFactory::createAppleHDAFunctionGroup
 * at docs/audio_stack_decompiled/AppleGFXHDA_decompiled.c:48356-48400, with
 * the same range checks and bitmaps verbatim:
 *
 *   if (id < 0xaad8) {
 *     if (id < 0xaa88) {
 *       d = id - 0xaa40;
 *       if (d < 0x29) {
 *         if ((0x10300000000ULL >> (d & 0x3f)) & 1 == 0) -> Park
 *         if ((0x1010000ULL      >> (d & 0x3f)) & 1)     -> Broadway
 *         if (d == 0)                                    -> RS710
 *         // else fall through to RS780/RS730 check
 *       }
 *       if (id == 0xaa30) -> RS780
 *       if (id == 0xaa38) -> RS730
 *     } else {
 *       d = id - 0xaa88;
 *       if (d < 0x39) {
 *         if ((0x10101ULL        >> (d & 0x3f)) & 1)     -> Broadway
 *         if (((0x10001000000ULL >> (d & 0x3f)) & 1) || d == 0x38)
 *                                                        -> Tahiti
 *       }
 *     }
 *   } else if (id < 0xaaf8) {
 *     if (id == 0xaad8 || id == 0xaae0 || id == 0xaaf0)  -> Tahiti
 *   } else {
 *     d = id - 0xab20;
 *     if ((d < 0x19 && ((0x1000101UL >> (d & 0x1f)) & 1)) ||
 *          id == 0xaaf8 || id == 0xabf8)                 -> Tahiti
 *   }
 *   -> Unknown
 */
static inline AppleGFXHDAAmdCodecFamily appleGfxHdaAmdCodecFamily(UInt16 hdaPciDeviceId)
{
	UInt32 id = hdaPciDeviceId;

	if (id < 0xaad8U) {
		if (id < 0xaa88U) {
			UInt32 d = id - 0xaa40U;
			if (d < 0x29U) {
				if (((0x10300000000ULL >> (d & 0x3fU)) & 1U) == 0)
					return kAppleGFXHDAAmdFamilyPark;
				if (((0x1010000ULL >> (d & 0x3fU)) & 1U) != 0)
					return kAppleGFXHDAAmdFamilyBroadway;
				if (d == 0)
					return kAppleGFXHDAAmdFamilyRS710;
				/* fall through to RS780/RS730 check (matches Apple's
				 * `goto LAB_00040f0d` for d != 0 in the carve-out). */
			}
			if (id == 0xaa30U)
				return kAppleGFXHDAAmdFamilyRS780;
			if (id == 0xaa38U)
				return kAppleGFXHDAAmdFamilyRS730;
			return kAppleGFXHDAAmdFamilyUnknown;
		}
		{
			UInt32 d = id - 0xaa88U;
			if (d < 0x39U) {
				if (((0x10101ULL >> (d & 0x3fU)) & 1U) != 0)
					return kAppleGFXHDAAmdFamilyBroadway;
				if (((0x10001000000ULL >> (d & 0x3fU)) & 1U) != 0 || d == 0x38U)
					return kAppleGFXHDAAmdFamilyTahiti;
			}
		}
		return kAppleGFXHDAAmdFamilyUnknown;
	}
	if (id < 0xaaf8U) {
		if (id == 0xaad8U || id == 0xaae0U || id == 0xaaf0U)
			return kAppleGFXHDAAmdFamilyTahiti;
		return kAppleGFXHDAAmdFamilyUnknown;
	}
	{
		UInt32 d = id - 0xab20U;
		if ((d < 0x19U && ((0x1000101UL >> (d & 0x1fU)) & 1U) != 0) ||
		    id == 0xaaf8U || id == 0xabf8U)
			return kAppleGFXHDAAmdFamilyTahiti;
	}
	return kAppleGFXHDAAmdFamilyUnknown;
}

static inline const char *appleGfxHdaAmdCodecFamilyName(UInt16 hdaPciDeviceId)
{
	switch (appleGfxHdaAmdCodecFamily(hdaPciDeviceId)) {
		case kAppleGFXHDAAmdFamilyRS710:
			return "ATI_RS710";
		case kAppleGFXHDAAmdFamilyRS730:
			return "ATI_RS730";
		case kAppleGFXHDAAmdFamilyRS780:
			return "ATI_RS780";
		case kAppleGFXHDAAmdFamilyPark:
			return "ATI_Park";
		case kAppleGFXHDAAmdFamilyBroadway:
			return "ATI_Broadway";
		case kAppleGFXHDAAmdFamilyTahiti:
			return "ATI_Tahiti";
		default:
			return "ATI_Generic";
	}
}

static inline UInt32 appleGfxHdaAmdMemoryDescCoeffForCodec(UInt16 hdaPciDeviceId)
{
	switch (appleGfxHdaAmdCodecFamily(hdaPciDeviceId)) {
		case kAppleGFXHDAAmdFamilyPark:
		case kAppleGFXHDAAmdFamilyTahiti:
			return 0x3000;
		default:
			return 0;
	}
}

static inline bool appleGfxHdaAmdSupportsDisableSlots(UInt16 hdaPciDeviceId)
{
	switch (appleGfxHdaAmdCodecFamily(hdaPciDeviceId)) {
		case kAppleGFXHDAAmdFamilyRS710:
		case kAppleGFXHDAAmdFamilyRS730:
		case kAppleGFXHDAAmdFamilyRS780:
		case kAppleGFXHDAAmdFamilyPark:
			return false;
		default:
			return true;
	}
}

static inline bool appleGfxHdaAmdUsesCachedELDPresence(UInt16 hdaPciDeviceId)
{
	switch (appleGfxHdaAmdCodecFamily(hdaPciDeviceId)) {
		case kAppleGFXHDAAmdFamilyRS710:
		case kAppleGFXHDAAmdFamilyRS730:
		case kAppleGFXHDAAmdFamilyRS780:
		case kAppleGFXHDAAmdFamilyPark:
			return true;
		default:
			return false;
	}
}

/* Rev3+ ATI codecs (0x1002aa01 with revision >= 0x03) support single-channel remap mode */
static inline bool isAtiHdmiRev3(Codec *codec) {
	return (CODEC_ID(codec) == 0x1002aa01) &&
	       (codec->revisionId >= 0x03);
}

#endif
