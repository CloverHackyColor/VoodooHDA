#include "License.h"

#ifndef _SHARED_H
#define _SHARED_H

#ifndef VOODOO_HDA_DEBUG_BUILD
#define VOODOO_HDA_DEBUG_BUILD 1
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
	kVoodooHDAMemoryExtMessageBuffer,
	/* Diagnostic shared memory (persistent, mapped read/write into userland) */
	kVoodooHDAMemoryDiagPCMRing  = 0x4000, /* PCM tap ring buffer */
	kVoodooHDAMemoryDiagSnapshot = 0x4001, /* VHDADiagSnapshot blob (read-only) */
	kVoodooHDAMemoryDiagELD      = 0x4002  /* injected ELD scratch (write before set-eld) */
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
	kVoodooHDAActionGetDiagTelemetry = 0x90,

	/* Diag-mode commands (HDMI crackle root-cause analysis) */
	kVoodooHDAActionDiagPCMTap        = 0xA0, /* args: ch, enable, _ */
	kVoodooHDAActionDiagSnapshot      = 0xA1, /* args: ch, _, _ */
	kVoodooHDAActionDiagForceActive   = 0xA2, /* args: ch, enable, _ */
	kVoodooHDAActionDiagSetELD        = 0xA3, /* args: ch, len_lo, len_hi (blob in DiagELD region) */
	kVoodooHDAActionDiagPCMRingReset  = 0xA4, /* args: ch, _, _ */
	kVoodooHDAActionDiagClearOverride = 0xA5  /* args: ch, _, _  (clear force-active+ELD) */
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
	kVoodooHDADiagForceATIVendorPath = 1U << 13,
	/* Diag-mode chord generator: replaces clipped samples with a sum of pure
	 * sine waves at 1/2/4/8/16 kHz. Spectrum analysis of the captured PCM
	 * shows clean peaks if the audio path is intact. */
	kVoodooHDADiagInjectChord        = 1U << 14
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

/* === Diag-mode shared structures =========================================
 *
 * The PCM ring sits in a fixed-size shared region. Header is at offset 0;
 * sample bytes start at `ring_offset` and span `ring_size` bytes.
 *
 *   head: monotonic byte count of total samples written by kernel.
 *   tail: monotonic byte count consumed by userland.
 *   Available bytes: head - tail (clamped to ring_size).
 *
 * Both sides treat head/tail as 64-bit free-running counters. Position in
 * ring = (counter % ring_size). Wrap is handled by userland reader.
 */

#define kVoodooHDADiagPCMRingMagic    0x52484456U  /* 'VHDR' little-endian */
#define kVoodooHDADiagSnapshotMagic   0x53484456U  /* 'VHDS' little-endian */

#define kVoodooHDADiagPCMRingTotalSize    (16U * 1024U * 1024U) /* 16 MiB */
#define kVoodooHDADiagPCMRingHeaderSize   256U
#define kVoodooHDADiagPCMRingDataSize     (kVoodooHDADiagPCMRingTotalSize - kVoodooHDADiagPCMRingHeaderSize)
#define kVoodooHDADiagSnapshotSize        4096U
#define kVoodooHDADiagELDBufferSize       512U
#define kVoodooHDADiagMaxBdlEntries       16
#define kVoodooHDADiagMaxELDLen           256
#define kVoodooHDADiagMaxDIPLen           64
#define kVoodooHDADiagMaxOverrideChannels 16

typedef struct _VHDAPCMRingHeader {
	UInt32 magic;
	UInt32 version;
	UInt32 ring_offset;          /* offset in mapped region where samples start */
	UInt32 ring_size;             /* bytes of sample area */
	volatile UInt64 head;         /* total bytes written by kernel (monotonic) */
	volatile UInt64 tail;         /* total bytes consumed by userland (monotonic) */
	UInt32 sample_rate;           /* Hz */
	UInt32 channels;              /* PCM channel count */
	UInt32 bits_per_sample;       /* 8, 16, 24, 32 */
	UInt32 frame_bytes;           /* channels * (bits_per_sample/8) */
	volatile UInt32 dropped_bytes;/* bytes lost due to overrun */
	UInt32 enabled;               /* non-zero when tap is producing */
	UInt32 channel_index;         /* engine index that produced these samples */
	UInt32 cad;                   /* codec address */
	UInt32 pin_nid;               /* HDA pin node id */
	UInt32 sd_format;             /* SDFMT register value at start */
	UInt32 reserved[10];
} VHDAPCMRingHeader;

typedef struct _VHDADiagBdlEntry {
	UInt32 addr_lo;
	UInt32 addr_hi;
	UInt32 length;
	UInt32 ioc;
} VHDADiagBdlEntry;

typedef struct _VHDADiagSnapshot {
	UInt32 magic;                 /* kVoodooHDADiagSnapshotMagic */
	UInt32 version;               /* 1 */
	UInt32 size;                  /* sizeof(VHDADiagSnapshot) */
	UInt32 timestamp_ns_lo;
	UInt32 timestamp_ns_hi;

	/* Engine identity */
	UInt32 channel;
	UInt32 cad;
	UInt32 pin_nid;
	UInt32 conv_nid;
	UInt32 digital;               /* PcmDevice digital type */
	UInt32 stream_id;
	UInt32 stream_offset;
	UInt32 num_blocks;
	UInt32 block_size;
	UInt32 buf_size;

	/* Stream descriptor registers (SDn) */
	UInt32 sd_ctl;
	UInt32 sd_sts;
	UInt32 sd_lpib;
	UInt32 sd_dpib;
	UInt32 sd_format;
	UInt32 sd_fifow;
	UInt32 sd_fifod;

	/* Global controller registers */
	UInt32 gctl;
	UInt32 gcap;
	UInt32 outpay;
	UInt32 inpay;
	UInt32 wakeen;
	UInt32 statests;
	UInt32 intctl;
	UInt32 intsts;
	UInt32 corbsts;
	UInt32 rirbsts;
	UInt32 dpiblbase;
	UInt32 dpibubase;

	/* PCI / link */
	UInt32 max_bus_stall_ns;
	UInt32 pcie_link_status;
	UInt32 pci_command;

	/* Codec identity */
	UInt32 codec_vendor;
	UInt32 codec_device;
	UInt32 codec_family;
	UInt32 hda_pci_device_id;

	/* HDMI converter/pin verb readback */
	UInt32 conv_dig_conv_fmt1;
	UInt32 conv_dig_conv_fmt2;
	UInt32 conv_chan_count;
	UInt32 conv_stream_format;
	UInt32 conv_stream_chan;
	UInt32 conv_power_state;
	UInt32 pin_widget_ctrl;
	UInt32 pin_eapd_btl;
	UInt32 pin_power_state;
	UInt32 pin_conn_select;
	UInt32 pin_sense;

	/* Override state */
	UInt32 force_active;
	UInt32 injected_eld_len;

	/* Framebuffer / EDID layer */
	UInt32 fb_present;
	UInt32 fb_eld_valid;
	UInt32 fb_eld_len;
	UInt32 fb_dip_len;

	/* Counts */
	UInt32 bdl_count;
	UInt32 reserved_a[7];

	/* Variable arrays (fixed cap) */
	VHDADiagBdlEntry bdl[kVoodooHDADiagMaxBdlEntries];
	UInt8  eld_blob[kVoodooHDADiagMaxELDLen];
	UInt8  dip_infoframe[kVoodooHDADiagMaxDIPLen];
	UInt8  injected_eld[kVoodooHDADiagMaxELDLen];

	UInt8  reserved_b[256];
} VHDADiagSnapshot;

#endif
