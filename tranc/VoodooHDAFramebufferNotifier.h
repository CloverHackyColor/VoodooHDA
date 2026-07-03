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

class VoodooHDADevice;

/* Per-framebuffer connection tracking */
struct FBConnectionState {
	IOService *framebuffer;
	IONotifier    *fbNotifier;
	bool           displayOnline;
	bool           edidValid;
	bool           audioPipeEnabled;
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
private:
	void clearWidgetELD(FBConnectionState *conn);

	FBConnectionState *findConnection(IOService *fb);
	FBConnectionState *findConnectionForPin(int cad, nid_t pinNid);
	void mapConnectionToPin(FBConnectionState *conn, int connIndex);
};

#endif
