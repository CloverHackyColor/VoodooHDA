#include "License.h"

#ifndef _VOODOO_HDA_FB_NOTIFIER_H
#define _VOODOO_HDA_FB_NOTIFIER_H

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IONotifier.h>
#include <IOKit/IOLocks.h>
#include <IOKit/pci/IOPCIDevice.h>
/*
 * Include IOFramebuffer.h for the class declaration only.
 * Virtual method calls (setAttributeForConnection) dispatch through the
 * vtable at runtime and do NOT require linking against IOGraphicsFamily.
 * This also provides kIODisplayEDIDKey, kConnectionEnableAudio, etc.
 */
#include <IOKit/graphics/IOFramebuffer.h>

#include "Private.h"

#define VHDA_FB_MAX_CONNECTIONS 6
#define VHDA_FB_MAX_PINS       16
#define VHDA_FB_MAX_SADS       15

enum VoodooHDAAMDGPUFamily {
  kVoodooHDAAMDGPUUnknown = 0,
  kVoodooHDAAMDGPUClassicPolaris,
  kVoodooHDAAMDGPUVega,
  kVoodooHDAAMDGPUVega20RadeonVII,
  kVoodooHDAAMDGPUModernNavi,
  kVoodooHDAAMDGPUGenericAMD
};

/* ========================================================================
 * GPU MMIO direct register access for AZ (Azalia) audio engine
 *
 * On AMD GPUs, the HDA codec's DIP buffers and audio engine are controlled
 * by GPU-side AZ registers in GPU BAR5 MMIO space.  Without programming
 * these registers, DIP_SIZE returns 0 and no audio reaches the display.
 *
 * Register offsets differ between GPU generations:
 *   - Polaris (DCE 11.x): absolute dword offsets, from dce_11_0_d.h
 *   - Vega   (DCE 12.0):  segment-relative, from dce_12_0_offset.h
 * ======================================================================== */

/* Convert dword offset to byte offset */
#define DW2B(dw)  ((dw) * 4)

/* AZ indirect register indices (written to ENDPOINT_INDEX — same for all DCE) */
#define AZ_REG_PIN_CONTROL_CHANNEL_SPEAKER           0x25
#define AZ_REG_PIN_CONTROL_AUDIO_DESCRIPTOR(n)      (0x28 + (n))
#define AZ_REG_PIN_CONTROL_MULTICHANNEL_ENABLE       0x36
#define AZ_REG_PIN_CONTROL_RESPONSE_HBR              0x38
#define AZ_REG_PIN_CONTROL_HOT_PLUG_CONTROL          0x54
#define AZ_REG_PIN_CONTROL_MULTICHANNEL_MODE         0x58

/* HOT_PLUG_CONTROL bits */
#define AZ_HPC_CLOCK_GATING_DISABLE  (1u << 0)
#define AZ_HPC_AUDIO_ENABLED         (1u << 31)

/* CHANNEL_SPEAKER bits */
#define AZ_CS_SPEAKER_ALLOC_MASK     0x0000007Fu
#define AZ_CS_CHANNEL_ALLOC_MASK     0x0000FF00u
#define AZ_CS_CHANNEL_ALLOC_SHIFT    8
#define AZ_CS_HDMI_CONNECTION        (1u << 16)
#define AZ_CS_DP_CONNECTION          (1u << 17)

/* DP_SEC_CNTL bits */
#define DP_SEC_STREAM_ENABLE  (1u << 0)
#define DP_SEC_ASP_ENABLE     (1u << 4)
#define DP_SEC_ATP_ENABLE     (1u << 8)
#define DP_SEC_AIP_ENABLE     (1u << 12)
#define DP_SEC_ACM_ENABLE     (1u << 16)

/* AFMT bits */
#define AFMT_AUDIO_SAMPLE_SEND   (1u << 0)
#define AFMT_AUDIO_CLOCK_EN      (1u << 0)

/*
 * Per-GPU register offset tables.
 * All values are BYTE offsets into GPU BAR5 MMIO.
 */
struct AZRegOffsets {
  /* AZ endpoint indirect registers — INDEX/DATA pairs, per-endpoint */
  uint32_t azEpIndex0;    /* endpoint 0 INDEX byte offset */
  uint32_t azEpData0;     /* endpoint 0 DATA byte offset */
  uint32_t azEpStride;    /* byte stride between endpoints */
  
  /* AFMT per-DIG encoder */
  uint32_t afmtPktCtl0;   /* AFMT_AUDIO_PACKET_CONTROL DIG0 */
  uint32_t afmtPktCtl2_0; /* AFMT_AUDIO_PACKET_CONTROL2 DIG0 */
  uint32_t afmtSrcCtl0;   /* AFMT_AUDIO_SRC_CONTROL DIG0 */
  uint32_t afmtCntl0;     /* AFMT_CNTL DIG0 */
  uint32_t digStride;     /* byte stride between DIGs */
  
  /* DP secondary packet control per-DIG */
  uint32_t dpSecCntl0;    /* DP_SEC_CNTL DIG0/DP0 */
  uint32_t dpSecAudN0;    /* DP_SEC_AUD_N DIG0/DP0 */
  uint32_t dpSecTimestamp0; /* DP_SEC_TIMESTAMP DIG0/DP0 */
  
  /* DIG encoder status (per-DIG, same stride as AFMT) */
  uint32_t digFeCntl0;     /* DIG_FE_CNTL DIG0: bit10=DIG_START, [2:0]=SOURCE_SELECT */
  uint32_t digBeCntl0;     /* DIG_BE_CNTL DIG0: [18:16]=DIG_MODE (0=DP,3=HDMI) */
  uint32_t digBeEnCntl0;   /* DIG_BE_EN_CNTL DIG0: bit0=DIG_ENABLE */
  uint32_t dpLinkCntl0;    /* DP_LINK_CNTL DIG0: bit4=TRAINING_COMPLETE, bit8=LINK_STATUS */
  
  /* DCCG audio DTO */
  uint32_t dccgDtoSource;
  uint32_t dccgDto0Phase;
  uint32_t dccgDto0Module;
  uint32_t dccgDto1Phase;
  uint32_t dccgDto1Module;
};

/*
 * Polaris (DCE 11.0 / 11.2) — RX 470/480/570/580/590, Ellesmere, Baffin
 * Absolute dword offsets from dce_11_0_d.h / dce_11_2_d.h
 */
static const AZRegOffsets kPolarisRegs = {
  .azEpIndex0     = DW2B(0x17A8),
  .azEpData0      = DW2B(0x17A9),
  .azEpStride     = DW2B(4),       /* 4 dwords between endpoints */
  
  .afmtPktCtl0    = DW2B(0x4A42),
  .afmtPktCtl2_0  = DW2B(0x4A14),
  .afmtSrcCtl0    = DW2B(0x4A45),
  .afmtCntl0      = DW2B(0x4A7E),
  .digStride      = DW2B(0x100),
  
  .dpSecCntl0     = DW2B(0x4AC3),
  .dpSecAudN0     = DW2B(0x4AC9),
  .dpSecTimestamp0 = DW2B(0x4ACD),
  
  .digFeCntl0     = DW2B(0x4A00),
  .digBeCntl0     = DW2B(0x4A47),
  .digBeEnCntl0   = DW2B(0x4A48),
  .dpLinkCntl0    = DW2B(0x4AA0),
  
  .dccgDtoSource  = DW2B(0x016B),
  .dccgDto0Phase  = DW2B(0x016C),
  .dccgDto0Module = DW2B(0x016D),
  .dccgDto1Phase  = DW2B(0x016E),
  .dccgDto1Module = DW2B(0x016F),
};

/*
 * Vega 10 (DCE 12.0) — RX Vega 56/64
 * SOC15: segment base + relative offset.  Seg1=0xC0, Seg2=0x34C0
 */
static const AZRegOffsets kVega10Regs = {
  .azEpIndex0     = DW2B(0x34C0 + 0x0480),
  .azEpData0      = DW2B(0x34C0 + 0x0481),
  .azEpStride     = DW2B(6),
  
  .afmtPktCtl0    = DW2B(0x34C0 + 0x18C0),
  .afmtPktCtl2_0  = DW2B(0x34C0 + 0x1892),
  .afmtSrcCtl0    = DW2B(0x34C0 + 0x18C3),
  .afmtCntl0      = DW2B(0x34C0 + 0x18FC),
  .digStride      = DW2B(0x100),
  
  .dpSecCntl0     = DW2B(0x34C0 + 0x1941),
  .dpSecAudN0     = DW2B(0x34C0 + 0x1947),
  .dpSecTimestamp0 = DW2B(0x34C0 + 0x194B),
  
  .digFeCntl0     = DW2B(0x34C0 + 0x1880),
  .digBeCntl0     = DW2B(0x34C0 + 0x18C7),
  .digBeEnCntl0   = DW2B(0x34C0 + 0x18C8),
  .dpLinkCntl0    = DW2B(0x34C0 + 0x1920),
  
  .dccgDtoSource  = DW2B(0x00C0 + 0x00AB),
  .dccgDto0Phase  = DW2B(0x00C0 + 0x00AC),
  .dccgDto0Module = DW2B(0x00C0 + 0x00AD),
  .dccgDto1Phase  = DW2B(0x00C0 + 0x00AE),
  .dccgDto1Module = DW2B(0x00C0 + 0x00AF),
};



class VoodooHDADevice;

/* Per-framebuffer connection tracking */
struct FBConnectionState {
	IOService *framebuffer;
	IONotifier    *fbNotifier;
	bool           displayOnline;
	bool           edidValid;
	bool           audioPipeEnabled;
  bool           isDP;
	nid_t          mappedPinNid;
	int            mappedCodecCad;
	uint8_t       *edidData;
	int            edidLen;
	/* Parsed from EDID CEA extension */
	uint8_t        speakerAllocation;
	uint8_t        sads[VHDA_FB_MAX_SADS * 3];
	int            numSADs;
	/* Constructed ELD */
	uint8_t       *eld;
	int            eldLen;
};

class VoodooHDAFramebufferNotifier : public OSObject {
	OSDeclareDefaultStructors(VoodooHDAFramebufferNotifier)
public:
	static VoodooHDAFramebufferNotifier *withDevice(VoodooHDADevice *device);
	virtual void free() APPLE_KEXT_OVERRIDE;

	void startMatching();
	void stopMatching();

	/* Called by VoodooHDADevice when ATI codec pins are known */
	void registerATIPins(int cad, nid_t *pinNids, int count);

	/* Query framebuffer ELD for a given pin */
	bool getFramebufferELD(int cad, nid_t pinNid, uint8_t **outELD, int *outLen);

	/* Ensure audio pipe is active for a pin (called at stream start) */
	void ensureAudioPipeEnabled(int cad, nid_t pinNid);

	/* Retry enableAudioPipe on all framebuffers (called from unsolicited response) */
	void retryEnableAudioPipeAll();

	/* Notify GPU that HDMI audio streaming started/stopped */
	void notifyStreamingState(int cad, nid_t pinNid, bool streaming);
  
  /* Pick the HDMI pin that is actually backed by an online IOFramebuffer/IODisplay. */
  bool getPreferredConnectedPin(int cad, const nid_t *pinNids, int pinCount, nid_t *outPin);
  
  /* Runtime AMD GPU family detected from the framebuffer GPU PCI function, not the HDA audio function. */
  bool detectedAMDGPUFamily(VoodooHDAAMDGPUFamily *outFamily, UInt16 *outDeviceId);

  const char *detectedAMDGPUFamilyName();

private:
	bool init(VoodooHDADevice *device);

	VoodooHDADevice        *mDevice;
	IONotifier             *mGFXMatchNotifier;
	IONotifier             *mGFXTermNotifier;
	IONotifier             *mDisplayMatchNotifier;
	FBConnectionState       mConnections[VHDA_FB_MAX_CONNECTIONS];
	int                     mNumConnections;
	IOLock                 *mLock;

	/* ATI pin NIDs to map */
	nid_t                   mATIPinNids[VHDA_FB_MAX_PINS];
	int                     mATIPinCad;
	int                     mATIPinCount;

	/* IOPCIDevice of the HDA controller — for PCI topology matching */
	IOPCIDevice            *mHDAPciDevice;

	/* GPU MMIO direct register access for AZ (Azalia) audio engine */
	IOPCIDevice            *mGPUPciDevice;
	IOMemoryMap            *mGPUMMIOMap;
	volatile uint32_t      *mGPUMMIO;
	uint32_t                mGPUMMIOSize;
	bool                    mGPUAudioInitDone;
	const void             *mRegs;  /* -> AZRegOffsets, selected per GPU */
  UInt16                  mAMDGPUDeviceId;
  VoodooHDAAMDGPUFamily   mAMDGPUFamily;

	bool mapGPUMMIO();
	void unmapGPUMMIO();
	uint32_t gpuRead32(uint32_t byteOffset);
	void gpuWrite32(uint32_t byteOffset, uint32_t value);
	uint32_t azEndpointRead(int endpoint, uint32_t index);
	void azEndpointWrite(int endpoint, uint32_t index, uint32_t value);
	void dumpAZState();
	bool enableGPUAudioEngine(int endpoint, int digIndex, bool isDP,
	                          uint8_t speakerAlloc, int numChannels);
public:
	void initGPUAudioIfNeeded();
private:

	/* IOService matching callbacks */
	static bool gfxMatchedHandler(void *target, void *refCon,
	                               IOService *newService, IONotifier *notifier);
	static bool gfxTerminatedHandler(void *target, void *refCon,
	                                  IOService *service, IONotifier *notifier);
	static bool displayMatchedHandler(void *target, void *refCon,
	                                   IOService *newService, IONotifier *notifier);

	/* IOService interest notification (no IOGraphicsFamily dependency) */
	static IOReturn interestHandler(void *target, void *refCon,
	                                 UInt32 messageType, IOService *provider,
	                                 void *messageArgument, vm_size_t argSize);

	/* Internal helpers */
	void handleFramebufferMatched(IOService *fb);
	void handleFramebufferTerminated(IOService *fb);

	bool isSameGPU(IOService *fbService);
	bool readEDID(FBConnectionState *conn);
	bool parseEDIDAudio(FBConnectionState *conn);
	void buildELDFromEDID(FBConnectionState *conn);
	bool enableAudioPipe(FBConnectionState *conn);
	void disableAudioPipe(FBConnectionState *conn);
	void injectELDIntoWidget(FBConnectionState *conn);
	void injectELDIntoAllPinsWithPresence(FBConnectionState *conn);
public:
	void injectELDIntoPinIfReady(int cad, nid_t pinNid);
  void disableAudioPipeForPin(int cad, nid_t pinNid);
private:
	void clearWidgetELD(FBConnectionState *conn);

	FBConnectionState *findConnection(IOService *fb);
	FBConnectionState *findConnectionForPin(int cad, nid_t pinNid);
	void mapConnectionToPin(FBConnectionState *conn, int connIndex);
};

#endif
