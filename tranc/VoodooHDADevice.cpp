#include "License.h"

#include "GitCommit.h"
#include "VoodooHDADevice.h"
#include "VoodooHDAFramebufferNotifier.h"
#include "VoodooGFXHDA.h"
#include "VoodooHDAEngine.h"
#include "Tables.h"
#include "Models.h"
#include "Common.h"
#include "Verbs.h"
#include "OssCompat.h"
#include "AppleALCPinConfigs.h"

#include "Shared.h"

#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/pci/IOPCIDevice.h>

#include <kern/locks.h>
#include <pexpert/pexpert.h>

#ifdef TIGER
#include "TigerAdditionals.h"
#endif

#define HDAC_REVISION "20120126_0002"

#define LOCK()		lock(__FUNCTION__)
#define UNLOCK()	unlock(__FUNCTION__)

#define super IOAudioDevice
OSDefineMetaClassAndStructors(VoodooHDADevice, IOAudioDevice)

	//#if __LP64__
#define MSG_BUFFER_SIZE 262140
	//#else
	//#define MSG_BUFFER_SIZE 65535
	//#endif

#define kVoodooHDAVerboseLevelKey "VoodooHDAVerboseLevel"

static const UInt32 kVoodooHDATimerIdleIntervalMs = 5000;
static const int kVoodooHDAPrefPanelMaxChannels = 24;

/*
 * Analog speaker protection curve. Keep the macOS slider range intact, but use
 * a linear 85% hardware-amp ceiling for analog outputs. The 0-100% slider is
 * divided into 10% points and interpolated between them, keeping normal volume
 * progression without letting the analog amp reach full scale. HDMI/DP and
 * input/capture paths are intentionally excluded.
 */
static const int kVoodooHDAAnalogOutputScaleTable[] = {
	0, 9, 17, 26, 34, 43, 51, 60, 68, 77, 85
};

static inline int VoodooHDAAnalogOutputScalePercent(int percent)
{
	if (percent <= 0)
		return 0;
	if (percent > 100)
		percent = 100;

	/*
	 * Speaker-safe analog protection - normalized hardware amp ceiling.
	 * Mapping: 10->9, 20->17, 30->26, 40->34, 50->43, 60->51,
	 * 70->60, 80->68, 90->77, 100->85.
	 */
	int bucket = percent / 10;
	int remainder = percent % 10;
	int lastBucket = (int)(sizeof(kVoodooHDAAnalogOutputScaleTable) /
	    sizeof(kVoodooHDAAnalogOutputScaleTable[0])) - 1;

	if (bucket >= lastBucket)
		return kVoodooHDAAnalogOutputScaleTable[lastBucket];

	int low = kVoodooHDAAnalogOutputScaleTable[bucket];
	int high = kVoodooHDAAnalogOutputScaleTable[bucket + 1];
	return low + (((high - low) * remainder) + 5) / 10;
}

static inline int VoodooHDAAnalogOutputStepForPercent(AudioControl *control, int percent)
{
	if (!control || control->step <= 0)
		return 0;
	return (VoodooHDAAnalogOutputScalePercent(percent) * control->step + 50) / 100;
}

static bool VoodooHDAReadLayoutIdProperty(OSObject *property, UInt32 *layoutId)
{
	if (!property || !layoutId)
		return false;

	OSData *layoutData = OSDynamicCast(OSData, property);
	if (layoutData && layoutData->getLength() >= sizeof(UInt32)) {
		bcopy(layoutData->getBytesNoCopy(), layoutId, sizeof(*layoutId));
		return *layoutId != 0;
	}

	OSNumber *layoutNum = OSDynamicCast(OSNumber, property);
	if (layoutNum) {
		*layoutId = layoutNum->unsigned32BitValue();
		return *layoutId != 0;
	}

	return false;
}

static bool VoodooHDAAppleALCHasLayoutId(UInt32 layoutId)
{
	if (layoutId == 0)
		return false;

	for (UInt32 i = 0; i < ALC_CONFIG_ENTRIES_COUNT; i++) {
		if (gALCConfigEntries[i].layoutId == layoutId)
			return true;
	}
	return false;
}

static bool VoodooHDAParseALCIDBootArg(UInt32 *layoutId)
{
	if (!layoutId)
		return false;

	UInt32 bootLayoutId = 0;
	if (!PE_parse_boot_argn("alcid", &bootLayoutId, sizeof(bootLayoutId)))
		return false;
	if (bootLayoutId == 0)
		return false;
	if (!VoodooHDAAppleALCHasLayoutId(bootLayoutId)) {
		IOLog("VoodooHDA DBG: ignored alcid boot-arg layout=%u; no embedded AppleALC layout match\n",
		      (unsigned)bootLayoutId);
		return false;
	}

	*layoutId = bootLayoutId;
	return true;
}

// cue8chalk: added to allow for the volume change fix to be controlled from the plist
#define kVoodooHDAEnableVolumeChangeFixKey "VoodooHDAEnableVolumeChangeFix"
#define kVoodooHDAEnableHalfVolumeFixKey "VoodooHDAEnableHalfVolumeFix"
// VertexBZ: added to allow for the Mic and Mute fixes to be controlled from the plist
#define kVoodooHDAEnableHalfMicVolumeFixKey "VoodooHDAEnableHalfMicVolumeFix"
#define kVoodooHDAEnableMuteFixKey "VoodooHDAEnableMuteFix"
#define kVoodooHDAEnableAnalogPathRestoreKey "VoodooHDAEnableAnalogPathRestore"
#define kVoodooHDAEnableAnalogAutoUnmuteKey "VoodooHDAEnableAnalogAutoUnmute"
#define kVoodooHDAEnableAnalogEAPDRestoreKey "VoodooHDAEnableAnalogEAPDRestore"
#define kVoodooHDAEnableAnalogDirectMasterKey "VoodooHDAEnableAnalogDirectMaster"
#define kVoodooHDAAllowMSI "AllowMSI"
#define kVoodooHDAAMDHDMIDynamicMatch "VoodooHDAAMDHDMIDynamicMatch"
#define kVoodooHDAEnableAMDHDMI "VoodooHDAEnableAMDHDMI"
#define kDisableInputMonitor "DisableInputMonitor"
#define kVoodooHDAEnableAMDFramebufferNotifier "VoodooHDAEnableAMDFramebufferNotifier"

/*
 * AMD HDMI controller policy.
 *
 * Matching is dynamic: VoodooHDA-HDMI attaches to AMD/ATI PCI HDA functions
 * by vendor 0x1002 + class 0x0403, then the runtime HDMI policy uses the
 * *framebuffer GPU function* to decide the safest connector strategy.  Do not
 * rely on marketing names or a short HDMI-audio IOPCIMatch list.
 *
 * Target families:
 *   - Polaris RX4xx/RX5xx: RX460, RX470, RX480, RX550, RX560, RX570, RX580
 *   - Vega: Vega56, Vega64, AMD Radeon VII/Vega20
 *   - Navi/RDNA1: RX5500/XT, RX5600/XT, RX5700/XT
 *   - Navi/RDNA2: RX6600/XT, RX6650/XT, RX6700/XT, RX6750/XT, RX6800/XT,
 *                 RX6900/XT, RX6950/XT
 *
 * Polaris boards are the risky legacy group: they often report stale/cached
 * ELD on several HDA pins and may black-screen if the framebuffer audio pipe
 * is touched too early or disabled during runtime.  Do not trust one guessed
 * physical pin on this group: keep one CoreAudio output visible, but mirror the
 * stream setup to the other candidate pins so the real route is not hidden.
 *
 * Vega/Radeon VII/RDNA1/RDNA2 use the EDID/framebuffer-driven path.  The RX6600
 * path is known-good and must be preserved.
 */
static inline bool isAmdLegacyPolarisHdaController(UInt32 packedControllerId)
{
	UInt16 vendor = packedControllerId & 0xffff;
	UInt16 device = (packedControllerId >> 16) & 0xffff;
	if (vendor != ATI_VENDORID)
		return false;
	switch (device) {
		case 0xaae0: /* Baffin/Polaris-era HDMI/DP audio */
		case 0xab00: /* Baffin/Polaris alternate HDMI/DP audio */
		case 0xaaf0: /* Ellesmere/Polaris HDMI/DP audio */
		case 0xaaf8: /* Ellesmere/Polaris alternate HDMI/DP audio */
			return true;
		default:
			return false;
	}
}

static inline VoodooHDAAMDGPUFamily classifyAmdGpuDeviceForHDAPolicy(UInt16 gpuDeviceId)
{
	if ((gpuDeviceId & 0xFF00) == 0x6700 ||
	    (gpuDeviceId & 0xFF00) == 0x6F00 ||
	    (gpuDeviceId & 0xFFF0) == 0x6980 ||
	    (gpuDeviceId & 0xFFF0) == 0x6990)
		return kVoodooHDAAMDGPUClassicPolaris;

	if ((gpuDeviceId & 0xFFF0) == 0x6860 ||
	    (gpuDeviceId & 0xFFF0) == 0x6870 ||
	    (gpuDeviceId & 0xFFF0) == 0x69A0)
		return kVoodooHDAAMDGPUVega;

	if ((gpuDeviceId & 0xFFF0) == 0x66A0)
		return kVoodooHDAAMDGPUVega20RadeonVII;

	if ((gpuDeviceId & 0xFF00) == 0x7300 ||
	    (gpuDeviceId & 0xFF00) == 0x7400)
		return kVoodooHDAAMDGPUModernNavi;

	return kVoodooHDAAMDGPUGenericAMD;
}

static inline const char *amdGpuFamilyNameForHDAPolicy(VoodooHDAAMDGPUFamily family)
{
	switch (family) {
		case kVoodooHDAAMDGPUClassicPolaris: return "Polaris RX4xx/RX5xx";
		case kVoodooHDAAMDGPUVega: return "Vega56/Vega64";
		case kVoodooHDAAMDGPUVega20RadeonVII: return "AMD Radeon VII / Vega20";
		case kVoodooHDAAMDGPUModernNavi: return "Navi/RDNA RX5xxx/RX6xxx";
		case kVoodooHDAAMDGPUGenericAMD: return "Generic AMD HDMI";
		default: return "Unknown AMD HDMI";
	}
}

static bool detectSiblingAmdGpuForHDAPolicy(IOPCIDevice *hdaDevice,
                                            VoodooHDAAMDGPUFamily *outFamily,
                                            UInt16 *outDeviceId)
{
	if (outFamily)
		*outFamily = kVoodooHDAAMDGPUUnknown;
	if (outDeviceId)
		*outDeviceId = 0;
	if (!hdaDevice)
		return false;

	IOService *parent = hdaDevice->getProvider();
	if (!parent)
		return false;

	OSIterator *iter = parent->getChildIterator(gIOServicePlane);
	if (!iter)
		return false;

	IOService *child;
	while ((child = OSDynamicCast(IOService, iter->getNextObject()))) {
		IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, child);
		if (!pci || pci == hdaDevice)
			continue;
		if (pci->configRead16(kIOPCIConfigVendorID) != ATI_VENDORID)
			continue;
		if (pci->getFunctionNumber() != 0)
			continue;

		UInt16 gpuDeviceId = pci->configRead16(kIOPCIConfigDeviceID);
		VoodooHDAAMDGPUFamily family = classifyAmdGpuDeviceForHDAPolicy(gpuDeviceId);
		if (outFamily)
			*outFamily = family;
		if (outDeviceId)
			*outDeviceId = gpuDeviceId;
		iter->release();
		return true;
	}

	iter->release();
	return false;
}

static bool shouldStartAmdFramebufferNotifierForController(IOPCIDevice *hdaDevice,
                                                           UInt32 packedControllerId,
                                                           VoodooHDAAMDGPUFamily *outFamily,
                                                           UInt16 *outGpuDeviceId,
                                                           bool *outUsedHDAFallback)
{
	if (outFamily)
		*outFamily = kVoodooHDAAMDGPUUnknown;
	if (outGpuDeviceId)
		*outGpuDeviceId = 0;
	if (outUsedHDAFallback)
		*outUsedHDAFallback = false;

	VoodooHDAAMDGPUFamily family = kVoodooHDAAMDGPUUnknown;
	UInt16 gpuDeviceId = 0;
	if (detectSiblingAmdGpuForHDAPolicy(hdaDevice, &family, &gpuDeviceId)) {
		if (outFamily)
			*outFamily = family;
		if (outGpuDeviceId)
			*outGpuDeviceId = gpuDeviceId;
		return true;
	}

	UInt16 hdaVendor = packedControllerId & 0xffff;
	if (hdaVendor == ATI_VENDORID) {
		if (outFamily)
			*outFamily = isAmdLegacyPolarisHdaController(packedControllerId) ?
			    kVoodooHDAAMDGPUClassicPolaris : kVoodooHDAAMDGPUGenericAMD;
		if (outUsedHDAFallback)
			*outUsedHDAFallback = true;
		return true;
	}

	return false;
}

static inline bool shouldUseAmdLegacyPolarisHDMIFallback(VoodooHDAFramebufferNotifier *notifier,
                                                         IOPCIDevice *hdaDevice,
                                                         UInt32 packedControllerId)
{
	/* Prefer runtime GPU-family detection from the framebuffer GPU PCI function.
	 * This is the only reliable way to distinguish Polaris from Vega/RDNA when
	 * AMD reuses or board vendors vary the separate HDA audio function ID. */
	if (notifier) {
		VoodooHDAAMDGPUFamily family = kVoodooHDAAMDGPUUnknown;
		UInt16 gpuDeviceId = 0;
		if (notifier->detectedAMDGPUFamily(&family, &gpuDeviceId))
			return family == kVoodooHDAAMDGPUClassicPolaris;
	}

	VoodooHDAAMDGPUFamily family = kVoodooHDAAMDGPUUnknown;
	UInt16 gpuDeviceId = 0;
	if (detectSiblingAmdGpuForHDAPolicy(hdaDevice, &family, &gpuDeviceId))
		return family == kVoodooHDAAMDGPUClassicPolaris;

	/* Early-boot fallback before any framebuffer/IODisplay has matched.  Keep this
	 * deliberately narrow so Vega/Radeon VII/RDNA cards stay on the modern
	 * EDID-driven path. */
	return isAmdLegacyPolarisHdaController(packedControllerId);
}

static const char *setHDMIEngineDisplayName(VoodooHDADevice::HDMIEngineSlot *slot, bool includePin)
{
	if (!slot || !slot->engine)
		return "VoodooHDA HDMI/DP Audio";

	if (includePin && slot->pinNid >= 0) {
		snprintf(slot->engine->mPortNameBuf, sizeof(slot->engine->mPortNameBuf),
		         "VoodooHDA HDMI/DP Audio P%d", slot->pinNid);
		slot->engine->mPortName = slot->engine->mPortNameBuf;
	} else {
		slot->engine->mPortName = "VoodooHDA HDMI/DP Audio";
	}
	slot->engine->mPortType = kIOAudioSelectorControlSelectionValueExternalSpeaker;
	return slot->engine->mPortName;
}


bool VoodooHDADevice::init(OSDictionary *dict)
{
	OSNumber *verboseLevelNum;
	OSBoolean *osBool;
	extern kmod_info_t kmod_info;
	mVerbose = 0;
	mFBNotifier = NULL;
	mGFXController = NULL;
	mNumHDMIEngines = 0;
	bzero(mHDMIEngines, sizeof(mHDMIEngines));
	if (!super::init(dict)) {
		return false;
	}

	dumpMsg("Loading VoodooHDA %s commit " VOODOO_HDA_GIT_COMMIT " (based on hdac version " HDAC_REVISION ")\n",
	        kmod_info.version);
	
//	ASSERT(dict);
	verboseLevelNum = OSDynamicCast(OSNumber, dict->getObject(kVoodooHDAVerboseLevelKey));
#if VOODOO_HDA_DEBUG_BUILD
	if (verboseLevelNum)
		mVerbose = verboseLevelNum->unsigned32BitValue();
	else
		mVerbose = 0;
#else
	(void)verboseLevelNum;
	mVerbose = 0;
#endif

	mMessageLock = IOLockAlloc();
	if (!mMessageLock) {
		IOLog("VoodooHDA: failed to allocate message lock\n");
		return false;
	}

	logMsg("VoodooHDADevice[%p]::init\n", this);

	// cue8chalk: read flag for volume change fix
	// TODO - when VoodooHDA properly supports multiple devices (at least on my system - lol)
	// make this a per-device setting (tied to vid/did)
	osBool = OSDynamicCast(OSBoolean, dict->getObject(kVoodooHDAEnableVolumeChangeFixKey));
	if (osBool) {
		mEnableVolumeChangeFix = (bool)osBool->getValue();
	} else {
		mEnableVolumeChangeFix = false;
	}
	
	// Half volume slider fix
	osBool = OSDynamicCast(OSBoolean, dict->getObject(kVoodooHDAEnableHalfVolumeFixKey));
	if (osBool) {
		mEnableHalfVolumeFix = (bool)osBool->getValue();
	} else {
		mEnableHalfVolumeFix = false;
	}
    
    // VertexBZ: Half Mic volume slider fix
    osBool = OSDynamicCast(OSBoolean, dict->getObject(kVoodooHDAEnableHalfMicVolumeFixKey));
	if (osBool) {
		mEnableHalfMicVolumeFix = (bool)osBool->getValue();
	} else {
		mEnableHalfMicVolumeFix = false;
	}
    
    // VertexBZ: Mute fix
    osBool = OSDynamicCast(OSBoolean, dict->getObject(kVoodooHDAEnableMuteFixKey));
	if (osBool) {
		mEnableMuteFix = (bool)osBool->getValue();
	} else {
		mEnableMuteFix = false;
	} 

	osBool = OSDynamicCast(OSBoolean, dict->getObject(kVoodooHDAEnableAnalogPathRestoreKey));
	mEnableAnalogPathRestore = osBool ? osBool->isTrue() : true;

	osBool = OSDynamicCast(OSBoolean, dict->getObject(kVoodooHDAEnableAnalogAutoUnmuteKey));
	mEnableAnalogAutoUnmute = osBool ? osBool->isTrue() : false;

	osBool = OSDynamicCast(OSBoolean, dict->getObject(kVoodooHDAEnableAnalogEAPDRestoreKey));
	mEnableAnalogEAPDRestore = osBool ? osBool->isTrue() : false;

	osBool = OSDynamicCast(OSBoolean, dict->getObject(kVoodooHDAEnableAnalogDirectMasterKey));
	mEnableAnalogDirectMaster = osBool ? osBool->isTrue() : false;

	//Slice - some chipsets needed Inhibit Cache
	osBool = OSDynamicCast(OSBoolean, dict->getObject("InhibitCache"));
	if (osBool) {
		mInhibitCache = (bool)osBool->getValue();
	} else {
		mInhibitCache = false;
	}
  
  //DisableInputMonitor
	osBool = OSDynamicCast(OSBoolean, dict->getObject("DisableInputMonitor"));
	if (osBool) {
		mDisableInputMonitor = (bool)osBool->getValue();
	} else {
		mDisableInputMonitor = false;
	}
  

	osBool = OSDynamicCast(OSBoolean, dict->getObject("Vectorize"));
	if (osBool) {
		vectorize = (bool)osBool->getValue();
	} else {
		/* Final Vectorize build: keep the historical accelerated PCM blitter
		 * default enabled when the plist key is absent. */
		vectorize = true;
	}

	verboseLevelNum = OSDynamicCast(OSNumber, dict->getObject("Noise"));
	if (verboseLevelNum)
		noiseLevel = verboseLevelNum->unsigned32BitValue();
	else
		noiseLevel = 0;

	verboseLevelNum = OSDynamicCast(OSNumber, dict->getObject("Boost"));
	if (verboseLevelNum)
		Boost = verboseLevelNum->unsigned32BitValue();
	else
		Boost = 0;

	osBool = OSDynamicCast(OSBoolean, dict->getObject(kVoodooHDAAllowMSI));
	mAllowMSI = (osBool ? osBool->isTrue() : true);

	mLock = IOLockAlloc();
	if (!mLock) {
		errorMsg("error: failed to allocate main device lock\n");
		return false;
	}

	mUnsolqState = HDAC_UNSOLQ_READY;

	mActionHandler = (IOCommandGate::Action) &VoodooHDADevice::handleAction;
	if (!mActionHandler) {
		if (mVerbose >= 1)
			IOLog("VoodooHDA DBG: mActionHandler is NULL\n");
		errorMsg("error: couldn't cast command gate action handler\n");
		return false;
	}

	nSliderTabsCount = 0;
	mPrefPanelMemoryBufSize = 0;
	mPrefPanelMemoryBuf = 0;

	if (mVerbose >= 1)
		IOLog("VoodooHDA DBG: init() returning true\n");
	return true;
}

#define	PCI_CLASS_MULTI				0x04
#define PCI_SUBCLASS_MULTI_HDA		0x03

void VoodooHDADevice::initMixerDefaultValues(void)
{
	OSDictionary *MixerValues = 0;
	OSNumber *tmpNumber = 0;
	UInt16 tmpUI16 = 0;
	int index;
	OSString *tmpString = 0;
//	int MixValueCount = sizeof(MixerValueNamesBind) / sizeof(MixerValueName);
	
	MixerValues = OSDynamicCast(OSDictionary, getProperty("MixerValues"));

	for(int i=0; i<SOUND_MIXER_NRDEVICES; i++){
						
		tmpUI16 = MixerValueNamesBind[i].initValue;
	
		
		if(MixerValues && MixerValueNamesBind[i].name != 0 && MixerValueNamesBind[i].name[0] != 0) {
			tmpNumber = OSDynamicCast(OSNumber, MixerValues->getObject(MixerValueNamesBind[i].name));
			if (tmpNumber) {
				tmpUI16 = tmpNumber->unsigned16BitValue();
			} else {
				tmpString = OSDynamicCast(OSString, MixerValues->getObject(MixerValueNamesBind[i].name));
				if(tmpString) {
					long unsigned int jj = 0;
					int jjj = 0;
					if(sscanf(tmpString->getCStringNoCopy(), "0x%08lx", &jj)) {
						tmpUI16 = jj;
					}else if(sscanf(tmpString->getCStringNoCopy(), "%4d", &jjj)){
						tmpUI16 = jjj;
					}
				}
			}
		}
		//logMsg("Item %d init %d, index %d\n", i , tmpUI16, MixerValueNamesBind[i].index);
	
		
		index = MixerValueNamesBind[i].index;
		if(index >= 0 && index < SOUND_MIXER_NRDEVICES) 
			mMixerDefaults[index] = tmpUI16;
	}
}

IOService *VoodooHDADevice::probe(IOService *provider, SInt32 *score)
{
	IOService *result;
	UInt16 vendorId, deviceId, subVendorId, subDeviceId;
	UInt32 classCode = 0;
	UInt8 devClass = 0, subClass = 0;
//	bool contIsGeneric = false;
	int n;

	if (mVerbose >= 1)
		IOLog("VoodooHDA DBG: probe() called, provider=%p score=%d\n", provider, score ? *score : -1);

	result = super::probe(provider, score);
	if (result != static_cast<IOService*>(this)) {
		if (mVerbose >= 1)
			IOLog("VoodooHDA DBG: super::probe() FAILED, result=%p this=%p\n", result, this);
		return result;
	}
	if (mVerbose >= 1)
		IOLog("VoodooHDA DBG: super::probe() OK\n");
	
	initMixerDefaultValues();
	
//Slice	
	OSDictionary *tmpDict = 0;
	OSIterator *iter = 0;
	const OSSymbol *dictKey = 0;
	OSNumber *tmpNumber = 0;
	UInt32 tmpUI32 = 0;
	OSString *tmpString = 0;
	OSArray *tmpArray = 0;
	UInt32 tmpUIArray[HDA_MAX_CONNS];
	UInt32 nArrayCount = 0;
//	UInt32 j = 0;
	
	NodesToPatch = OSDynamicCast(OSArray, getProperty("NodesToPatch"));
	if(NodesToPatch){
		NumNodes = NodesToPatch->getCount();
		if (NumNodes > MAX_NODES) {
			IOLog("VoodooHDA: NodesToPatch has %d entries, limiting to %d\n", NumNodes, MAX_NODES);
			NumNodes = MAX_NODES;
		}
		for(int i=0; i<NumNodes; i++){
			bzero(&NodesToPatchArray[i], sizeof(NodesToPatchArray[i]));
			tmpDict = OSDynamicCast(OSDictionary, NodesToPatch->getObject(i)); 
			if (!tmpDict)
				continue;
			iter = OSCollectionIterator::withCollection(tmpDict);
			if (iter) {
				while ((dictKey = (const OSSymbol *)iter->getNextObject())) {
					nArrayCount = 0;
					tmpUI32 = 0;
					bzero(tmpUIArray, sizeof(tmpUIArray));
					tmpArray = OSDynamicCast(OSArray, tmpDict->getObject(dictKey));
					if(tmpArray) {
						//logMsg("Array (%d) ", tmpArray->getCount());
						for(unsigned int arrayIndex = 0; arrayIndex < tmpArray->getCount(); arrayIndex++) {
							bool valueFound = false;
							tmpNumber = OSDynamicCast(OSNumber, tmpArray->getObject(arrayIndex));
							if (tmpNumber) {
								tmpUI32 = tmpNumber->unsigned32BitValue();
								valueFound = true;
							} else {
								tmpString = OSDynamicCast(OSString, tmpArray->getObject(arrayIndex));
								if(tmpString) {
									long unsigned int jj = 0;
									int jjj = 0;
									if(sscanf(tmpString->getCStringNoCopy(), "0x%08lx", &jj)) {
										tmpUI32 = static_cast<UInt32>(jj);
										valueFound = true;
									}else if(sscanf(tmpString->getCStringNoCopy(), "%4d", &jjj)){
										tmpUI32 = jjj;
										valueFound = true;
									}
								}
							}
							if (valueFound && nArrayCount < HDA_MAX_CONNS)
								tmpUIArray[nArrayCount++]= tmpUI32;
							
							//logMsg("%d ", tmpUI32);
						}
						//logMsg("\n");
					}else{
					
						tmpNumber = OSDynamicCast(OSNumber, tmpDict->getObject(dictKey));
						if (tmpNumber) {
							tmpUI32 = tmpNumber->unsigned32BitValue();
							tmpUIArray[0]= tmpUI32;
							nArrayCount = 1;
						} else {
							tmpString = OSDynamicCast(OSString, tmpDict->getObject(dictKey));
							long unsigned int jj = 0;
							if(tmpString && sscanf(tmpString->getCStringNoCopy(), "0x%08lx", &jj)) {
								tmpUI32 = static_cast<UInt32>(jj);
								tmpUIArray[0]= tmpUI32;
								nArrayCount = 1;
							}
						}
					}
					if(dictKey->isEqualTo("Node")){
						if(tmpUI32 == 0) 
							break;
						NodesToPatchArray[i].Node = tmpUI32;
					} else if (dictKey->isEqualTo("Config")){
						NodesToPatchArray[i].Config = tmpUI32;
						NodesToPatchArray[i].Enable |= 0x1;
					} else if (dictKey->isEqualTo("Conns")){
						for(unsigned int arrayIndex = 0; arrayIndex < nArrayCount; arrayIndex++) {
							NodesToPatchArray[i].Conns[arrayIndex] = tmpUIArray[arrayIndex];
						}
						NodesToPatchArray[i].nConns = nArrayCount;
						NodesToPatchArray[i].Enable |= 0x2;
					} else if (dictKey->isEqualTo("Type")){
						NodesToPatchArray[i].Type = tmpUI32;
						NodesToPatchArray[i].Enable |= 0x4;
					} else if (dictKey->isEqualTo("Cap")){
						NodesToPatchArray[i].Cap = tmpUI32;
						NodesToPatchArray[i].Enable |= 0x8;
					} else if (dictKey->isEqualTo("Enable")) {
						NodesToPatchArray[i].bEnabledWidget = tmpUI32;
						NodesToPatchArray[i].Enable |= 0x10;
					} else if (dictKey->isEqualTo("Control")) {
						NodesToPatchArray[i].Control = tmpUI32;
						NodesToPatchArray[i].Enable |= 0x20;
					} else if (dictKey->isEqualTo("Codec")) {
						//Codec по умолчанию = 0
						NodesToPatchArray[i].cad = tmpUI32;
					} else if (dictKey->isEqualTo("Select")) {
						NodesToPatchArray[i].nSel = tmpUI32;
						NodesToPatchArray[i].Enable |= 0x40;
					} else if (dictKey->isEqualTo("DAC")) {
						NodesToPatchArray[i].favoritDAC = tmpUI32;
						NodesToPatchArray[i].Enable |= 0x80;
					} else if (dictKey->isEqualTo("SwitchCh")) {
						//Меняем левый канал на правый для входных данных
						mSwitchCh = true;
					}
					
				}
				iter->release();
			}
		}
	}
// Temporary trace
	dumpMsg("VHD %d nodes patching \n", NumNodes);
#if __LP64__
    for(int i=0; i<NumNodes; i++){
        dumpMsg("VHD Codec=%d Node=%d Config=%08lx Conns=%ld Type=%d\n", NodesToPatchArray[i].cad, NodesToPatchArray[i].Node,
                (long unsigned int)NodesToPatchArray[i].Config, (long int)NodesToPatchArray[i].Conns, 
				(int)NodesToPatchArray[i].Type);
    }
#else
    for(int i=0; i<NumNodes; i++){
        dumpMsg("VHD Codec=%d Node=%d Config=%08lx Conns=%d Type=%d\n", (int)NodesToPatchArray[i].cad, (int)NodesToPatchArray[i].Node,
                NodesToPatchArray[i].Config, (int)NodesToPatchArray[i].Conns, (int)NodesToPatchArray[i].Type);
    }
#endif//	
	mPciNub = OSDynamicCast(IOPCIDevice, provider);
	if (!mPciNub) {
		if (mVerbose >= 1)
			IOLog("VoodooHDA DBG: cast to IOPCIDevice FAILED\n");
		errorMsg("error: couldn't cast provider to IOPCIDevice\n");
		return NULL;
	}
	if (mVerbose >= 1)
		IOLog("VoodooHDA DBG: mPciNub=%p, opening...\n", mPciNub);
	if (!mPciNub->open(this)) {
		if (mVerbose >= 1)
			IOLog("VoodooHDA DBG: mPciNub->open() FAILED\n");
		errorMsg("error: couldn't open PCI device\n");
		mPciNub = NULL;
		return NULL;
	}
	if (mVerbose >= 1)
		IOLog("VoodooHDA DBG: PCI device opened OK\n");
/*
	classCode = mPciNub->configRead32(kIOPCIConfigClassCode & 0xfc) >> 8;
	subClass = (classCode >> 8) & 0xff;
	devClass = (classCode >> 16) & 0xff;
	if ((devClass != PCI_CLASS_MULTI) || (subClass != PCI_SUBCLASS_MULTI_HDA)) {
		result = NULL;
		goto done;
	}*/ //Slice - do not check class code twice, it is performed by IOKit

	vendorId = mPciNub->configRead16(kIOPCIConfigVendorID);
	deviceId = mPciNub->configRead16(kIOPCIConfigDeviceID);
	mDeviceId = (deviceId << 16) | vendorId;

	/*
	 * Global AMD HDMI gate.
	 *
	 * The generic VoodooHDA personality also matches PCI multimedia/HDA
	 * devices, so an AMD GPU HDMI-audio controller can still be attached by
	 * the generic personality after the HDMI personality is disabled.  This
	 * must be blocked here, otherwise VoodooHDA still appears below HDAU@0,1
	 * even when VoodooHDAEnableAMDHDMI=false.
	 */
	classCode = mPciNub->configRead32(kIOPCIConfigClassCode & 0xfc) >> 8;
	subClass = (classCode >> 8) & 0xff;
	devClass = (classCode >> 16) & 0xff;
	bool isAMDHDACtrl = (vendorId == 0x1002 && devClass == PCI_CLASS_MULTI && subClass == PCI_SUBCLASS_MULTI_HDA);
	OSBoolean *amdHdmiDynamic = OSDynamicCast(OSBoolean, getProperty(kVoodooHDAAMDHDMIDynamicMatch));
	bool isAMDHDMIDynamicPersonality = (amdHdmiDynamic && amdHdmiDynamic->isTrue());
	OSBoolean *amdHdmiEnabledGlobal = OSDynamicCast(OSBoolean, getProperty(kVoodooHDAEnableAMDHDMI));
	bool amdHdmiEnabled = (amdHdmiEnabledGlobal && amdHdmiEnabledGlobal->isTrue());

	if (isAMDHDACtrl) {
		/*
		 * AMD HDMI is disabled by default.  It must be explicitly enabled in
		 * the active plist personality with VoodooHDAEnableAMDHDMI=true.
		 * This prevents the generic VoodooHDA personality from attaching to
		 * HDAU@0,1 when the user wants analog-only operation.
		 */
		if (!amdHdmiEnabled) {
			IOLog("VoodooHDA HDMI: AMD HDMI disabled by default/VoodooHDAEnableAMDHDMI=false, rejecting vendor=%04x device=%04x class=%06x\n",
			      vendorId, deviceId, classCode);
			mPciNub->close(this);
			mPciNub = NULL;
			return NULL;
		}

		if (!isAMDHDMIDynamicPersonality) {
			IOLog("VoodooHDA HDMI: generic personality rejected AMD HDMI controller vendor=%04x device=%04x class=%06x\n",
			      vendorId, deviceId, classCode);
			mPciNub->close(this);
			mPciNub = NULL;
			return NULL;
		}
	}

	/*
	 * Dynamic AMD HDMI match.
	 *
	 * The VoodooHDA-HDMI personality intentionally matches the PCI HDA
	 * audio class dynamically instead of relying only on a finite IOPCIMatch
	 * device-id list.  AMD reuses several HDMI-audio controller IDs across
	 * Polaris, Vega, Radeon VII, Navi/RDNA1 and Navi/RDNA2, and board vendors
	 * can expose variants that are not present in older plist lists.
	 *
	 * When VoodooHDAAMDHDMIDynamicMatch is true, this personality only accepts
	 * vendor 0x1002 and PCI class 0x0403xx (High Definition Audio).  Non-AMD
	 * HDA controllers are rejected here so the normal VoodooHDA personality can
	 * still attach to Intel/Realtek analog controllers.
	 */
	if (isAMDHDMIDynamicPersonality) {
		if (vendorId != 0x1002 || devClass != PCI_CLASS_MULTI || subClass != PCI_SUBCLASS_MULTI_HDA) {
			IOLog("VoodooHDA HDMI: dynamic AMD HDMI personality rejected vendor=%04x device=%04x class=%06x\n",
			      vendorId, deviceId, classCode);
			mPciNub->close(this);
			mPciNub = NULL;
			return NULL;
		}

		if (mVerbose >= 1) {
			IOLog("VoodooHDA HDMI: dynamic AMD HDMI personality accepted vendor=%04x device=%04x class=%06x\n",
			      vendorId, deviceId, classCode);
		}
	}
	for (n = 0; gControllerList[n].name; n++) {
		if (gControllerList[n].model == mDeviceId)
			break;
		else if (HDA_DEV_MATCH(gControllerList[n].model, mDeviceId)) {
//			contIsGeneric = true;
			break;
		}
	}
	mControllerName = gControllerList[n].name;
	if (!mControllerName)
		mControllerName = "Generic";

	if (mVerbose >= 1)
		IOLog("VoodooHDA DBG: Controller: %s (vendor=%04x device=%04x)\n", mControllerName, vendorId, deviceId);
	errorMsg("Controller: %s (vendor ID: %04x, device ID: %04x)\n", mControllerName, vendorId, deviceId);

	/* Beat AppleGFXHDA for AMD/ATI HDMI audio — VoodooHDA programs
	 * GPU AZ registers directly via MMIO to enable audio pipeline. */
	if (vendorId == 0x1002 && score)
		*score = 5000000;

	subVendorId = mPciNub->configRead16(kIOPCIConfigSubSystemVendorID);
	subDeviceId = mPciNub->configRead16(kIOPCIConfigSubSystemID);
	mSubDeviceId = (subDeviceId << 16) | subVendorId;
	if (mSubDeviceId == HP_NX6325_SUBVENDORX)
		mSubDeviceId = HP_NX6325_SUBVENDOR;

	mLayoutId = 0;
	UInt32 bootLayoutId = 0;
	/* voodoo-layout-id is set by bootloader (OpenCore DeviceProperties);
	   layout-id on the PCI device may be overwritten by AppleHDA or the system */
	VoodooHDAReadLayoutIdProperty(mPciNub->getProperty("voodoo-layout-id"), &mLayoutId);
	if (mVerbose >= 1)
		IOLog("VoodooHDA DBG: voodoo-layout-id -> mLayoutId=%u\n", (unsigned)mLayoutId);
	if (mLayoutId == 0) {
		VoodooHDAReadLayoutIdProperty(mPciNub->getProperty("layout-id"), &mLayoutId);
		if (mVerbose >= 1)
			IOLog("VoodooHDA DBG: layout-id -> mLayoutId=%u\n", (unsigned)mLayoutId);
	}
	if (mLayoutId == 0) {
		VoodooHDAReadLayoutIdProperty(getProperty("LayoutId"), &mLayoutId);
		if (mVerbose >= 1)
			IOLog("VoodooHDA DBG: LayoutId plist -> mLayoutId=%u\n", (unsigned)mLayoutId);
	}
	if (VoodooHDAParseALCIDBootArg(&bootLayoutId)) {
		mLayoutId = bootLayoutId;
		if (mVerbose >= 1)
			IOLog("VoodooHDA DBG: alcid boot-arg -> mLayoutId=%u\n", (unsigned)mLayoutId);
	}
	if (mVerbose >= 1)
		IOLog("VoodooHDA DBG: final mLayoutId=%u\n", (unsigned)mLayoutId);
	if (mLayoutId)
		errorMsg("AppleALC: layout-id=%u\n", (unsigned int)mLayoutId);

//done:
	mPciNub->close(this);

	if (mVerbose >= 1)
		IOLog("VoodooHDA DBG: probe() returning result=%p\n", result);
	return result;
}

__attribute__((noinline, visibility("hidden")))
void VoodooHDADevice::disablePCIeNoSnoop(UInt16 vendorId)
{
	UInt16 snoop16;
	UInt8 snoop8;
	switch (vendorId) {
		case INTEL_VENDORID:
			/* Defines for Intel SCH HDA snoop control */
			snoop16 = mPciNub->configRead16( INTEL_SCH_HDA_DEVC );
			if (snoop16 & INTEL_SCH_HDA_DEVC_NOSNOOP) {
				mPciNub->configWrite16( INTEL_SCH_HDA_DEVC,	snoop16 & (~INTEL_SCH_HDA_DEVC_NOSNOOP));
			}
			break;
		case AMD_VENDORID:
			/* FALL THROUGH */
		case ATI_VENDORID:
			snoop8 = mPciNub->configRead8(0x42U);
			if (!(snoop8 & 2U)) {
				mPciNub->configWrite8(0x42U, (snoop8 & 0xf8U) | 2U);
			}
			break;
		case NVIDIA_VENDORID:
			snoop8 = mPciNub->configRead8(0x4eU);
			if ((snoop8 & 15U) != 15U) {
				mPciNub->configWrite8(0x4eU, (snoop8 & 0xf0U) | 15U);
			}
			break;
	}
}

bool VoodooHDADevice::initHardware(IOService *provider)
{
	bool result = false;
	UInt16 config, vendorId;
	UInt32 gCtl;
	UInt16 msiCtl;
	OSBoolean *osBool;
	char string[20];
	bool enableAMDFramebufferNotifier = false;
	bool atiHDMICodecPresent = false;
	VoodooHDAAMDGPUFamily amdFramebufferNotifierFamily = kVoodooHDAAMDGPUUnknown;
	UInt16 amdFramebufferNotifierGpuId = 0;
	bool amdFramebufferNotifierUsedHDAFallback = false;

	if (mVerbose >= 1)
		IOLog("VoodooHDA DBG: initHardware() called\n");

//moved here from init ----------
  mMsgBufferEnabled = false;
	mMsgBufferSize = MSG_BUFFER_SIZE;
	mMsgBufferPos = 0;
	
	mSwitchCh = false;
	mMsgBuffer = (char *) allocMem(mMsgBufferSize);
	if (!mMsgBuffer) {
		errorMsg("error: couldn't allocate message buffer (%ld bytes)\n", mMsgBufferSize);
		goto done;
	}
	
	mExtMessageLock = IOLockAlloc();
	if (!mExtMessageLock) {
		errorMsg("error: failed to allocate extended message lock\n");
		goto done;
	}
	mExtMsgBufferSize = MSG_BUFFER_SIZE;
	mExtMsgBufferPos = 0;
	
	mExtMsgBuffer = (char *) allocMem(mExtMsgBufferSize);
	if (!mExtMsgBuffer) {
		errorMsg("error: couldn't allocate ext message buffer (%ld bytes)\n", mExtMsgBufferSize);
		goto done;
	}
//--------------  
  
	//logMsg("VoodooHDADevice[%p]::initHardware\n", this);

	oldConfig = UINT16_MAX;
	if (!mPciNub || !super::initHardware(provider))
		goto done;
	if (!mPciNub->open(this)) {
		errorMsg("error: failed to open PCI device\n");
		goto done;
	}

	config = mPciNub->configRead16(kIOPCIConfigCommand);
	oldConfig = config;
	config |= (kIOPCICommandBusMaster | kIOPCICommandMemorySpace); // | kIOPCICommandMemWrInvalidate); //Slice
	 //config &= ~kIOPCICommandIOSpace; //Slice - not implemented for HDA
	mPciNub->configWrite16(kIOPCIConfigCommand, config);
	if (mPciNub->hasPCIPowerManagement(kPCIPMCD3Support))
		mPciNub->enablePCIPowerManagement(kPCIPMCSPowerStateD3);

	// Note: kIOMapInhibitCache is the default for PCI bars... - Zenith432
	mBarMap = mPciNub->mapDeviceMemoryWithIndex(0U);
	if (!mBarMap) {
		errorMsg("error: mapDeviceMemoryWith for BAR0 failed\n");
		goto done;
	}
	mRegBase = mBarMap->getVirtualAddress();
	if (!mRegBase) {
		errorMsg("error: getVirtualAddress for mBarMap failed\n");
		goto done;
	}
//	strncpy(string, "Voodoo HDA Device", sizeof(string));
  snprintf(string, sizeof(string), "VoodooHDADevice%x ", (unsigned int)(mDeviceId >> 16));
	setDeviceName(string);
	setDeviceShortName("VoodooHDA ");
	setManufacturerName("Voodoo ");
	//TODO: setDeviceModelName
  snprintf(string, sizeof(string), "VoodooHDA:%x ", (unsigned int)(mDeviceId >> 16));
  setDeviceModelName(string);
	setDeviceTransportType(kIOAudioDeviceTransportTypeBuiltIn);

//	logMsg("deviceId: %08lx, subDeviceId: %08lx\n", mDeviceId, mSubDeviceId);

	vendorId = mDeviceId & 0xffff;
	if (vendorId == INTEL_VENDORID) {
		/* TCSEL -> TC0 */
		UInt8 value = mPciNub->configRead8(0x44);
		mPciNub->configWrite8(0x44, value & 0xf8);
//		logMsg("TCSEL: %02x -> %02x\n", value, mPciNub->configRead8(0x44));
	}
	disablePCIeNoSnoop(vendorId);
	if (vendorId == NVIDIA_VENDORID &&
      !OSDynamicCast(OSBoolean, getProperty(kVoodooHDAAllowMSI))) {
		/*
		 * Disable MSI for NVIDIA controller if not in Info.plist.
		 *   Known to have problems with MSI (Quirk from HDAC)
		 */
		mAllowMSI = false;
	}
	msiCtl = mPciNub->configRead16(0x62);
	logMsg("MSI_CTL=0x%04x\n", msiCtl);

	/*
	 * Reset controller BEFORE reading capabilities.
	 * AMD Radeon GPUs report invalid CORB/RIRB size (0) unless
	 * the controller is reset first (FreeBSD commit dab9ef544868).
	 */
	if (!resetController(true)) {
		errorMsg("error: resetController failed\n");
		goto done;
	}

	if (!getCapabilities()) {
		errorMsg("error: getCapabilities failed\n");
		goto done;
	}

	mCorbMem = allocateDmaMemory(mCorbSize * sizeof (UInt32), "CORB", 0 /* kIOMapInhibitCache */);
	if (!mCorbMem) {
		errorMsg("error: allocateDmaMemory for CORB memory failed\n");
		goto done;
	}

	mRirbMem = allocateDmaMemory(mRirbSize * sizeof (RirbResponse), "RIRB", 0 /* kIOMapInhibitCache */);
	if (!mRirbMem) {
		errorMsg("error: allocateDmaMemory for RIRB memory failed\n");
		goto done;
	}

	initCorb();
	initRirb();

		if (!setupWorkloop()) {
			errorMsg("error: setupWorkloop failed\n");
			goto done;
		}
		enableEventSources();

	/*
	 * HDMI/DP PCM channels are attached during scanCodecs() -> pcmAttach() ->
	 * channelInit().  The Apple-like graphics-audio DMA path must therefore
	 * exist before codec parsing starts, otherwise digital channels fall back
	 * to the legacy 262144-byte VoodooHDA ring and never re-enter the
	 * VoodooGFXHDA allocator.
	 */
	if (!mGFXController) {
		mGFXController = new VoodooGFXHDAController;
		if (!mGFXController || !mGFXController->init(this)) {
			errorMsg("error: couldn't initialize VoodooGFXHDAController\n");
			goto done;
		}
	}

	LOCK();

//	logMsg("Starting CORB Engine...\n");
	startCorb();
// logMsg("Starting RIRB Engine...\n");
	startRirb();

	logMsg("Enabling controller interrupt...\n");
	gCtl = readData32(HDAC_GCTL);
	logMsg("HDAC_CTL=0x%04x\n", gCtl);
	writeData32(HDAC_GCTL, gCtl | HDAC_GCTL_UNSOL);
	writeData32(HDAC_INTCTL, HDAC_INTCTL_CIE | HDAC_INTCTL_GIE);
	IODelay(1000);

	// todo: hdac_config_fetch(&mQuirksOn, &mQuirksOff);
	mQuirksOn = 0;
	mQuirksOff = 0;

	enableMsgBuffer(true);
//	logMsg("Scanning HDA codecs...\n");
	scanCodecs();
	enableMsgBuffer(false);
	UNLOCK();
	for (int n = 0; n < HDAC_CODEC_MAX; n++) {
		Codec *codec = mCodecs[n];
		if (!codec)
			continue;
		dumpMsg("Codec #%d: %s (vendor ID: %04x, device ID: %04x)\n", codec->cad, findCodecName(codec),
				codec->vendorId, codec->deviceId);
	}

	/*
	 * AMD/ATI HDMI needs framebuffer EDID/audio-codec-info to select the single
	 * connected HDA pin and avoid exposing every stale HDMI widget.  RX6600/RDNA
	 * confirmed this path: VoodooHDA owns HDAU and uses the notifier for read-only
	 * display routing while the native AMD framebuffer owns the display pipe.
	 *
	 * VoodooHDAEnableAMDFramebufferNotifier remains an opt-out.  The dangerous
	 * AppleGraphics pipe write is guarded inside VoodooHDAFramebufferNotifier by
	 * GPU family, not by disabling the notifier itself.
	 */
	for (int n = 0; n < HDAC_CODEC_MAX; n++) {
		Codec *codec = mCodecs[n];
		if (codec && isAtiHdmiCodec(codec)) {
			atiHDMICodecPresent = true;
			break;
		}
	}
	if (atiHDMICodecPresent) {
			enableAMDFramebufferNotifier = shouldStartAmdFramebufferNotifierForController(
			    mPciNub, mDeviceId, &amdFramebufferNotifierFamily,
			    &amdFramebufferNotifierGpuId, &amdFramebufferNotifierUsedHDAFallback);
			if (!enableAMDFramebufferNotifier) {
				if (mVerbose >= 1) {
					IOLog("VoodooHDA HDMI: AMD framebuffer notifier skipped; no AMD GPU/HDA policy match family=%s gpu=%04x hda=%08x\n",
					      amdGpuFamilyNameForHDAPolicy(amdFramebufferNotifierFamily),
					      amdFramebufferNotifierGpuId, (unsigned)mDeviceId);
				}
			}
		}
	osBool = OSDynamicCast(OSBoolean, getProperty(kVoodooHDAEnableAMDFramebufferNotifier));
	if (osBool && !osBool->isTrue())
		enableAMDFramebufferNotifier = false;

	if (enableAMDFramebufferNotifier) {
		if (mVerbose >= 1) {
			IOLog("VoodooHDA HDMI: AMD framebuffer notifier allowed for EDID/pin routing family=%s gpu=%04x hda=%08x%s\n",
			      amdGpuFamilyNameForHDAPolicy(amdFramebufferNotifierFamily),
			      amdFramebufferNotifierGpuId, (unsigned)mDeviceId,
			      amdFramebufferNotifierUsedHDAFallback ? " (HDA fallback)" : "");
		}
		for (int n = 0; n < HDAC_CODEC_MAX; n++) {
			Codec *codec = mCodecs[n];
			if (!codec || !isAtiHdmiCodec(codec))
				continue;
			if (!mFBNotifier)
				mFBNotifier = VoodooHDAFramebufferNotifier::withDevice(this);
			if (mFBNotifier) {
				nid_t pins[VHDA_FB_MAX_PINS];
				int pinCount = 0;
				for (int fg = 0; fg < codec->numFuncGroups; fg++) {
					FunctionGroup *funcGroup = &codec->funcGroups[fg];
					if (funcGroup->nodeType != HDA_PARAM_FCT_GRP_TYPE_NODE_TYPE_AUDIO)
						continue;
					for (int w = 0; w < funcGroup->numNodes; w++) {
						Widget *widget = &funcGroup->widgets[w];
						if (widget->enable && widget->type == HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX &&
							(HDA_PARAM_PIN_CAP_HDMI(widget->pin.cap) || HDA_PARAM_PIN_CAP_DP(widget->pin.cap)) &&
							pinCount < VHDA_FB_MAX_PINS)
							pins[pinCount++] = widget->nid;
					}
				}
				if (pinCount > 0) {
					mFBNotifier->registerATIPins(codec->cad, pins, pinCount);
					mFBNotifier->startMatching();
					if (mVerbose >= 1)
						IOLog("VoodooHDA HDMI: framebuffer notifier started for %d ATI pins\n", pinCount);
				}
			}
		}
	} else if (atiHDMICodecPresent) {
		if (mVerbose >= 1)
			IOLog("VoodooHDA HDMI: AMD framebuffer notifier inactive; HDMI remains on HDA/native framebuffer path\n");
	}

	if (!mNumChannels) {
		errorMsg("error: no PCM channels found\n");
		goto done;
	}
	for (int n = 0; n < mNumChannels; n++) {
		if (!createAudioEngine(&mChannels[n])) {
			errorMsg("error: createAudioEngine for channel %d failed\n", n);
			goto done;
		}
	}
	/* After all HDMI slots have been collected, publish the effective HDMI
	 * set.  Modern AMD keeps the single framebuffer-selected route; legacy
	 * Polaris recovery keeps one visible output and mirrors candidate pins. */
	if (mNumHDMIEngines > 0)
		updateHDMIEnginePresence();
	if ((!audioEngines || (audioEngines->getCount() == 0)) && mNumHDMIEngines == 0) {
		errorMsg("error: no audio engines were created\n");
		goto done;
	}
	LOCK();
	//Slice - it's a time to switch engines
	for (int n = 0; n < HDAC_CODEC_MAX; n++) {
		Codec *codec = mCodecs[n];
		if (!codec) continue;
		for (int funcGroupNum = 0; funcGroupNum < codec->numFuncGroups; funcGroupNum++) {
			FunctionGroup *funcGroup = &codec->funcGroups[funcGroupNum];
			if (!funcGroup) continue;
			if (funcGroup->nodeType != HDA_PARAM_FCT_GRP_TYPE_NODE_TYPE_AUDIO)
				continue;	
			if (funcGroup->mSwitchEnable)
				switchHandler(funcGroup, true);
		}
	}
	UNLOCK();

	result = true;
done:
	if (!result)
		stop(provider);

	return result;
}

void VoodooHDADevice::stop(IOService *provider)
{
	logMsg("VoodooHDADevice[%p]::stop\n", this);

	if (mFBNotifier) {
		mFBNotifier->stopMatching();
		mFBNotifier->release();
		mFBNotifier = NULL;
	}

	disableEventSources();

	if (mWorkLoop) {
		if (mTimerSource) {
			mWorkLoop->removeEventSource(mTimerSource);
			mTimerSource->release();
			mTimerSource = NULL;
		}

		if (mInterruptSource) {
			mWorkLoop->removeEventSource(mInterruptSource);
			mInterruptSource->release();
			mInterruptSource = NULL;
		}

		mWorkLoop->release();
		mWorkLoop = NULL;
	}

	if (mRegBase) {
		LOCK();
		if (!resetController(false))
			errorMsg("warning: resetController failed\n");
		UNLOCK();
	}
	if (mPciNub &&
		(mPciNub->isOpen(this) || mPciNub->open(this))) {
		if (oldConfig != UINT16_MAX) {
			mPciNub->configWrite16(kIOPCIConfigCommand, oldConfig); //Slice
		}
		mPciNub->enablePCIPowerManagement(kPCIPMCSPowerStateD0);
		mPciNub->close(this);
	}
	
	super::stop(provider);
}

#define FREE_LOCK(x)		do { if (x) { IOLockFree(x); (x) = NULL; } } while (0)
#define FREE_DMA_MEMORY(x)	do { if (x) { freeDmaMemory(x); (x) = NULL; } } while (0)

void VoodooHDADevice::free()
{
	if (mVerbose >= 1)
		IOLog("VoodooHDA DBG: free() called\n");
	logMsg("VoodooHDADevice[%p]::free\n", this);

	/* free() can run after a failed start or during an unsafe live unload.  Keep
	 * objects that child engines may still need (mLock, mGFXController, channel
	 * buffers) alive until after engines/notifiers are detached. */
	mMsgBufferEnabled = false;

	if (mFBNotifier) {
		mFBNotifier->stopMatching();
		mFBNotifier->release();
		mFBNotifier = NULL;
	}

	if (mGFXController)
		mGFXController->detachAllStreams();

	int hdmiEngineCount = (mNumHDMIEngines > 16) ? 16 : mNumHDMIEngines;
	if (hdmiEngineCount < 0)
		hdmiEngineCount = 0;
	for (int i = 0; i < hdmiEngineCount; i++) {
		if (mHDMIEngines[i].engine) {
			mHDMIEngines[i].engine->release();
			mHDMIEngines[i].engine = NULL;
		}
		mHDMIEngines[i].channel = NULL;
	}
	mNumHDMIEngines = 0;

	if (mGFXController) {
		delete mGFXController;
		mGFXController = NULL;
	}

	if (mRegBase)
		mRegBase = 0;
	RELEASE(mBarMap);

	mPciNub = NULL;

	if (mNumChannels) {
		ASSERT(mChannels);
		for (int i = 0; i < mNumChannels; i++) {
			if (mChannels[i].buffer)
				FREE_DMA_MEMORY(mChannels[i].buffer);
			if (mChannels[i].bdlMem)
				FREE_DMA_MEMORY(mChannels[i].bdlMem);
		}
		FREE(mChannels);
	} else
		ASSERT(!mChannels);

	FREE_DMA_MEMORY(mDmaPosMem);
	FREE_DMA_MEMORY(mCorbMem);
	FREE_DMA_MEMORY(mRirbMem);

	for (int i = 0; i < HDAC_CODEC_MAX; i++) {
		Codec *codec = mCodecs[i];
		if (!codec)
			continue;
		mCodecs[i] = NULL;
		if (codec->numFuncGroups)
			ASSERT(codec->funcGroups);
		else
			ASSERT(!codec->funcGroups);
		for (int j = 0; j < codec->numFuncGroups; j++) {
			FunctionGroup *funcGroup = &codec->funcGroups[j];
			if (funcGroup->widgets) {
				for (int w = 0; w < funcGroup->numNodes; w++) {
					if (funcGroup->widgets[w].eld) {
						freeMem(funcGroup->widgets[w].eld);
						funcGroup->widgets[w].eld = NULL;
						funcGroup->widgets[w].eld_len = 0;
					}
				}
			}
			FREE(funcGroup->widgets);
			if (funcGroup->nodeType == HDA_PARAM_FCT_GRP_TYPE_NODE_TYPE_AUDIO) {
				FREE(funcGroup->audio.controls);
				FREE(funcGroup->audio.assocs);
				FREE(funcGroup->audio.pcmDevices);
			}
		}
		FREE(codec->funcGroups);
		FREE(codec);
	}

	FREE(mPrefPanelMemoryBuf);
	FREE(mExtMsgBuffer);
	FREE(mMsgBuffer);

	/* Locks are last: error/log paths and engine teardown can still reference them
	 * while the objects above are being released. */
	FREE_LOCK(mPrefPanelMemoryBufLock);
	FREE_LOCK(mExtMessageLock);
	FREE_LOCK(mMessageLock);
	FREE_LOCK(mLock);

	super::free();
}

nid_t VoodooHDADevice::getHDMIPinForChannel(Channel *channel)
{
	if (!channel || !channel->funcGroup || channel->assocNum < 0)
		return (nid_t)-1;
	if (!channel->funcGroup->audio.assocs)
		return (nid_t)-1;
	if (channel->assocNum >= channel->funcGroup->audio.numAssocs)
		return (nid_t)-1;
	AudioAssoc *assoc = &channel->funcGroup->audio.assocs[channel->assocNum];
	for (int j = 0; j < 16; j++) {
		if (assoc->pins[j] <= 0) continue;
		Widget *w = widgetGet(channel->funcGroup, assoc->pins[j]);
		if (w && (HDA_PARAM_PIN_CAP_HDMI(w->pin.cap) || HDA_PARAM_PIN_CAP_DP(w->pin.cap)))
			return assoc->pins[j];
	}
	return (nid_t)-1;
}


static bool VoodooHDAIsAnalogOutputPin(Widget *widget)
{
	if (!widget || widget->enable == 0)
		return false;
	if (widget->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX)
		return false;
	if (HDA_PARAM_PIN_CAP_HDMI(widget->pin.cap) || HDA_PARAM_PIN_CAP_DP(widget->pin.cap))
		return false;

	UInt32 device = widget->pin.config & HDA_CONFIG_DEFAULTCONF_DEVICE_MASK;
	return device == HDA_CONFIG_DEFAULTCONF_DEVICE_LINE_OUT ||
	       device == HDA_CONFIG_DEFAULTCONF_DEVICE_SPEAKER ||
	       device == HDA_CONFIG_DEFAULTCONF_DEVICE_HP_OUT;
}

void VoodooHDADevice::restoreAnalogPlaybackPath(Channel *channel, const bool shouldLock)
{
	if (!channel || !channel->funcGroup || !channel->funcGroup->codec || !channel->pcmDevice)
		return;
	if (channel->direction != PCMDIR_PLAY || channel->pcmDevice->digital)
		return;
	if (!mEnableAnalogPathRestore)
		return;
	if (!channel->funcGroup->audio.assocs)
		return;
	if (channel->assocNum < 0 || channel->assocNum >= channel->funcGroup->audio.numAssocs)
		return;

	if (shouldLock)
		LOCK();

	FunctionGroup *funcGroup = channel->funcGroup;
	AudioAssoc *assoc = &funcGroup->audio.assocs[channel->assocNum];
	nid_t cad = funcGroup->codec->cad;
	bool restored = false;

	/*
	 * Line-out -> Headphone -> Line-out restore.
	 *
	 * This intentionally is not a codec reset. Some codecs leave the selected
	 * analog pin with OUT_ENABLE cleared, or keep a mute bit stuck after jack
	 * switching. Rebooting works because it fully reprograms the codec; here we
	 * do the smallest local repair when the analog output becomes active again.
	 *
	 * Default path: only active analog playback pins (Line-out, Speaker,
	 * Headphone), no HDMI/DP, no capture/input mux controls. Re-enable
	 * PIN_WIDGET_CTRL OUT_ENABLE and reapply the current protected mixer level
	 * only on that pin. EAPD restore and broad auto-unmute remain opt-in because
	 * they can create pops or unsafe transients on old codecs.
	 */
	for (int j = 0; j < 16; j++) {
		nid_t pin = assoc->pins[j];
		if (pin <= 0)
			continue;
		Widget *pinWidget = widgetGet(funcGroup, pin);
		if (!VoodooHDAIsAnalogOutputPin(pinWidget))
			continue;

		UInt32 newCtrl = pinWidget->pin.ctrl | HDA_CMD_SET_PIN_WIDGET_CTRL_OUT_ENABLE;
		if (newCtrl != pinWidget->pin.ctrl) {
			pinWidget->pin.ctrl = newCtrl;
			sendCommand(HDA_CMD_SET_PIN_WIDGET_CTRL(cad, pinWidget->nid, pinWidget->pin.ctrl), cad);
			restored = true;
		}

		if (mEnableAnalogEAPDRestore && pinWidget->params.eapdBtl != HDAC_INVALID) {
			UInt32 newEapd = pinWidget->params.eapdBtl | HDA_CMD_SET_EAPD_BTL_ENABLE_EAPD;
			if (newEapd != pinWidget->params.eapdBtl) {
				pinWidget->params.eapdBtl = newEapd;
				UInt32 val = newEapd;
				if (funcGroup->audio.quirks & HDA_QUIRK_EAPDINV)
					val ^= HDA_CMD_SET_EAPD_BTL_ENABLE_EAPD;
				sendCommand(HDA_CMD_SET_EAPD_BTL_ENABLE(cad, pinWidget->nid, val), cad);
				restored = true;
			}
		}

		for (int i = 0; ; i++) {
			AudioControl *control = audioCtlEach(funcGroup, i);
			if (!control)
				break;
			if (control->enable == 0 || control->widget != pinWidget)
				continue;
			if (!((control->dir & HDA_CTL_OUT) || (control->dir & HDA_CTL_IN)))
				continue;
			if (!(control->ossmask & (SOUND_MASK_VOLUME | SOUND_MASK_PCM | SOUND_MASK_OGAIN)))
				continue;
			if (control->step <= 0)
				continue;

			int lvol = 100;
			int rvol = 100;
			for (int k = 0; k < SOUND_MIXER_NRDEVICES; k++) {
				if (control->ossmask & (1U << k)) {
					lvol = lvol * channel->pcmDevice->left[k] / 100;
					rvol = rvol * channel->pcmDevice->right[k] / 100;
				}
			}

			lvol = VoodooHDAAnalogOutputScalePercent(lvol);
			rvol = VoodooHDAAnalogOutputScalePercent(rvol);
			UInt32 mute = (lvol == 0) ? HDA_AMP_MUTE_LEFT : 0;
			mute |= (rvol == 0) ? HDA_AMP_MUTE_RIGHT : 0;
			lvol = (lvol * control->step + 50) / 100;
			rvol = (rvol * control->step + 50) / 100;

			if (!control->forcemute && control->muted == mute &&
			    control->left == lvol && control->right == rvol)
				continue;

			control->forcemute = 0;
			audioCtlAmpSet(control, mute, lvol, rvol);
			restored = true;
		}

		AudioControl *pinMute = audioCtlAmpGet(funcGroup, pinWidget->nid, HDA_CTL_IN, -1, 1);
		if (pinMute && pinMute->mute && pinMute->forcemute) {
			UInt32 mute = HDA_AMP_MUTE_NONE;
			if (channel->pcmDevice->left[SOUND_MIXER_VOLUME] == 0 ||
			    channel->pcmDevice->left[SOUND_MIXER_PCM] == 0)
				mute |= HDA_AMP_MUTE_LEFT;
			if (channel->pcmDevice->right[SOUND_MIXER_VOLUME] == 0 ||
			    channel->pcmDevice->right[SOUND_MIXER_PCM] == 0)
				mute |= HDA_AMP_MUTE_RIGHT;
			pinMute->forcemute = 0;
			audioCtlAmpSet(pinMute, mute, HDA_AMP_VOL_DEFAULT, HDA_AMP_VOL_DEFAULT);
			restored = true;
		}
	}

	if (mEnableAnalogAutoUnmute) {
		for (int i = 0; ; i++) {
			AudioControl *control = audioCtlEach(funcGroup, i);
			if (!control)
				break;
			if (control->enable == 0 || !control->widget)
				continue;
			if (!(control->dir & HDA_CTL_OUT) && !(control->dir & HDA_CTL_IN))
				continue;
			if (control->widget->bindAssoc != channel->assocNum && control->widget->bindAssoc != -2)
				continue;
			/* Never clear capture/input-mux mutes while fixing playback switching.
			 * Some codecs implement an output pin amp as HDA_CTL_IN; allow that
			 * only when the widget itself is the active analog output pin. */
			if ((control->dir & HDA_CTL_IN) && !VoodooHDAIsAnalogOutputPin(control->widget))
				continue;
			if (!(control->ossmask & (SOUND_MASK_VOLUME | SOUND_MASK_PCM | SOUND_MASK_OGAIN)))
				continue;
			if (!control->forcemute)
				continue;

			control->forcemute = 0;
			audioCtlAmpSet(control, HDA_AMP_MUTE_DEFAULT, HDA_AMP_VOL_DEFAULT, HDA_AMP_VOL_DEFAULT);
			restored = true;
		}
	}

	if (restored && mVerbose >= 1)
		logMsg("restoreAnalogPlaybackPath: restored analog playback association %d\n", channel->assocNum);

	if (shouldLock)
		UNLOCK();
}

bool VoodooHDADevice::createAudioEngine(Channel *channel)
{
	VoodooHDAEngine *audioEngine = NULL;
	bool result = false;

	//logMsg("VoodooHDADevice[%p]::createAudioEngine\n", this);

	audioEngine = new VoodooHDAEngine;
  if (!audioEngine) return false;
	if (!audioEngine->initWithChannel(channel)) {
		errorMsg("error: VoodooHDAEngine::init failed\n");
  } else {

    // cue8chalk: set volume change fix on the engine
    audioEngine->mEnableVolumeChangeFix = mEnableVolumeChangeFix;
    // VertexBZ: set Mute fix on the engine
    audioEngine->mEnableMuteFix = mEnableMuteFix;

    //  audioEngine->mDisableInputMonitor = mDisableInputMonitor;
    audioEngine->Boost = Boost;

    /*
     * For HDMI/DP engines, only activate if the pin has a connected display.
     * Inactive engines are stored for dynamic activation on hot-plug.
     */
	    bool isHDMI = (channel->pcmDevice && channel->pcmDevice->digital >= 2);
	    nid_t hdmiPin = isHDMI ? getHDMIPinForChannel(channel) : (nid_t)-1;
	    bool hasPresence = false;

	    if (mNumHDMIEngines < 0)
	      mNumHDMIEngines = 0;

	    if (isHDMI && hdmiPin != (nid_t)-1 && mNumHDMIEngines >= 16) {
	      errorMsg("error: too many HDMI/DP engines, deferring pin=%d\n", hdmiPin);
	      result = true;
	    } else if (isHDMI && hdmiPin != (nid_t)-1 && mNumHDMIEngines < 16) {
	      Codec *codec = channel->funcGroup->codec;
	      UInt32 pinSense = sendCommand(HDA_CMD_GET_PIN_SENSE(codec->cad, hdmiPin), codec->cad);
      bool presence = (pinSense & (1U << 31)) != 0;
      bool eldValid = (pinSense & HDA_CMD_GET_PIN_SENSE_ELD_VALID) != 0;
      hasPresence = presence ||
		    (isAtiHdmiCodec(codec) && appleGfxHdaAmdUsesCachedELDPresence(codec->deviceId) && eldValid);

      HDMIEngineSlot *slot = &mHDMIEngines[mNumHDMIEngines++];
      slot->engine = audioEngine;
      slot->channel = channel;
      slot->pinNid = hdmiPin;
      slot->cad = channel->funcGroup->codec->cad;
      slot->activated = false;
      slot->mirrorCandidate = false;
      slot->mirroredActive = false;
      audioEngine->retain(); /* keep alive past RELEASE below */

      /* RX5xx/Polaris HDMI safety: do not publish inactive/no-display
       * HDMI engines at boot.  Keep the slot retained for later hot-plug/ELD
       * activation, but only publish the IOAudioEngine when the pin reports
       * real presence (or AMD cached ELD presence).  This keeps CoreAudio
       * from creating "no display" HDMI outputs and avoids extra GPU/HDMI
       * churn during display init. */
      bool atiHDMI = isAtiHdmiCodec(codec);

      /* RX5xx/Polaris connected-only policy:
       * Do not publish ATI/AMD HDMI engines one-by-one while the parser is
       * still creating channels.  Many AMD HDMI codecs expose stale/cached
       * ELD on every pin, so early activation makes CoreAudio list five
       * identical HDMI outputs.  Store the slots now and let
       * updateHDMIEnginePresence() publish the safe effective set after all
       * channels exist. */
      if (atiHDMI) {
        result = true;
      } else if (hasPresence) {
        if (activateAudioEngine(audioEngine) == kIOReturnSuccess) {
          slot->activated = true;
          result = true;
        }
      } else {
        result = true;
      }
      if (mVerbose >= 1) {
        IOLog("VoodooHDA DBG: HDMI engine pin=%d presence=%d activated=%d%s\n",
              hdmiPin, hasPresence, slot->activated,
              slot->activated ? "" : " deferred-connected-only");
      }
    } else {
      /* Non-HDMI: always activate */
      if (activateAudioEngine(audioEngine) != kIOReturnSuccess) {
        errorMsg("error: activateAudioEngine failed\n");
      } else {
        result = true;
      }
    }
  }

	RELEASE(audioEngine);

	return result;
}

IOReturn VoodooHDADevice::performPowerStateChange(IOAudioDevicePowerState oldPowerState,
		IOAudioDevicePowerState newPowerState, __unused UInt32 *microsecondsUntilComplete)
{
	IOReturn result = kIOReturnSuccess;

	//logMsg("VoodooHDADevice[%p]::performPowerStateChange(%d, %d)\n", this, oldPowerState, newPowerState);

	if (oldPowerState == kIOAudioDeviceSleep) {
		if (!resume()) {
			errorMsg("error: resume action failed\n");
			result = kIOReturnError;
		}
	} else if (newPowerState == kIOAudioDeviceSleep) {
		if (!suspend()) {
			errorMsg("error: suspend action failed\n");
			result = kIOReturnError;
		}
	}

	return result;
}

/*
 * Suspend and power down HDA bus and codecs.
 */
bool VoodooHDADevice::suspend()
{
		//logMsg("VoodooHDADevice[%p]::suspend\n", this);

	LOCK();
		//Slice - trace PCI
/*	for (int i=0; i<0xff; i+=16) {
		for(int j=0; j<15; j+=4)
			logMsg("(%02x)=%08x   ",(unsigned int)(i+j), (unsigned int)mPciNub->configRead32(i+j));  //for trace
		logMsg("\n");
	}*/
		//	
	for (int i = 0; i < mNumChannels; i++) {
		if (mChannels[i].flags & HDAC_CHN_RUNNING) {
			errorMsg("warning: found active channel during suspend action\n");
			channelStop(&mChannels[i], false);
		}
		mChannels[i].flags |= HDAC_CHN_SUSPEND;
	}

	for (int codecNum = 0; codecNum < HDAC_CODEC_MAX; codecNum++) {
		Codec *codec = mCodecs[codecNum];
		if (!codec)
			continue;
		for (int funcGroupNum = 0; funcGroupNum < codec->numFuncGroups; funcGroupNum++) {
			FunctionGroup *funcGroup = &codec->funcGroups[funcGroupNum];
	//		logMsg("Power down FG cad=%d nid=%d to the D3 state...\n", codec->cad, funcGroup->nid);
			sendCommand(HDA_CMD_SET_POWER_STATE(codec->cad, funcGroup->nid, HDA_CMD_POWER_STATE_D3),
					codec->cad);
		}
	}

//	logMsg("Resetting controller...\n");
	if (!resetController(false)) {
		errorMsg("error: resetController failed\n");
		UNLOCK();
		return false;
	}

	UNLOCK();

//	logMsg("Suspend done.\n");

	return true;
}

/*
 * Powerup and restore HDA bus and codecs state.
 */
bool VoodooHDADevice::resume()
{
	logMsg("VoodooHDADevice[%p]::resume\n", this);

	LOCK();
	if (!mPciNub) {
		errorMsg("error: resume without PCI device\n");
		UNLOCK();
		return false;
	}
		//Slice - dump PCI to understand what is the sleep issue
/*	for (int i=0; i<0xff; i+=16) {
		for(int j=0; j<15; j+=4)
			logMsg("(%02x)=%08x   ",(unsigned int)(i+j), (unsigned int)mPciNub->configRead32(i+j));  //for trace
		logMsg("\n");
	}*/
		//Slice - this trick was resolved weird sleep issue
	int vendorId = mDeviceId & 0xffff;
	if (vendorId == INTEL_VENDORID) {
		/* TCSEL -> TC0 */
		UInt8 value = mPciNub->configRead8(0x44);
		mPciNub->configWrite8(0x44, value & 0xf8);
			//		logMsg("TCSEL: %02x -> %02x\n", value, mPciNub->configRead8(0x44));
	}
	disablePCIeNoSnoop(static_cast<UInt16>(vendorId));

	logMsg("Resetting controller...\n");
	if (!resetController(true)) {
		errorMsg("error: resetController failed\n");
		UNLOCK();
		return false;
	}

	initCorb();
	initRirb();
//Slice

//	setupWorkloop();
//	enableEventSources();

//	logMsg("Starting CORB Engine...\n");
	startCorb();
//	logMsg("Starting RIRB Engine...\n");
	startRirb();

//	logMsg("Enabling controller interrupt...\n");
	writeData32(HDAC_GCTL, readData32(HDAC_GCTL) | HDAC_GCTL_UNSOL);
	writeData32(HDAC_INTCTL, HDAC_INTCTL_CIE | HDAC_INTCTL_GIE);
	IODelay(1000);

	for (int codecNum = 0; codecNum < HDAC_CODEC_MAX; codecNum++) {
		Codec *codec = mCodecs[codecNum];
		if (!codec)
			continue;
		for (int funcGroupNum = 0; funcGroupNum < codec->numFuncGroups; funcGroupNum++) {
			FunctionGroup *funcGroup = &codec->funcGroups[funcGroupNum];
			if (funcGroup->nodeType != HDA_PARAM_FCT_GRP_TYPE_NODE_TYPE_AUDIO) {
				logMsg("Power down unsupported non-audio FG cad=%d nid=%d to the D3 state...\n",
						codec->cad, funcGroup->nid);
				sendCommand(HDA_CMD_SET_POWER_STATE(codec->cad, funcGroup->nid, HDA_CMD_POWER_STATE_D3),
						codec->cad);
				continue;
			}

//			logMsg("Power up audio FG cad=%d nid=%d...\n", funcGroup->codec->cad, funcGroup->nid);
			powerup(funcGroup);
			applyAppleALCWakeVerbs(funcGroup);
//			logMsg("AFG commit...\n");
			audioCommit(funcGroup);
//			logMsg("HP switch init...\n");
			UNLOCK(); // xxx
			for (int i = 0; i < funcGroup->audio.numPcmDevices; i++) {
//				logMsg("OSS mixer reinitialization...\n");
				mixerResume(&funcGroup->audio.pcmDevices[i]);
			}

			LOCK(); // xxx

			switchInit(funcGroup);

			if (funcGroup->mSwitchEnable)
				switchHandler(funcGroup, false);

		}
	}

	for (int i = 0; i < mNumChannels; i++) {
		if (!(mChannels[i].flags & HDAC_CHN_SUSPEND)) {
			errorMsg("warning: found non-suspended channel during resume action\n");
			continue;
		}
		mChannels[i].flags &= ~HDAC_CHN_SUSPEND;
	}

	UNLOCK();

//	logMsg("Resume done.\n");

	return true;
}

/******************************************************************************************/
/******************************************************************************************/

/*
 * Reset the controller to a quiescent and known state.
 hdac_reset(struct hdac_softc *sc, int wakeup)
 */
bool VoodooHDADevice::resetController(bool wakeup)
{
	UInt32 gctl;

	//logMsg("VoodooHDADevice[%p]::resetController(%d)\n", this, wakeup);

	/* Make sure WAKEEN bits are off */
	writeData16(HDAC_WAKEEN, 0U);

	/* Stop all Streams DMA engine */
	for (int i = 0; i < mInStreamsSup; i++)
		writeData32(HDAC_ISDCTL(i), 0);
	for (int i = 0; i < mOutStreamsSup; i++)
		writeData32(HDAC_OSDCTL(i), 0);
	for (int i = 0; i < mBiStreamsSup; i++)
		writeData32(HDAC_BSDCTL(i), 0);

	/* Stop Control DMA engines. */
	writeData8(HDAC_CORBCTL, 0);
	writeData8(HDAC_RIRBCTL, 0);

	/* Reset DMA position buffer. */
	writeData32(HDAC_DPIBLBASE, 0);
	writeData32(HDAC_DPIBUBASE, 0);

	/* Reset the controller. The reset must remain asserted for a minimum of 100us. */
	gctl = readData32(HDAC_GCTL);
	writeData32(HDAC_GCTL, gctl & ~HDAC_GCTL_CRST);
	for (int count = 0; count < 10000; count++) {
		gctl = readData32(HDAC_GCTL);
		if (!(gctl & HDAC_GCTL_CRST))
			break;
		IODelay(10);
	}
	if (gctl & HDAC_GCTL_CRST) {
		errorMsg("error: unable to put controller in reset\n");
		return false;
	}

	/* If wakeup is not requested - leave the controller in reset state. */
	if (!wakeup)
		return true;

	IODelay(100);
	gctl = readData32(HDAC_GCTL);
	writeData32(HDAC_GCTL, gctl | HDAC_GCTL_CRST);
	for (int count = 0; count < 10000; count++) {
		gctl = readData32(HDAC_GCTL);
		if (gctl & HDAC_GCTL_CRST)
			break;
		IODelay(10);
	}
	if (!(gctl & HDAC_GCTL_CRST)) {
		errorMsg("error: controller stuck in reset\n");
		return false;
	}

	/* Wait for codecs to finish their own reset sequence. The delay here
	 * should be of 250us but for some reasons, on it's not enough on my
	 * computer. Let's use twice as much as necessary to make sure that
	 * it's reset properly. */
	IODelay(1000);

	return true;
}

/*
 * Retreive the general capabilities of the hdac;
 *	Number of Input Streams
 *	Number of Output Streams
 *	Number of bidirectional Streams
 *	64bit ready
 *	CORB and RIRB sizes
 */
bool VoodooHDADevice::getCapabilities()
{
	bool result = false;
	UInt16 globalCap;
	UInt8 corbSizeReg, rirbSizeReg;

//	logMsg("VoodooHDADevice[%p]::getCapabilities\n", this);

	globalCap = readData16(HDAC_GCAP);
	mInStreamsSup = HDAC_GCAP_ISS(globalCap);
	mOutStreamsSup = HDAC_GCAP_OSS(globalCap);
	mBiStreamsSup = HDAC_GCAP_BSS(globalCap);
	mSDO = HDAC_GCAP_NSDO(globalCap);
	if (mVerbose >= 1) {
		IOLog("VoodooHDA DBG: GCAP=0x%04x ISS=%d OSS=%d BSS=%d NSDO=%d 64bit=%d\n",
		      globalCap, mInStreamsSup, mOutStreamsSup, mBiStreamsSup, mSDO,
		      (int)HDA_FLAG_MATCH(globalCap, HDAC_GCAP_64OK));
	}

	mSupports64Bit = HDA_FLAG_MATCH(globalCap, HDAC_GCAP_64OK);
	if ((mDeviceId & ~0x30000U) == HDA_NVIDIA_MCP78_1)	// Quirk from HDAC
		mSupports64Bit = false;

	corbSizeReg = readData8(HDAC_CORBSIZE);
	if ((corbSizeReg & HDAC_CORBSIZE_CORBSZCAP_256) == HDAC_CORBSIZE_CORBSZCAP_256)
		mCorbSize = 256;
	else if ((corbSizeReg & HDAC_CORBSIZE_CORBSZCAP_16) == HDAC_CORBSIZE_CORBSZCAP_16)
		mCorbSize = 16;
	else if ((corbSizeReg & HDAC_CORBSIZE_CORBSZCAP_2) == HDAC_CORBSIZE_CORBSZCAP_2)
		mCorbSize = 2;
	else {
		/* Hardware reports invalid CORB size (e.g. AMD Radeon GPUs).
		 * Default to 256 and write back to register (FreeBSD PR 289284). */
		errorMsg("warning: invalid CORB size (%02x), defaulting to 256\n", corbSizeReg);
		mCorbSize = 256;
		writeData8(HDAC_CORBSIZE, HDAC_CORBSIZE_CORBSIZE(HDAC_CORBSIZE_CORBSIZE_256));
	}

	rirbSizeReg = readData8(HDAC_RIRBSIZE);
	if ((rirbSizeReg & HDAC_RIRBSIZE_RIRBSZCAP_256) == HDAC_RIRBSIZE_RIRBSZCAP_256)
		mRirbSize = 256;
	else if ((rirbSizeReg & HDAC_RIRBSIZE_RIRBSZCAP_16) == HDAC_RIRBSIZE_RIRBSZCAP_16)
		mRirbSize = 16;
	else if ((rirbSizeReg & HDAC_RIRBSIZE_RIRBSZCAP_2) == HDAC_RIRBSIZE_RIRBSZCAP_2)
		mRirbSize = 2;
	else {
		/* Hardware reports invalid RIRB size (e.g. AMD Radeon GPUs).
		 * Default to 256 and write back to register (FreeBSD PR 289284). */
		errorMsg("warning: invalid RIRB size (%02x), defaulting to 256\n", rirbSizeReg);
		mRirbSize = 256;
		writeData8(HDAC_RIRBSIZE, HDAC_RIRBSIZE_RIRBSIZE(HDAC_RIRBSIZE_RIRBSIZE_256));
	}

//	logMsg("    CORB size: %d\n", mCorbSize);
//	logMsg("    RIRB size: %d\n", mRirbSize);
//	logMsg("      Streams: ISS=%d OSS=%d BSS=%d\n", mInStreamsSup, mOutStreamsSup, mBiStreamsSup);

	ASSERT(mCorbSize);
	ASSERT(mRirbSize);

	result = true;
	return result;
}

/******************************************************************************************/
/******************************************************************************************/

UInt8 VoodooHDADevice::readData8(UInt32 offset)
{
	return *(volatile UInt8 *) ((UInt8 *) mRegBase + offset);
}

UInt16 VoodooHDADevice::readData16(UInt32 offset)
{
	return *(volatile UInt16 *) ((UInt8 *) mRegBase + offset);
}

UInt32 VoodooHDADevice::readData32(UInt32 offset)
{
	return *(volatile UInt32 *) ((UInt8 *) mRegBase + offset);
}

void VoodooHDADevice::writeData8(UInt32 offset, UInt8 value)
{
	*(volatile UInt8 *) ((UInt8 *) mRegBase + offset) = value;
}

void VoodooHDADevice::writeData16(UInt32 offset, UInt16 value)
{
	*(volatile UInt16 *) ((UInt8 *) mRegBase + offset) = value;
}

void VoodooHDADevice::writeData32(UInt32 offset, UInt32 value)
{
	*(volatile UInt32 *) ((UInt8 *) mRegBase + offset) = value;
}

/******************************************************************************************/
/******************************************************************************************/

void VoodooHDADevice::lockMsgBuffer()
{
	ASSERT(mMessageLock);
	IOLockLock(mMessageLock);
}

void VoodooHDADevice::unlockMsgBuffer()
{
	ASSERT(mMessageLock);
	IOLockUnlock(mMessageLock);
}

void VoodooHDADevice::enableMsgBuffer(bool isEnabled)
{
	if (mMsgBufferEnabled == isEnabled) {
		errorMsg("warning: enableMsgBuffer(%d) has no effect\n", isEnabled);
		return;
	}

	lockMsgBuffer();
	mMsgBufferEnabled = isEnabled;
	if (isEnabled) {
		bzero(mMsgBuffer, mMsgBufferSize);
		mMsgBufferPos = 0;
	}
	unlockMsgBuffer();
}

void VoodooHDADevice::logMsg(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	messageHandler(kVoodooHDAMessageTypeGeneral, format, args);
	va_end(args);
}

void VoodooHDADevice::errorMsg(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	messageHandler(kVoodooHDAMessageTypeError, format, args);
	va_end(args);
}

void VoodooHDADevice::dumpMsg(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	messageHandler(kVoodooHDAMessageTypeDump, format, args);
	va_end(args);
}

void VoodooHDADevice::messageHandler(UInt32 type, const char *format, va_list args)
{
	bool lockExists;
	int length;

	ASSERT(type);
	ASSERT(format);

	lockExists = (!isInactive() && mMessageLock);
	if (lockExists)
		lockMsgBuffer();

	switch (type) {
	case kVoodooHDAMessageTypeGeneral:
		if (mVerbose < 1)
			break;
		/* fall through */
	case kVoodooHDAMessageTypeError: {
		va_list consoleArgs;
		va_copy(consoleArgs, args);
		vprintf(format, consoleArgs);
		va_end(consoleArgs);
		break;
	}
	case kVoodooHDAMessageTypeDump:
		if (mVerbose >= 2) {
			va_list consoleArgs;
			va_copy(consoleArgs, args);
			vprintf(format, consoleArgs);
			va_end(consoleArgs);
		}
		if (lockExists && mMsgBufferEnabled && mMsgBuffer && mMsgBufferSize > 1) {
			if (mMsgBufferPos >= (mMsgBufferSize - 1)) {
				mMsgBufferPos = mMsgBufferSize - 1;
				mMsgBuffer[mMsgBufferPos] = '\0';
				break;
			}
			va_list bufferArgs;
			va_copy(bufferArgs, args);
			length = vsnprintf(mMsgBuffer + mMsgBufferPos,
			                   mMsgBufferSize - mMsgBufferPos,
			                   format, bufferArgs);
			va_end(bufferArgs);
			if (length > 0) {
				size_t remaining = mMsgBufferSize - mMsgBufferPos;
				if ((size_t)length >= remaining)
					mMsgBufferPos = mMsgBufferSize - 1;
				else
					mMsgBufferPos += length;
			} else if (length < 0) {
				IOLog("warning: vsnprintf in messageHandler failed\n");
			}
		}
		break;
	default:
		BUG("unknown message type");
	}

	if (lockExists)
		unlockMsgBuffer();
}

IOReturn VoodooHDADevice::runAction(UInt32 *action, UInt32 *outSize, void **outData, void *extraArg)
{
	//logMsg("VoodooHDADevice[%p]::runAction(0x%lx, %p, %p, %p)\n", this, *action, outSize, outData, extraArg);

	if (!action || !outSize || !outData)
		return kIOReturnBadArgument;

	*outSize = 0;
	*outData = NULL;

	if (isInactive())
		return kIOReturnNoDevice;
	if (!commandGate || !mActionHandler)
		return kIOReturnNotReady;

	return commandGate->runAction(mActionHandler, action, (UInt32 *) outSize, (void *) outData,
			extraArg);
}

__attribute__((visibility("hidden")))
IOReturn VoodooHDADevice::handleAction(OSObject *owner, void *arg0, void *arg1, void *arg2,
		__unused void *arg3)
{
	VoodooHDADevice *device;
	IOReturn result = kIOReturnSuccess;
	UInt32 action;
	UInt32 *outSize;
	void **outData;

	device = OSDynamicCast(VoodooHDADevice, owner);
	if (!device)
		return kIOReturnBadArgument;
	if (!arg0 || !arg1 || !arg2)
		return kIOReturnBadArgument;

	action = *static_cast<UInt32 const*>(arg0);
	outSize = (UInt32 *) arg1;
	outData = (void **) arg2;
	*outSize = 0;
	*outData = NULL;

	//device->logMsg("VoodooHDADevice[%p]::handleAction(0x%lx, %p, %p)\n", owner, action, outSize, outData);

	if((action & 0xFF)  == kVoodooHDAActionSetMixer) {
		 //Команда от PrefPanel
		UInt8 value;  // slide value
		UInt8 sliderNum;  //OSS device
		UInt8 tabNum; //Channel number

		tabNum = ((action >> 8) & 0xFF);
		sliderNum = ((action >> 16) & 0xFF);
		value = ((action >> 24) & 0xFF);

		device->changeSliderValue(tabNum, sliderNum, value);

		*outSize = 0;
		*outData = NULL;

		return result;
	}

	if((action & 0xFF)  == kVoodooHDAActionSetMath) {
		UInt8 ch, opt, val;
		ch = ((action >> 8) & 0xFF);
		opt = ((action >> 16) & 0xFF);
		val = ((action >> 24) & 0xFF);
		//IOLog("HDA: Channel=%02x Options=%02x Value=%02x\n", ch, opt, val);
			  //device->vectorize?"Yes":"No", device->noiseLevel,
			  //device->useStereo?"Yes":"No", device->StereoBase);


		if (device->mPrefPanelMemoryBuf && ch < device->nSliderTabsCount) {
			device->lockPrefPanelMemoryBuf();
			device->mPrefPanelMemoryBuf[ch].vectorize = ((opt & 0x1) == 1);
			device->mPrefPanelMemoryBuf[ch].noiseLevel = (val & 0x0F);
			device->mPrefPanelMemoryBuf[ch].useStereo = ((opt & 0x2) == 2);
			device->mPrefPanelMemoryBuf[ch].StereoBase = (val & 0xF0) >> 4;
			device->unlockPrefPanelMemoryBuf();
		}

		device->setMath(ch, opt, val);
/*
		device->vectorize = ((opt & 0x1) == 1);
		device->noiseLevel = (val & 0x0F);
		device->useStereo = ((opt & 0x2) == 2);
		device->StereoBase = (val & 0xF0) >> 4;
*/
		*outSize = 0;
		*outData = NULL;

		return result;		
		}

	if ((action & 0xFF) == kVoodooHDAActionSetDiag) {
#if !VOODOO_HDA_DEBUG_BUILD
		*outSize = 0;
		*outData = NULL;
		return kIOReturnUnsupported;
#else
		UInt8 ch;
		UInt16 flags;

		ch = ((action >> 8) & 0xFF);
		flags = static_cast<UInt16>(((action >> 16) & 0xFF) | (((action >> 24) & 0xFF) << 8));

		if (device->mPrefPanelMemoryBuf && ch < device->nSliderTabsCount) {
			device->lockPrefPanelMemoryBuf();
			device->mPrefPanelMemoryBuf[ch].diagnosticFlags = flags;
			device->unlockPrefPanelMemoryBuf();
		}

		device->setDiagnosticFlags(ch, flags);

		*outSize = 0;
		*outData = NULL;

		return result;
#endif
	}

	if ((action & 0xFF) == kVoodooHDAActionSetDebug) {
#if !VOODOO_HDA_DEBUG_BUILD
		*outSize = 0;
		*outData = NULL;
		return kIOReturnUnsupported;
#else
		UInt8 level;

		level = ((action >> 24) & 0xFF);
		device->setDebugLevel(level);
		*outSize = 0;
		*outData = NULL;
		return result;
#endif
	}

	if ((action & 0xFF) == kVoodooHDAActionGetDiagTelemetry) {
#if !VOODOO_HDA_DEBUG_BUILD
		*outSize = 0;
		*outData = NULL;
		return kIOReturnUnsupported;
#else
		UInt8 ch = ((action >> 8) & 0xFF);
		if (!device->getDiagnosticTelemetry(ch, &device->mDiagTelemetry)) {
			*outSize = 0;
			*outData = NULL;
			return kIOReturnBadArgument;
		}
		*outSize = sizeof(device->mDiagTelemetry);
		*outData = &device->mDiagTelemetry;
		return result;
#endif
	}

	//Команда от моей версии getDump для обновления данных об усилении
	if((action & 0xFF)  == kVoodooHDAActionGetMixers) {

//		device->LOCK();
		device->updateExtDump();
//		device->UNLOCK();

		*outSize = 0;
		*outData = NULL;

		return result;
	}


	switch (action) {
	case kVoodooHDAActionTest:
		device->logMsg("test action received\n");
		*outSize = 0;
		*outData = NULL;
		break;
	default:
		result = kIOReturnBadArgument;
		*outSize = 0;
		*outData = NULL;
	}

	return result;
}
// from v0.2.2
/******************************************************************************************/
ChannelInfo *VoodooHDADevice::getChannelInfo() {
	int ossDev=1, i=0;
	ChannelInfo *info = (ChannelInfo*)allocMem(sizeof(ChannelInfo) * mNumChannels);
	VoodooHDAEngine *engine;
	const char *pName;
	
	for(; i < mNumChannels; i++) {
		engine = lookupEngine(i);
		if (!engine)
			continue;
		pName = engine->getPortName();
		snprintf(info[i].name, strlen(pName)+1, "%s", pName);
		info[i].numChannels = mNumChannels;

		// initialise
		// We dont want to control Master Volume in our PrefPane -> we start at ossDev=1
		for(ossDev = 1 ; ossDev < SOUND_MIXER_NRDEVICES ; ossDev++) {
			const char *name;
			UInt32 ossMask;
			//name = engine->getOssDevName(ossDev);
			name = engine->mPortName;
			
			if (engine->getEngineDirection() == kIOAudioStreamDirectionOutput)
				ossMask = engine->mChannel->pcmDevice->devMask;
			else
				ossMask = engine->mChannel->pcmDevice->recDevMask;
			
			info[i].mixerValues[ossDev-1].enabled = false;			
			if ((ossMask & (1 << ossDev)) == 0)
				continue;
			
			info[i].mixerValues[ossDev-1].mixId = ossDev;
			info[i].mixerValues[ossDev-1].enabled = true;
			info[i].mixerValues[ossDev-1].value = engine->mChannel->pcmDevice->left[ossDev];// mMixerDefaults[ossDev];
			snprintf(info[i].mixerValues[ossDev-1].name, strlen(name)+1, "%s", name);
		}
		info[i].mixerValues[24].mixId = 0;
		info[i].mixerValues[24].enabled = true;
		info[i].mixerValues[24].value = engine->mChannel->pcmDevice->left[0];// mMixerDefaults[ossDev];
		info[i].vectorize = engine->mChannel->vectorize;
		info[i].noiseLevel = engine->mChannel->noiseLevel;
		info[i].useStereo = engine->mChannel->useStereo;
		info[i].StereoBase = engine->mChannel->StereoBase;
		info[i].digital = engine->mChannel->pcmDevice ? engine->mChannel->pcmDevice->digital : 0;
		info[i].direction = static_cast<SInt8>(engine->mChannel->direction);
		info[i].diagnosticFlags = engine->mChannel->diagnosticFlags;
		info[i].debugLevel = static_cast<UInt8>(mVerbose & 0xff);
		info[i].buildFlags = VOODOO_HDA_DEBUG_BUILD ? kVoodooHDABuildSupportsDebug : 0;
	}
	
	return info;
}

/******************************************************************************************/
/******************************************************************************************/

bool VoodooHDADevice::setupWorkloop()
{
	//logMsg("VoodooHDADevice[%p]::setupWorkloop\n", this);

	if (mWorkLoop)
		return true;

	mWorkLoop = IOWorkLoop::workLoop(); // create our own workloop (super has workLoop member)
	if (!mWorkLoop) {
		errorMsg("error: couldn't allocate workloop\n");
		return false;
	}

	mTimerSource = IOTimerEventSource::timerEventSource(this,
			(IOTimerEventSource::Action) &VoodooHDADevice::timeoutOccurred);
	if (!mTimerSource) {
		errorMsg("error: couldn't allocate timer event source\n");
		mWorkLoop->release();
		mWorkLoop = NULL;
		return false;
	}
	mTimerSource->disable();
	if (mWorkLoop->addEventSource(mTimerSource) != kIOReturnSuccess) {
		errorMsg("error: couldn't add timer event source to workloop\n");
		mTimerSource->release();
		mTimerSource = NULL;
		mWorkLoop->release();
		mWorkLoop = NULL;
		return false;
	}
	mTimerSource->setTimeoutMS(5000);

	IOService* _provider = getProvider();
	if (!_provider) {
		errorMsg("error: missing provider for interrupt event source\n");
		mWorkLoop->removeEventSource(mTimerSource);
		mTimerSource->release();
		mTimerSource = NULL;
		mWorkLoop->release();
		mWorkLoop = NULL;
		return false;
	}
	mInterruptSource = IOFilterInterruptEventSource::filterInterruptEventSource(this,
			(IOInterruptEventAction) &VoodooHDADevice::interruptHandler,
			(IOFilterInterruptEventSource::Filter) &VoodooHDADevice::interruptFilter,
			_provider, findInterruptIndex(_provider, mAllowMSI));
	if (!mInterruptSource) {
		errorMsg("error: couldn't allocate interrupt event source\n");
		mWorkLoop->removeEventSource(mTimerSource);
		mTimerSource->release();
		mTimerSource = NULL;
		mWorkLoop->release();
		mWorkLoop = NULL;
		return false;
	}
	mInterruptSource->disable();
	if (mWorkLoop->addEventSource(mInterruptSource) != kIOReturnSuccess) {
		errorMsg("error: couldn't add interrupt event source to workloop\n");
		mInterruptSource->release();
		mInterruptSource = NULL;
		mWorkLoop->removeEventSource(mTimerSource);
		mTimerSource->release();
		mTimerSource = NULL;
		mWorkLoop->release();
		mWorkLoop = NULL;
		return false;
	}

	return true;
}

__attribute__((noinline, visibility("hidden")))
int VoodooHDADevice::findInterruptIndex(IOService* target, bool allowMSI)
{
	int source, interruptType, pinInterruptToUse, msgInterruptToUse;
	bool isMsgInterruptFound;

	isMsgInterruptFound = false;
	pinInterruptToUse = 0;
	msgInterruptToUse = 0;
	for (interruptType = 0, source = 0;
		 target->getInterruptType(source, &interruptType) == kIOReturnSuccess;
		 ++source)
		if (interruptType & kIOInterruptTypePCIMessaged) {
			if (!isMsgInterruptFound) {
				isMsgInterruptFound = true;
				msgInterruptToUse = source;
			}
		} else
			pinInterruptToUse = source;
	return (isMsgInterruptFound && allowMSI) ? msgInterruptToUse : pinInterruptToUse;
}

void VoodooHDADevice::enableEventSources()
{
	//logMsg("VoodooHDADevice[%p]::enableEventSources\n", this);

	if (mInterruptSource)
		mInterruptSource->enable();
	if (mTimerSource)
		mTimerSource->enable();
}

void VoodooHDADevice::disableEventSources()
{
	//logMsg("VoodooHDADevice[%p]::disableEventSources\n", this);

	if (mTimerSource)
		mTimerSource->disable();
	if (mInterruptSource)
		mInterruptSource->disable();
}

__attribute__((visibility("hidden")))
bool VoodooHDADevice::interruptFilter(OSObject *owner, __unused IOFilterInterruptEventSource *source)
{
	VoodooHDADevice *device;
	UInt32 status;

#if 0
	device = OSDynamicCast(VoodooHDADevice, owner);
#else
	/*
	 * Not sure OSDynamicCast is safe in interrupt context, and
	 *   anyhow, this function will never get called with any
	 *   other OSObject. - Zenith432
	 */
	device = static_cast<VoodooHDADevice*>(owner);
#endif
	if (!device)
		return false;

	status = *(UInt32 volatile*) ((UInt8 *) device->mRegBase + HDAC_INTSTS);
	if (!HDA_FLAG_MATCH(status, HDAC_INTSTS_GIS))
		return false;
	/*
	 * Note: If multiple stream primary interrupts take place
	 *   before threaded handler, last time-stamp is used.
	 */
	if (status & HDAC_INTSTS_SIS_MASK)
		device->mIntrTimeStamp = mach_absolute_time();
	*(UInt32 volatile*) ((UInt8 *) device->mRegBase + HDAC_INTSTS) = status;
	OSBitOrAtomic(status, &device->mIntStatus);

	return true;
}

__attribute__((visibility("hidden")))
void VoodooHDADevice::interruptHandler(OSObject *owner, __unused IOInterruptEventSource *source,
		__unused int count)
{
#if 0
	VoodooHDADevice *device = OSDynamicCast(VoodooHDADevice, owner);
#else
	/*
	 * Not interrupt context anymore, but OSDynamicCast
	 *   still not needed - Zenith432.
	 */
	VoodooHDADevice *device = static_cast<VoodooHDADevice*>(owner);
#endif
	if (!device)
		return;
	device->handleInterrupt();
}

LIBKERN_RETURNS_NOT_RETAINED VoodooHDAEngine *VoodooHDADevice::lookupEngine(int channelId)
{
	OSCollectionIterator *engineIter;
	VoodooHDAEngine *engine = NULL;

	engineIter = OSCollectionIterator::withCollection(audioEngines);
	if (!engineIter)
		return NULL;
	engineIter->reset();
	while ((engine = OSDynamicCast(VoodooHDAEngine, engineIter->getNextObject()))) {
//		ASSERT(OSDynamicCast(VoodooHDAEngine, engine));
		if (engine->getEngineId() == channelId)
			break;
	}
	RELEASE(engineIter);

	return engine;
}

void VoodooHDADevice::handleChannelInterrupt(int channelId)
{
	VoodooHDAEngine *engine;

	mTotalChanInt++;

	engine = lookupEngine(channelId);
	if (!engine) {
		errorMsg("warning: couldn't find engine matching channel %d\n", channelId);
		return;
	}
	if (mGFXController && mGFXController->ownsChannel(engine->mChannel))
		return;
	engine->takeTimeStamp(true, reinterpret_cast<AbsoluteTime*>(&mIntrTimeStamp));
}

/******************************************************************************************/
/******************************************************************************************/

void VoodooHDADevice::lock(const char *callerName)
{
	if (mVerbose >= 4)
		logMsg("VoodooHDADevice[%p]::lock(%s)\n", this, callerName);
	//ASSERT(mLock);
	IOLockLock(mLock);
}

void VoodooHDADevice::unlock(const char *callerName)
{
	if (mVerbose >= 4)
		logMsg("VoodooHDADevice[%p]::unlock(%s)\n", this, callerName);
	//ASSERT(mLock);
	IOLockUnlock(mLock);
}

void VoodooHDADevice::assertLock(IOLock *lock, UInt32 type)
{
	lck_mtx_t *mutex;
	ASSERT(lock);
	ASSERT(type); // type can be either LCK_MTX_ASSERT_OWNED or LCK_MTX_ASSERT_NOTOWNED
	mutex = IOLockGetMachLock(lock);
	ASSERT(mutex);
//	lck_mtx_assert(mutex, type);
}

extern "C" {
	extern void *kern_os_malloc(size_t size);
	extern void *kern_os_realloc(void *addr, size_t size);
	extern void kern_os_free(void *addr);
}

__attribute__((visibility("hidden")))
void *VoodooHDADevice::allocMem(size_t size)
{
	void *addr = kern_os_malloc(size);
//	ASSERT(addr); //will check result
	return addr;
}

__attribute__((visibility("hidden")))
void *VoodooHDADevice::reallocMem(void *addr, size_t size)
{
	void *newAddr = kern_os_realloc(addr, size);
//	ASSERT(newAddr); //will check result
	return newAddr;
}

__attribute__((visibility("hidden")))
void VoodooHDADevice::freeMem(void *addr)
{
	if (!addr)
		return;
	kern_os_free(addr);
}

DmaMemory *VoodooHDADevice::allocateDmaMemory(mach_vm_size_t size, const char *description, UInt32 cacheOption)
{
	mach_vm_address_t physMask;
	IOBufferMemoryDescriptor *memDesc = NULL;
	DmaMemory *dmaMemory = NULL;
	UInt64 segAddr;
	IOByteCount segLength;
	void* virtAddr;

	if (!size || !description) {
		errorMsg("error: invalid DMA allocation request\n");
		return NULL;
	}

//	logMsg("VoodooHDADevice::allocateDmaMemory(%llu, %s)\n", size, description);

	if (mSupports64Bit)
		physMask = -HDAC_DMA_ALIGNMENT;
	else
		physMask = -HDAC_DMA_ALIGNMENT & UINT32_MAX;
	cacheOption &= kIOMapCacheMask;
	if (!cacheOption) {
		cacheOption = mInhibitCache ? kIOMapInhibitCache : kIOMapDefaultCache;
    }
	memDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
			kIOMemoryPhysicallyContiguous | kIODirectionInOut | cacheOption, size, physMask);
	
	if (!memDesc) {
		errorMsg("error: IOBufferMemoryDescriptor::inTaskWithPhysicalMask failed\n");
		goto failed;
	}
	if (memDesc->getLength() < size) {
		errorMsg("error: DMA descriptor shorter than requested for %s (%llu < %llu)\n",
				description, (UInt64)memDesc->getLength(), size);
		goto failed;
	}

	/*
	 * Note: memDesc is kIOMemoryAutoPrepare, but just to conform...
	 */
	if (memDesc->prepare() != kIOReturnSuccess) {
		errorMsg("error: IOMemoryDescriptor::prepare failed\n");
		goto failed;
	}

	segAddr = memDesc->getPhysicalSegment(0U, &segLength, 0U);
	if (!segAddr) {
		errorMsg("error: IOBufferMemoryDescriptor::getPhysicalSegment failed\n");
		memDesc->complete();
		goto failed;
	}
	if (segLength < size) {
		errorMsg("error: DMA physical segment shorter than requested for %s (%llu < %llu)\n",
				description, (UInt64)segLength, size);
		memDesc->complete();
		goto failed;
	}

	virtAddr = memDesc->getBytesNoCopy();
	if (!virtAddr) {
		errorMsg("error: DMA virtual mapping failed for %s\n", description);
		memDesc->complete();
		goto failed;
	}
	bzero(virtAddr, size);

	dmaMemory = new DmaMemory;
	if (!dmaMemory) {
		errorMsg("error: couldn't allocate DMA bookkeeping for %s\n", description);
		memDesc->complete();
		goto failed;
	}
	dmaMemory->description = description;
	dmaMemory->md = memDesc;
	dmaMemory->size = size;
	dmaMemory->physAddr = segAddr;
	dmaMemory->virtAddr = reinterpret_cast<IOVirtualAddress>(virtAddr);

//	logMsg("%s: allocated %llu bytes DMA memory (phys: %#llx, virt: %p)\n",
//			dmaMemory->description, dmaMemory->size, dmaMemory->physAddr, virtAddr);

	return dmaMemory;

failed:
	RELEASE(memDesc);
	DELETE(dmaMemory);

	return NULL;
}

void VoodooHDADevice::freeDmaMemory(DmaMemory *dmaMemory)
{
	if (!dmaMemory)
		return;

	dmaMemory->description = NULL;
	dmaMemory->size = 0;
	dmaMemory->physAddr = 0;
	dmaMemory->virtAddr = 0;

	if (dmaMemory->md)
		dmaMemory->md->complete();
	RELEASE(dmaMemory->md);

	DELETE(dmaMemory);
}

/******************************************************************************************/
/******************************************************************************************/

/*
 * Wrapper function that sends only one command to a given codec
 */
UInt32 VoodooHDADevice::sendCommand(UInt32 verb, nid_t cad)
{
	CommandList cmdList;
	UInt32 response = HDAC_INVALID;

	//assertLock(mLock, LCK_MTX_ASSERT_OWNED);

	cmdList.numCommands = 1;
	cmdList.verbs = &verb;
	cmdList.responses = &response;

	sendCommands(&cmdList, cad);

	return response;
}

/*
 * Send a command list to the codec via the corb. We queue as much verbs as
 * we can and msleep on the codec. When the interrupt get the responses
 * back from the rirb, it will wake us up so we can queue the remaining verbs
 * if any.
 */
void VoodooHDADevice::sendCommands(CommandList *commands, nid_t cad)
{
	Codec *codec;
	int corbReadPtr;
	UInt32 *corb;
	int timeout;
	int retry = 10;

	if ((cad < 0) || (cad >= HDAC_CODEC_MAX) || !mCodecs[cad] ||
	    !commands || (commands->numCommands < 1))
		return;

	codec = mCodecs[cad];
	codec->commands = commands;
	codec->numRespReceived = 0;
	codec->numVerbsSent = 0;
	corb = (UInt32 *) mCorbMem->virtAddr;

	do {
		if (codec->numVerbsSent != commands->numCommands) {
			/* Queue as many verbs as possible */
			corbReadPtr = readData16(HDAC_CORBRP);
			while ((codec->numVerbsSent != commands->numCommands) &&
					(((mCorbWritePtr + 1) % mCorbSize) != corbReadPtr)) {
				mCorbWritePtr++;
				mCorbWritePtr %= mCorbSize;
				corb[mCorbWritePtr] = commands->verbs[codec->numVerbsSent++];
			}

			/* Send the verbs to the codecs */
			writeData16(HDAC_CORBWP, mCorbWritePtr);
		}

#if 0
		/*
		 * Lock testing code
		 */
		if (IOLockTryLock(mLock)) {
			IOLog("%s: Was unlocked, should have been locked!!\n", __FUNCTION__);
			IOLockUnlock(mLock);
		}
#endif
		timeout = 200;
		while ((rirbFlush() == 0) && --timeout)
			IODelay(10);
	} while (((codec->numVerbsSent != commands->numCommands) ||
			(codec->numRespReceived != commands->numCommands)) && --retry);

	if (retry == 0)
		errorMsg("TIMEOUT numcmd=%d, sent=%d, received=%d\n", commands->numCommands,
				codec->numVerbsSent, codec->numRespReceived);

	codec->commands = NULL;
	codec->numRespReceived = 0;
	codec->numVerbsSent = 0;

	unsolqFlush();
}

/*
 * Initialize the corb registers for operations but do not start it up yet.
 * The CORB engine must not be running when this function is called.
 */
void VoodooHDADevice::initCorb()
{
	UInt8 corbSizeReg = 0;
	UInt64 corbPhysAddr;

//	logMsg("VoodooHDADevice[%p]::initCorb\n", this);

	/* Setup the CORB size. */
	switch (mCorbSize) {
	case 256:
		corbSizeReg = HDAC_CORBSIZE_CORBSIZE(HDAC_CORBSIZE_CORBSIZE_256);
		break;
	case 16:
		corbSizeReg = HDAC_CORBSIZE_CORBSIZE(HDAC_CORBSIZE_CORBSIZE_16);
		break;
	case 2:
		corbSizeReg = HDAC_CORBSIZE_CORBSIZE(HDAC_CORBSIZE_CORBSIZE_2);
		break;
	default:
		errorMsg("error: invalid CORB size %d\n", mCorbSize);
		return;
	}
	writeData8(HDAC_CORBSIZE, corbSizeReg);

	/* Setup the CORB Address in the hdac */
	corbPhysAddr = (uint64_t)mCorbMem->physAddr;
	writeData32(HDAC_CORBLBASE, (UInt32) corbPhysAddr);
	writeData32(HDAC_CORBUBASE, (UInt32) (corbPhysAddr >> 32));

	/* Set the WP and RP */
	mCorbWritePtr = 0;
	writeData16(HDAC_CORBWP, mCorbWritePtr);
	writeData16(HDAC_CORBRP, HDAC_CORBRP_CORBRPRST);
	/* The HDA specification indicates that the CORBRPRST bit will always
	 * read as zero. Unfortunately, it seems that at least the 82801G
	 * doesn't reset the bit to zero, which stalls the corb engine.
	 * manually reset the bit to zero before continuing. */
	writeData16(HDAC_CORBRP, 0);

#if 0
	/* Enable CORB error reporting */
	writeData8(HDAC_CORBCTL, HDAC_CORBCTL_CMEIE);
#endif
}

/*
 * Initialize the rirb registers for operations but do not start it up yet.
 * The RIRB engine must not be running when this function is called.
 */
void VoodooHDADevice::initRirb()
{
	UInt8 rirbSizeReg = 0;
	UInt64 rirbPhysAddr;

//	logMsg("VoodooHDADevice[%p]::initRirb\n", this);

	/* Setup the RIRB size. */
	switch (mRirbSize) {
	case 256:
		rirbSizeReg = HDAC_RIRBSIZE_RIRBSIZE(HDAC_RIRBSIZE_RIRBSIZE_256);
		break;
	case 16:
		rirbSizeReg = HDAC_RIRBSIZE_RIRBSIZE(HDAC_RIRBSIZE_RIRBSIZE_16);
		break;
	case 2:
		rirbSizeReg = HDAC_RIRBSIZE_RIRBSIZE(HDAC_RIRBSIZE_RIRBSIZE_2);
		break;
	default:
		errorMsg("error: invalid RIRB size %d\n", mRirbSize);
		return;
	}
	writeData8(HDAC_RIRBSIZE, rirbSizeReg);

	/* Setup the RIRB Address in the hdac */
	rirbPhysAddr = mRirbMem->physAddr;
	writeData32(HDAC_RIRBLBASE, (UInt32) rirbPhysAddr);
	writeData32(HDAC_RIRBUBASE, (UInt32) (rirbPhysAddr >> 32));

	/* Setup the WP and RP */
	mRirbReadPtr = 0;
	writeData16(HDAC_RIRBWP, HDAC_RIRBWP_RIRBWPRST);

	/* Setup the interrupt threshold */
	writeData16(HDAC_RINTCNT, mRirbSize / 2);

	/* Enable Overrun and response received reporting */
#if 0
	writeData8(HDAC_RIRBCTL, HDAC_RIRBCTL_RIRBOIC | HDAC_RIRBCTL_RINTCTL);
#else
	writeData8(HDAC_RIRBCTL, HDAC_RIRBCTL_RINTCTL);
#endif
}

/*
 * Startup the corb DMA engine
 */
void VoodooHDADevice::startCorb()
{
	UInt32 corbCtl;
	corbCtl = readData8(HDAC_CORBCTL);
	corbCtl |= HDAC_CORBCTL_CORBRUN;
	writeData8(HDAC_CORBCTL, corbCtl);
}

/*
 * Startup the rirb DMA engine
 */
void VoodooHDADevice::startRirb()
{
	UInt32 rirbCtl;
	rirbCtl = readData8(HDAC_RIRBCTL);
	rirbCtl |= HDAC_RIRBCTL_RIRBDMAEN;
	writeData8(HDAC_RIRBCTL, rirbCtl);
}

/********************************************************************************************/
/********************************************************************************************/

int VoodooHDADevice::rirbFlush()
{
	RirbResponse *rirbBase;
	UInt8 rirbWritePtr;
	int ret;

	rirbBase = (RirbResponse *) mRirbMem->virtAddr;
	rirbWritePtr = readData8(HDAC_RIRBWP);

	ret = 0;
	while (mRirbReadPtr != rirbWritePtr) {
		RirbResponse *rirb;
		Codec *codec;
		CommandList *commands;
		nid_t cad;
		UInt32 resp;

		mRirbReadPtr++;
		mRirbReadPtr %= mRirbSize;
		rirb = &rirbBase[mRirbReadPtr];
		cad = HDAC_RIRB_RESPONSE_EX_SDATA_IN(rirb->response_ex);
		if ((cad < 0) || (cad >= HDAC_CODEC_MAX) || !mCodecs[cad])
			continue;
		resp = rirb->response;
		codec = mCodecs[cad];
		commands = codec->commands;
		if (rirb->response_ex & HDAC_RIRB_RESPONSE_EX_UNSOLICITED) {
			/* Store both the tag (bits [31:26]) and the full response
			 * so HDMI/DP flag bits [1:0] are available to handleUnsolicited().
			 * Queue format: even slot = (cad << 16) | tag, odd slot = resp */
			mUnsolq[mUnsolqWritePtr++] = (cad << 16) | ((resp >> 26) & 0xffff);
			mUnsolqWritePtr %= HDAC_UNSOLQ_MAX;
			mUnsolq[mUnsolqWritePtr++] = resp;
			mUnsolqWritePtr %= HDAC_UNSOLQ_MAX;
		} else if (commands && (commands->numCommands > 0) &&
				(codec->numRespReceived < commands->numCommands))
			commands->responses[codec->numRespReceived++] = resp;
		ret++;
	}
		//Slice
	UInt32 rirbCtl;
	rirbCtl = readData8(HDAC_GCTL);
	rirbCtl |= HDAC_GCTL_FCNTRL;
	writeData8(HDAC_GCTL, rirbCtl);
	
	return ret;
}

int VoodooHDADevice::unsolqFlush()
{
	int ret = 0;

	if (mUnsolqState == HDAC_UNSOLQ_READY) {
		mUnsolqState = HDAC_UNSOLQ_BUSY;
		while (mUnsolqReadPtr != mUnsolqWritePtr) {
			nid_t cad;
			UInt32 tag, resp;
			cad = mUnsolq[mUnsolqReadPtr] >> 16;
			tag = mUnsolq[mUnsolqReadPtr++] & 0xffff;
			mUnsolqReadPtr %= HDAC_UNSOLQ_MAX;
			resp = mUnsolq[mUnsolqReadPtr++];
			mUnsolqReadPtr %= HDAC_UNSOLQ_MAX;
			if ((cad >= 0) && (cad < HDAC_CODEC_MAX) && mCodecs[cad])
				handleUnsolicited(mCodecs[cad], tag, resp);
			ret++;
		}
		mUnsolqState = HDAC_UNSOLQ_READY;
	}

	return ret;
}

/*
 * Unsolicited messages handler.
 * For HDMI/DP pins, resp bits [1:0] carry flags:
 *   bit 0 = presence change, bit 1 = ELD/status change.
 * For analog pins, only presence (bit 0) is meaningful.
 * (Based on FreeBSD hdaa_unsol_intr)
 */
void VoodooHDADevice::updateHDMIEnginePresence()
{
	/* First pass: read pin sense for all engines and find which have presence.
	 * ATI codecs may report stale presence on previously-connected pins,
	 * so count total presence to detect the "cable moved" scenario. */
	bool presence[16] = {};
	UInt32 pinSenses[16] = {};
	int presenceCount = 0;
	int firstPresenceIdx = -1;
	nid_t presentPins[16] = {};
	int presentPinCount = 0;
	nid_t preferredPin = (nid_t)-1;
	bool havePreferredPin = false;
	int hdmiEngineCount = (mNumHDMIEngines > 16) ? 16 : mNumHDMIEngines;
	bool legacyPolarisRecovery = shouldUseAmdLegacyPolarisHDMIFallback(mFBNotifier, mPciNub, mDeviceId);

	if (hdmiEngineCount < 0)
		hdmiEngineCount = 0;

	for (int i = 0; i < hdmiEngineCount; i++) {
		HDMIEngineSlot *slot = &mHDMIEngines[i];
		if (!slot->engine) continue;
		slot->mirrorCandidate = false;
		if (slot->cad < 0 || slot->cad >= HDAC_CODEC_MAX) {
			presence[i] = false;
			continue;
		}
		pinSenses[i] = sendCommand(HDA_CMD_GET_PIN_SENSE(slot->cad, slot->pinNid), slot->cad);
		Codec *codec = mCodecs[slot->cad];
		bool hasPresence = (pinSenses[i] & (1U << 31)) != 0;
		bool eldValid = (pinSenses[i] & HDA_CMD_GET_PIN_SENSE_ELD_VALID) != 0;
		if (!hasPresence && codec && isAtiHdmiCodec(codec) &&
		    appleGfxHdaAmdUsesCachedELDPresence(codec->deviceId) && eldValid)
			hasPresence = true;
		presence[i] = hasPresence;
		if (presence[i]) {
			presenceCount++;
			if (presentPinCount < 16)
				presentPins[presentPinCount++] = slot->pinNid;
			if (firstPresenceIdx < 0)
				firstPresenceIdx = i;
		}
	}

	/* If the framebuffer notifier has EDID for a concrete IOFramebuffer, let it
	 * choose the physical HDA pin.  Some Polaris cards report no reliable
	 * HDA pin-sense bits until the display pipe is enabled, while others report
	 * stale ELD/presence on every pin.  Therefore query the notifier even when
	 * presenceCount is 0 or 1 and pass the full ATI HDMI pin list as fallback. */
	if (mFBNotifier && hdmiEngineCount > 0) {
		nid_t candidatePins[16] = {};
		int candidatePinCount = 0;
		int cad = -1;
		if (presentPinCount > 0) {
			for (int p = 0; p < presentPinCount && candidatePinCount < 16; p++)
				candidatePins[candidatePinCount++] = presentPins[p];
		}
		for (int i = 0; i < hdmiEngineCount; i++) {
			HDMIEngineSlot *slot = &mHDMIEngines[i];
			Codec *codec = (slot->cad >= 0 && slot->cad < HDAC_CODEC_MAX) ? mCodecs[slot->cad] : NULL;
			if (!codec || !isAtiHdmiCodec(codec))
				continue;
			if (cad < 0)
				cad = slot->cad;
			bool already = false;
			for (int p = 0; p < candidatePinCount; p++) {
				if (candidatePins[p] == slot->pinNid) { already = true; break; }
			}
			if (!already && candidatePinCount < 16)
				candidatePins[candidatePinCount++] = slot->pinNid;
		}
		if (cad >= 0 && candidatePinCount > 0)
			havePreferredPin = mFBNotifier->getPreferredConnectedPin(cad, candidatePins, candidatePinCount, &preferredPin);
	}

	/*
	 * RX4xx/RX5xx/Polaris recovery rule:
	 * If a legacy Polaris-class AMD controller reports stale/cached HDA pins, the
	 * EDID-backed IOFramebuffer index may not be the same as the physical HDA pin
	 * that actually carries audio.  A single forced visible pin can therefore hide
	 * the working route.  Keep one Sound output visible, but mirror stream setup to
	 * every other ATI HDMI candidate pin for this legacy group.
	 *
	 * Do not apply this forced fallback to Navi/RDNA1 RX5500/5600/5700 or
	 * Navi/RDNA2 RX6600/6650/6700/6750/6800/6900/6950; those cards keep the
	 * EDID/framebuffer-driven path because RX6600 is already validated and the
	 * Navi connector order is more reliable than Polaris stale pin-sense.
	 */
	if (legacyPolarisRecovery) {
		if (mVerbose >= 1) {
			IOLog("VoodooHDA ATI DBG: legacy Polaris HDMI recovery single visible with candidate mirrors presenceCount=%d preferred=%d controller=0x%08x family=%s\n",
			      presenceCount, havePreferredPin ? preferredPin : -1, (unsigned)mDeviceId,
			      mFBNotifier ? mFBNotifier->detectedAMDGPUFamilyName() : "HDA fallback");
		}
	}

	/* On modern AMD, once an HDMI engine has been published, keep using that
	 * engine as the single CoreAudio-facing output.  Legacy Polaris recovery
	 * intentionally skips this collapse because the guessed single pin can be
	 * the silent one.
	 */
	nid_t visiblePin = preferredPin;
	bool haveVisiblePin = havePreferredPin;
	if (legacyPolarisRecovery) {
		haveVisiblePin = false;
		visiblePin = (nid_t)-1;
		for (int pass = 0; pass < 2 && !haveVisiblePin; pass++) {
			for (int i = 0; i < hdmiEngineCount; i++) {
				HDMIEngineSlot *slot = &mHDMIEngines[i];
				Codec *codec = (slot->cad >= 0 && slot->cad < HDAC_CODEC_MAX) ? mCodecs[slot->cad] : NULL;
				if (!slot->engine || !codec || !isAtiHdmiCodec(codec))
					continue;
				if (pass == 0 && !presence[i])
					continue;
				visiblePin = slot->pinNid;
				haveVisiblePin = true;
				break;
			}
		}
		if (haveVisiblePin && mVerbose >= 1)
			IOLog("VoodooHDA ATI DBG: legacy Polaris HDMI recovery visible pin=%d with mirrored candidates\n",
			      visiblePin);
	} else {
		for (int i = 0; i < hdmiEngineCount; i++) {
			HDMIEngineSlot *slot = &mHDMIEngines[i];
			Codec *codec = (slot->cad >= 0 && slot->cad < HDAC_CODEC_MAX) ? mCodecs[slot->cad] : NULL;
			if (!slot->engine || !slot->activated || !codec || !isAtiHdmiCodec(codec))
				continue;
			visiblePin = slot->pinNid;
			haveVisiblePin = true;
			break;
		}
		if (!haveVisiblePin && firstPresenceIdx >= 0) {
			visiblePin = mHDMIEngines[firstPresenceIdx].pinNid;
			haveVisiblePin = true;
		}
	}

	/* Second pass: update status and inject ELD */
	for (int i = 0; i < hdmiEngineCount; i++) {
		HDMIEngineSlot *slot = &mHDMIEngines[i];
		if (!slot->engine) continue;
		if (slot->cad < 0 || slot->cad >= HDAC_CODEC_MAX)
			continue;

		Codec *slotCodec = mCodecs[slot->cad];
		bool atiHDMI = slotCodec && isAtiHdmiCodec(slotCodec);
		bool effectivePresence = presence[i];

		/* AMD connected-only policy:
		 * Prefer the pin backed by a real online IOFramebuffer/IODisplay EDID.
		 * This is used for Polaris RX4xx/RX5xx, Navi/RDNA1 RX5xxx and Navi/RDNA2
		 * RX6xxx.  In legacy Polaris recovery mode, publish one ATI HDMI pin and
		 * mirror the hidden candidates instead of forcing only one physical route. */
		if (atiHDMI) {
			if (legacyPolarisRecovery) {
				effectivePresence = (haveVisiblePin && slot->pinNid == visiblePin);
				if (haveVisiblePin && slot->pinNid != visiblePin) {
					slot->mirrorCandidate = true;
					if (mFBNotifier)
						mFBNotifier->injectELDIntoPinIfReady(slot->cad, slot->pinNid);
					if (mVerbose >= 2) {
						IOLog("VoodooHDA ATI DBG: HDMI recovery mirror candidate pin=%d visiblePin=%d\n",
						      slot->pinNid, visiblePin);
					}
				}
			} else if (haveVisiblePin)
				effectivePresence = (slot->pinNid == visiblePin);
			else if (presenceCount > 1 && i != firstPresenceIdx)
				effectivePresence = false;
		}

		bool hasPresence = effectivePresence;
		const char *engineName = setHDMIEngineDisplayName(slot, false);

		if (hasPresence && !slot->activated) {
			if (mVerbose >= 1)
				IOLog("VoodooHDA DBG: HDMI hot-plug: activating engine for pin=%d\n", slot->pinNid);
			IOReturn ret = activateAudioEngine(slot->engine);
			if (mVerbose >= 1)
				IOLog("VoodooHDA DBG: HDMI hot-plug: activateAudioEngine ret=0x%x\n", ret);
			if (ret == kIOReturnSuccess)
				slot->activated = true;
		}

		if (hasPresence && mFBNotifier) {
			mFBNotifier->injectELDIntoPinIfReady(slot->cad, slot->pinNid);
			mFBNotifier->ensureAudioPipeEnabled(slot->cad, slot->pinNid);
		}

		/* When cable is removed, tell the GPU to stop the audio pipe so it
		 * can power-gate the display engine and reduce power consumption.
		 * ATI HDMI codecs always report presence=0 (bit 31) even when a display
		 * is connected — they set ELD_VALID (bit 1) instead.  Only disable the
		 * audio pipe for ATI when ELD_VALID is also 0, meaning truly disconnected. */
		if (!hasPresence && mFBNotifier) {
			/*
			 * Do not disable AMD/ATI framebuffer audio pipes from the HDA side.
			 * RX4xx/RX5xx/RX5xxx/RX6xxx AMD cards can report stale pin sense while
			 * the display pipe is still valid; disabling here can kill HDMI audio or
			 * trigger black-screen during hotplug/sleep-wake.  Non-ATI paths keep the
			 * normal disable behavior.
			 */
			bool disablePipe = !atiHDMI;
			Codec *codec = mCodecs[slot->cad];
			if (codec && isAtiHdmiCodec(codec)) {
				bool eldValid = (pinSenses[i] & (1U << 1)) != 0;
				if (mVerbose >= 2) {
					IOLog("VoodooHDA ATI DBG: updatePresence pin=%d pinSense=0x%08x ELD_VALID=%d disablePipe=0 preferred=%d\n",
					      slot->pinNid, (unsigned)pinSenses[i], eldValid,
					      legacyPolarisRecovery ? 3 : (havePreferredPin ? 1 : 0));
				}
			}
			if (disablePipe)
				mFBNotifier->disableAudioPipeForPin(slot->cad, slot->pinNid);
		}

		/* Keep HDMI/DP output names generic; Polaris recovery uses hidden mirrors
		 * rather than exposing separate pin-labeled Sound outputs.
		 */
		slot->engine->setProperty("IOAudioEngineDescription", engineName);
	}
}

void VoodooHDADevice::handleUnsolicited(Codec *codec, UInt32 tag, UInt32 resp)
{
	FunctionGroup *funcGroup = NULL;

	if (!codec)
		return;

	for (int i = 0; i < codec->numFuncGroups; i++) {
		if (codec->funcGroups[i].nodeType == HDA_PARAM_FCT_GRP_TYPE_NODE_TYPE_AUDIO) {
			funcGroup = &codec->funcGroups[i];
			break;
		}
	}

	if (!funcGroup)
		return;

	switch (tag) {
	case HDAC_UNSOLTAG_EVENT_HP:
	{
		/*
		 * Determine flags based on pin type (FreeBSD pattern).
		 * HDMI/DP: flags = resp & 0x03 (presence + ELD change)
		 * Analog:  flags = 0x01 (presence only)
		 */
		int flags = 0x01; /* default: presence only */

		/* Check if any HDMI/DP pin uses this tag — if so, extract both flags */
		for (int j = funcGroup->startNode; j < funcGroup->endNode; j++) {
			Widget *w = widgetGet(funcGroup, j);
			if (!w || w->enable == 0 || w->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX)
				continue;
			if (HDA_PARAM_PIN_CAP_DP(w->pin.cap) || HDA_PARAM_PIN_CAP_HDMI(w->pin.cap)) {
				flags = resp & 0x03;
				break;
			}
		}

		if (mVerbose >= 2) {
			IOLog("VoodooHDA DBG: unsol tag=0x%x resp=0x%08x flags=0x%x\n",
			      (unsigned)tag, (unsigned)resp, flags);
		}

		/* Presence change — standard jack switching */
		if (flags & 0x01) {
			switchHandler(funcGroup, false);
			updateHDMIEnginePresence();
		}

		/* ELD change — re-read ELD for HDMI/DP pins */
		if (flags & 0x02) {
			for (int j = funcGroup->startNode; j < funcGroup->endNode; j++) {
				Widget *w = widgetGet(funcGroup, j);
				if (!w || w->enable == 0 || w->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX)
					continue;
				if (!HDA_PARAM_PIN_CAP_DP(w->pin.cap) && !HDA_PARAM_PIN_CAP_HDMI(w->pin.cap))
					continue;
				if (mVerbose >= 2)
					IOLog("VoodooHDA DBG: ELD change event, re-reading ELD for nid=%d\n", w->nid);
				hdaa_eld_handler(w);
			}
		}
		break;
	}
	default:
		errorMsg("Unknown unsol tag: 0x%08lx!\n", (long unsigned int)tag);
		break;
	}
}

/********************************************************************************************/
/********************************************************************************************/

int VoodooHDADevice::handleStreamInterrupt(Channel *channel)
{
	/* XXX to be removed */
	UInt32 res;

	if (!channel || !mRegBase || channel->off < 0)
		return 0;

	if (!(channel->flags & HDAC_CHN_RUNNING))
		return 0;

	/* XXX to be removed */
	res = readData8(channel->off + HDAC_SDSTS);

	/* AMD/ATI GPU HDA spuriously asserts FIFOE during normal HDMI playback;
	 * only log descriptor errors (DESE) for AMD, log both for Intel. */
	if (res & HDAC_SDSTS_DESE)
		errorMsg("PCMDIR_%s DESC error SDSTS=0x%02lx\n",
				(channel->direction == PCMDIR_PLAY) ? "PLAY" : "REC", (long unsigned int)res);
	else if ((res & HDAC_SDSTS_FIFOE) && (mDeviceId & 0xffff) != ATI_VENDORID)
		errorMsg("PCMDIR_%s FIFO error SDSTS=0x%02lx\n",
				(channel->direction == PCMDIR_PLAY) ? "PLAY" : "REC", (long unsigned int)res);

	writeData8(channel->off + HDAC_SDSTS, HDAC_SDSTS_DESE | HDAC_SDSTS_FIFOE | HDAC_SDSTS_BCIS);

	/* HDMI/DP playback no longer advances IOAudioEngine timing from BCIS.
	 * Keep handling/clearing stream status here, but route digital progress
	 * through the controller-owned GFXHDA service path. */
	if (channel->pcmDevice && channel->pcmDevice->digital >= 2 &&
	    channel->direction == PCMDIR_PLAY) {
		if (mGFXController)
			mGFXController->handleStreamInterrupt(channel, res,
				reinterpret_cast<AbsoluteTime*>(&mIntrTimeStamp));
		return 0;
	}

	/* Apple GFXHDA behavior: only BCIS triggers the completion callback
	 * ((sdsts & 0x1c) == 4).  AMD/ATI HDA controllers spuriously assert FIFOE
	 * during normal HDMI playback (SDSTS=0x28); treating FIFOE as a completion
	 * event calls takeTimeStamp() at wrong times → timing drift → crackling.
	 * Intel PCH (Raptor Lake) asserts FIFOE without BCIS on ALC897 — keep the
	 * FIFOE soft-completion path for Intel only. */
	if (res & HDAC_SDSTS_BCIS)
		return 1;
	if ((mDeviceId & 0xffff) == INTEL_VENDORID &&
	    (res & (HDAC_SDSTS_DESE | HDAC_SDSTS_FIFOE)))
		return 1;

	return 0;
}

/* Make room for possible 4096 playback/record channels, in 100 years to come. */

#define HDAC_TRIGGER_NONE	0x00000000
#define HDAC_TRIGGER_PLAY	0x00000fff
#define HDAC_TRIGGER_REC	0x00fff000
#define HDAC_TRIGGER_UNSOL	0x80000000

void VoodooHDADevice::handleInterrupt()
{
	UInt32 status, trigger;

	mTotalInt++;
	status = OSBitAndAtomic(0U, &mIntStatus);
	while (HDA_FLAG_MATCH(status, HDAC_INTSTS_GIS)) {

		trigger = 0;
		
		LOCK();

		/* Was this a controller interrupt? */
		if (HDA_FLAG_MATCH(status, HDAC_INTSTS_CIS)) {
			UInt8 rirbStatus = readData8(HDAC_RIRBSTS);
			/* Get as many responses that we can */
			while (HDA_FLAG_MATCH(rirbStatus, HDAC_RIRBSTS_RINTFL)) {
				writeData8(HDAC_RIRBSTS, HDAC_RIRBSTS_RINTFL);
				if (rirbFlush() != 0)
					trigger |= HDAC_TRIGGER_UNSOL;
				rirbStatus = readData8(HDAC_RIRBSTS);
			}
		}

		if (status & HDAC_INTSTS_SIS_MASK) {
			for (int i = 0; i < mNumChannels; i++) {
				if ((status & (1 << (mChannels[i].off >> 5))) &&
				    (handleStreamInterrupt(&mChannels[i]) != 0))
					trigger |= (1 << i);
			}
		}

		for (int i = 0; i < mNumChannels; i++)
			if (trigger & (1 << i))
				handleChannelInterrupt(i);
		if (trigger & HDAC_TRIGGER_UNSOL)
			unsolqFlush();

		UNLOCK();

		status = OSBitAndAtomic(0U, &mIntStatus);
	}
}

__attribute__((visibility("hidden")))
void VoodooHDADevice::timeoutOccurred(OSObject *owner, IOTimerEventSource *source)
{
	VoodooHDADevice *device = OSDynamicCast(VoodooHDADevice, owner);

	if (!device)
		return;

	if (device->mVerbose >= 3)
		device->logMsg("total interrupts: %lld (%lld channel interrupts)\n", device->mTotalInt,
					device->mTotalChanInt);

	source->setTimeoutMS(kVoodooHDATimerIdleIntervalMs);
}

/********************************************************************************************/
/********************************************************************************************/

int VoodooHDADevice::audioCtlOssMixerInit(PcmDevice *pcmDevice)
{
	if (!pcmDevice || !pcmDevice->funcGroup || !mChannels)
		return -1;

	FunctionGroup *funcGroup = pcmDevice->funcGroup;
	AudioControl *control;
	UInt32 mask, recmask;
	int softpcmvol;
	bool playChannelValid = (pcmDevice->playChanId >= 0 && pcmDevice->playChanId < mNumChannels);
	bool recChannelValid = (pcmDevice->recChanId >= 0 && pcmDevice->recChanId < mNumChannels);
	int playAssoc = playChannelValid ? mChannels[pcmDevice->playChanId].assocNum : -1;
	int recAssoc = recChannelValid ? mChannels[pcmDevice->recChanId].assocNum : -1;

//	logMsg("VoodooHDADevice[%p]::audioCtlOssMixerInit(%p)\n", this, pcmDevice);

	/* Make sure that in case of soft volume it won't stay muted. */
	for (int i = 0; i < SOUND_MIXER_NRDEVICES; i++) {
		pcmDevice->left[i] = 100;
		pcmDevice->right[i] = 100;
	}

	mask = 0;
	recmask = 0;
	/* Declare EAPD as ogain control. */
	if (playChannelValid) {
		for (int i = funcGroup->startNode; i < funcGroup->endNode; i++) {
			Widget *widget = widgetGet(funcGroup, i);
			if (!widget || (widget->enable == 0))
				continue;
			if ((widget->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX) ||
			    	(widget->params.eapdBtl == HDAC_INVALID) ||
					(widget->bindAssoc != playAssoc))
				continue;
			mask |= SOUND_MASK_OGAIN;
			break;
		}
	}

	/* Declare volume controls assigned to this association. */
	control = NULL;
	for (int i = 0; (control = audioCtlEach(funcGroup, i)); i++) {
		if (control->enable == 0 || !control->widget)
			continue;
		if ((playChannelValid && (control->widget->bindAssoc == playAssoc)) ||
		    	(recChannelValid && (control->widget->bindAssoc == recAssoc)) ||
				((control->widget->bindAssoc == -2) && (pcmDevice->index == 0)))
			mask |= control->ossmask;
	}

	/* Declare record sources available to this association. */
	if (recChannelValid) {
		Channel *channel = &mChannels[pcmDevice->recChanId];
		for (int i = 0; i < 16 && channel->io[i] != -1; i++) {
			Widget *widget = widgetGet(funcGroup, channel->io[i]);
			if (!widget || (widget->enable == 0))
				continue;
			for (int j = 0; j < widget->nconns; j++) {
				Widget *childWidget;
				if (widget->connsenable[j] == 0)
					continue;
				childWidget = widgetGet(funcGroup, widget->conns[j]);
				if (!childWidget || (childWidget->enable == 0))
					continue;
				if ((childWidget->bindAssoc != recAssoc) &&
						(childWidget->bindAssoc != -2))
					continue;
				recmask |= childWidget->ossmask;
			}
		}
	}

	/* Declare soft PCM volume if needed. */
	if (playChannelValid && !pcmDevice->digital) {
		control = NULL;
		if ((mask & SOUND_MASK_PCM) == 0 ||
				(funcGroup->audio.quirks & HDA_QUIRK_SOFTPCMVOL)) {
			softpcmvol = 1;
			mask |= SOUND_MASK_PCM;
		} else {
			softpcmvol = 0;
				for (int i = 0; (control = audioCtlEach(funcGroup, i)); i++) {
					if (control->enable == 0 || !control->widget)
						continue;
					if ((control->widget->bindAssoc != playAssoc) &&
					    	((control->widget->bindAssoc != -2) || (pcmDevice->index != 0)))
						continue;
					if (!(control->ossmask & SOUND_MASK_PCM))
					continue;
				if (control->step > 0)
					break;
			}
		}

		if ((softpcmvol == 1) || !control) {
#if 0
			pcm_setflags(pcmDevice->dev, pcm_getflags(pcmDevice->dev) | SD_F_SOFTPCMVOL);
#else
//			logMsg("XXX pcm_setflags SD_F_SOFTPCMVOL\n");
#endif
//			logMsg("%s Soft PCM volume\n", (softpcmvol == 1) ? "Forcing" : "Enabling");
		}
	}

	/* Declare master volume if needed. */
	if (playChannelValid) {
		if ((mask & (SOUND_MASK_VOLUME | SOUND_MASK_PCM)) == SOUND_MASK_PCM) {
			mask |= SOUND_MASK_VOLUME;
#if 0
			mix_setparentchild(m, SOUND_MIXER_VOLUME, SOUND_MASK_PCM);
			mix_setrealdev(m, SOUND_MIXER_VOLUME, SOUND_MIXER_NONE);
#else
//			logMsg("XXX mix_setparentchild SOUND_MIXER_VOLUME SOUND_MASK_PCM\n");
//			logMsg("XXX mix_setrealdev SOUND_MIXER_VOLUME SOUND_MIXER_NONE\n");
#endif
//			logMsg("Forcing master volume with PCM\n");
		}
	}

	recmask &= (1 << SOUND_MIXER_NRDEVICES) - 1;
	mask &= (1 << SOUND_MIXER_NRDEVICES) - 1;

	pcmDevice->recDevMask = recmask;
	pcmDevice->devMask = mask;

	return 0;
}


bool VoodooHDADevice::shouldRouteMasterVolumeToPCM(PcmDevice *pcmDevice)
{
	AudioControl *control;
	bool hasPCMOutputControl = false;

	if (!pcmDevice || !pcmDevice->funcGroup)
		return false;

	/* Keep HDMI/DP digital paths untouched.  The laptop volume bug this fixes is
	 * caused by analog codecs where the macOS master slider is exposed as
	 * SOUND_MIXER_VOLUME but the real output amplifier that actually changes the
	 * speaker level is attached to SOUND_MIXER_PCM.
	 *
	 * The first auto-fix was too strict because it only triggered when no Volume
	 * control existed at all.  Some laptop codecs expose both Volume and PCM, but
	 * only PCM effectively changes the analog output.  For analog playback devices
	 * with a real PCM output amp, route the master slider to PCM.  This preserves
	 * HDMI, capture, iGain, iMix and monitor paths.
	 */
	if (pcmDevice->digital)
		return false;

	if (pcmDevice->playChanId < 0 || pcmDevice->playChanId >= mNumChannels)
		return false;

	if (!(pcmDevice->devMask & SOUND_MASK_VOLUME) || !(pcmDevice->devMask & SOUND_MASK_PCM))
		return false;

	for (int i = 0; (control = audioCtlEach(pcmDevice->funcGroup, i)); i++) {
		if (control->enable == 0 || !control->widget)
			continue;

		if (!(control->dir & HDA_CTL_OUT))
			continue;

		if (!((control->widget->bindAssoc == mChannels[pcmDevice->playChanId].assocNum) ||
		      (control->widget->bindAssoc == -2)))
			continue;

		if ((control->ossmask & SOUND_MASK_PCM) && control->step > 0) {
			hasPCMOutputControl = true;
			break;
		}
	}

	return hasPCMOutputControl;
}


bool VoodooHDADevice::audioCtlApplyAnalogMasterVolume(PcmDevice *pcmDevice, UInt32 left, UInt32 right)
{
	FunctionGroup *funcGroup;
	AudioControl *control;
	UInt32 mute;
	int assocNum;
	bool applied = false;

	if (!pcmDevice || !pcmDevice->funcGroup)
		return false;
	if (pcmDevice->digital)
		return false;
	if (pcmDevice->playChanId < 0 || pcmDevice->playChanId >= mNumChannels)
		return false;

	if (left > 100)
		left = 100;
	if (right > 100)
		right = 100;

	funcGroup = pcmDevice->funcGroup;
	assocNum = mChannels[pcmDevice->playChanId].assocNum;

	LOCK();

	/*
	 * Keep the logical OSS state in sync with the visible CoreAudio master, then
	 * apply that value to one effective analog playback amp.  Relying on the
	 * generic Volume -> PCM fallback makes the slider nearly inert on codecs whose
	 * visible OSS map does not match the actual output amp.  Updating every
	 * Volume/PCM amp in series can multiply gain on legacy codecs, so pick one
	 * best control and keep the speaker-safe hardware ceiling there.
	 */
	pcmDevice->left[SOUND_MIXER_VOLUME] = left;
	pcmDevice->right[SOUND_MIXER_VOLUME] = right;
	pcmDevice->left[SOUND_MIXER_PCM] = left;
	pcmDevice->right[SOUND_MIXER_PCM] = right;

	AudioControl *bestControl = NULL;
	int bestScore = -1;
	for (int i = 0; (control = audioCtlEach(funcGroup, i)); i++) {
		int score;

		if (control->enable == 0 || !control->widget)
			continue;
		if (!((control->dir & HDA_CTL_OUT) ||
		    ((control->dir & HDA_CTL_IN) && VoodooHDAIsAnalogOutputPin(control->widget))))
			continue;
		if (!((control->widget->bindAssoc == assocNum) || (control->widget->bindAssoc == -2)))
			continue;
		if (!(control->ossmask & (SOUND_MASK_VOLUME | SOUND_MASK_PCM)))
			continue;
		if (control->step <= 0)
			continue;
		if (control->forcemute)
			continue;

		score = 0;
		if (control->widget->bindAssoc == assocNum)
			score += 16;
		if (VoodooHDAIsAnalogOutputPin(control->widget))
			score += 8;
		if (control->dir & HDA_CTL_OUT)
			score += 4;
		if (control->ossmask & SOUND_MASK_VOLUME)
			score += 2;
		if (control->ossmask & SOUND_MASK_PCM)
			score += 1;
		if (score > bestScore) {
			bestScore = score;
			bestControl = control;
		}
	}

	if (bestControl) {
		int lvol = VoodooHDAAnalogOutputStepForPercent(bestControl, (int)left);
		int rvol = VoodooHDAAnalogOutputStepForPercent(bestControl, (int)right);
		mute = (left == 0) ? HDA_AMP_MUTE_LEFT : 0;
		mute |= (right == 0) ? HDA_AMP_MUTE_RIGHT : 0;

		audioCtlAmpSet(bestControl, mute, lvol, rvol);
		applied = true;
	}

	UNLOCK();

	return applied;
}

int VoodooHDADevice::audioCtlOssMixerSet(PcmDevice *pcmDevice, UInt32 dev, UInt32 left, UInt32 right)
{
	if (!pcmDevice || !pcmDevice->funcGroup || !mChannels || dev >= SOUND_MIXER_NRDEVICES)
		return -1;

	FunctionGroup *funcGroup = pcmDevice->funcGroup;
	AudioControl *control;
	UInt32 mask = 0;
	bool playChannelValid = (pcmDevice->playChanId >= 0 && pcmDevice->playChanId < mNumChannels);
	bool recChannelValid = (pcmDevice->recChanId >= 0 && pcmDevice->recChanId < mNumChannels);
	int playAssoc = playChannelValid ? mChannels[pcmDevice->playChanId].assocNum : -1;
	int recAssoc = recChannelValid ? mChannels[pcmDevice->recChanId].assocNum : -1;

	if (left > 100)
		left = 100;
	if (right > 100)
		right = 100;
		
	LOCK();
	
	//logMsg("VoodooHDADevice[%p]::audioCtlOssMixerSet(%p, %ld, %ld, %ld)\n", this, pcmDevice, dev, left, right);

	// Save new values. 
	pcmDevice->left[dev] = left;
	pcmDevice->right[dev] = right;

	
	// 'ogain' is the special case implemented with EAPD.
	if (dev == SOUND_MIXER_OGAIN) {
		Widget *widget = NULL;
		int i;
		UInt32 orig;
	
		for (i = funcGroup->startNode; i < funcGroup->endNode; i++) {
			widget = widgetGet(funcGroup, i);
			if (!widget || (widget->enable == 0))
				continue;
			if ((widget->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX) ||
			    	(widget->params.eapdBtl == HDAC_INVALID))
				continue;
			break;
		}
		
		if (i >= funcGroup->endNode) {
			UNLOCK();
			return -1;
		}
		orig = widget->params.eapdBtl;
		if (left == 0)
			widget->params.eapdBtl &= ~HDA_CMD_SET_EAPD_BTL_ENABLE_EAPD;
		else
			widget->params.eapdBtl |= HDA_CMD_SET_EAPD_BTL_ENABLE_EAPD;
		if (orig != widget->params.eapdBtl) {
			UInt32 val = widget->params.eapdBtl;
			if (funcGroup->audio.quirks & HDA_QUIRK_EAPDINV)
				val ^= HDA_CMD_SET_EAPD_BTL_ENABLE_EAPD;
			sendCommand(HDA_CMD_SET_EAPD_BTL_ENABLE(funcGroup->codec->cad, widget->nid, val),
					funcGroup->codec->cad);
		}
		UNLOCK();
		return (left | (left << 8));
	}
	//Slice
	mask = (1 << dev);
/*	if(dev == SOUND_MIXER_MIC)
		mask |= SOUND_MASK_MONITOR;*/
	// Recalculate all controls related to this OSS device.
		for (int i = 0; (control = audioCtlEach(funcGroup, i)); i++) {
			UInt32 mute;
			int lvol, rvol;
			bool analogOutputProtected;
			if ((control->enable == 0) || !control->widget || !(control->ossmask & mask))
				continue;
			if (!((playChannelValid && (control->widget->bindAssoc == playAssoc)) ||
		    	(recChannelValid && (control->widget->bindAssoc == recAssoc)) ||
		    	(control->widget->bindAssoc == -2)))
				continue;

		lvol = 100;
		rvol = 100;
		for (int j = 0; j < SOUND_MIXER_NRDEVICES; j++) {
			if (control->ossmask & (1 << j)) {
				lvol = lvol * pcmDevice->left[j] / 100;
				rvol = rvol * pcmDevice->right[j] / 100;
			}
		}

		/*
		 * Do not let analog playback amps hit their absolute maximum step.
		 * On some laptop codecs this produces clipping/DC-like transients at
		 * full scale; external powered speakers can then shut down for protection.
		 * Keep capture and HDMI/DP untouched.  The user's visible slider can still
		 * reach 100; only the hardware amp command is capped.
		 */
			analogOutputProtected = (!pcmDevice->digital &&
			    playChannelValid &&
			    (control->widget->bindAssoc == playAssoc ||
			     control->widget->bindAssoc == -2) &&
			    ((control->dir & HDA_CTL_OUT) ||
			     ((control->dir & HDA_CTL_IN) && VoodooHDAIsAnalogOutputPin(control->widget))));
		if (analogOutputProtected) {
			lvol = VoodooHDAAnalogOutputScalePercent(lvol);
			rvol = VoodooHDAAnalogOutputScalePercent(rvol);
		}

		mute = (lvol == 0) ? HDA_AMP_MUTE_LEFT : 0;
		mute |= (rvol == 0) ? HDA_AMP_MUTE_RIGHT : 0;

		// VertexBZ: Separated flags for Volume/PCM and Mic Half Volume fixes
		const bool autoRoutePCMVolumeFix = (dev == SOUND_MIXER_PCM) && shouldRouteMasterVolumeToPCM(pcmDevice);
		if (!analogOutputProtected &&
		    ((dev == SOUND_MIXER_VOLUME && mEnableHalfVolumeFix) ||
		     (dev == SOUND_MIXER_PCM && (mEnableVolumeChangeFix || autoRoutePCMVolumeFix)) ||
		     (dev == SOUND_MIXER_MIC && mEnableHalfMicVolumeFix))) {
			// cue8chalk: lerp the volume between the midpoint and the end to get the true value
			lvol = ilerp(control->offset >> 1, control->offset, ((lvol * control->step + 50) / 100) / (control->offset != 0 ? (float)control->offset : 1));
			rvol = ilerp(control->offset >> 1, control->offset, ((rvol * control->step + 50) / 100) / (control->offset != 0 ? (float)control->offset : 1));
		} else {
			lvol = (lvol * control->step + 50) / 100;
			rvol = (rvol * control->step + 50) / 100;
		}

		/*
		 * Keep speaker protection as the last analog-output volume transform.
		 * The Volume/PCM expansion fix is useful on some paths, but it must not
		 * expand a compressed analog percent back toward the codec's max amp step.
		 */
        
		audioCtlAmpSet(control, mute, lvol, rvol);
	}

	UNLOCK();

	return (left | (right << 8));
}

int VoodooHDADevice::ilerp(int a, int b, float t) {
	return a + (t * (float)(b - a));
}

UInt32 VoodooHDADevice::audioCtlOssMixerSetRecSrc(PcmDevice *pcmDevice, UInt32 src)
{
	if (!pcmDevice || !pcmDevice->funcGroup || !mChannels)
		return 0;

	FunctionGroup *funcGroup = pcmDevice->funcGroup;
	Channel *channel;
	UInt32 ret = 0xffffffff;

//		logMsg("VoodooHDADevice[%p]::audioCtlOssMixerSetRecSrc(%p, 0x%lx)\n", this, pcmDevice, src);

	if (pcmDevice->recChanId < 0 || pcmDevice->recChanId >= mNumChannels)
		return 0;

	LOCK();

	/* Commutate requested recsrc for each ADC. */
	channel = &mChannels[pcmDevice->recChanId];
	for (int i = 0; i < 16 && channel->io[i] != -1; i++) {
		Widget *widget = widgetGet(funcGroup, channel->io[i]);
		if (!widget || (widget->enable == 0))
			continue;
		ret &= audioCtlRecSelComm(pcmDevice, src, channel->io[i], 0);
	}

	UNLOCK();

	return ((ret == 0xffffffff) ? 0 : ret);
}
//never used
int VoodooHDADevice::audioCtlOssMixerGet(PcmDevice *pcmDevice, UInt32 dev, UInt32* left, UInt32* right)
{
	if (!pcmDevice || !pcmDevice->funcGroup || !mChannels || dev >= SOUND_MIXER_NRDEVICES)
		return -1;

	FunctionGroup *funcGroup = pcmDevice->funcGroup;
	AudioControl *control;
	int lvol = 100, rvol = 100;
//	bool bFound = false;
//	int controlIndex;
	//Slice
	UInt32 mask = (1 << dev);
	bool playChannelValid = (pcmDevice->playChanId >= 0 && pcmDevice->playChanId < mNumChannels);
	bool recChannelValid = (pcmDevice->recChanId >= 0 && pcmDevice->recChanId < mNumChannels);
	int playAssoc = playChannelValid ? mChannels[pcmDevice->playChanId].assocNum : -1;
	int recAssoc = recChannelValid ? mChannels[pcmDevice->recChanId].assocNum : -1;
 
	
	LOCK();
	
	
	/* Recalculate all controls related to this OSS device. */
	for (int i = 0; (control = audioCtlEach(funcGroup, i)); i++) {
		if ((control->enable == 0) || !control->widget || !(control->ossmask & mask))
			continue;
		if (!((playChannelValid && (control->widget->bindAssoc == playAssoc)) ||
			  (recChannelValid && (control->widget->bindAssoc == recAssoc)) ||
			  (control->widget->bindAssoc == -2)))
			continue;
	
		audioCtlAmpGetGain(control);
	
		if(control->step != 0) {
//			bFound = true;
//			controlIndex = i;
			lvol = 100 * control->left / control->step;
			rvol = 100 * control->right / control->step;
			
			pcmDevice->left[dev] = lvol;
			pcmDevice->right[dev] = rvol;
		}
	}
	
	UNLOCK();
	
	if(left != 0) (*left) = lvol;
	if(right != 0) (*right) = rvol;
	
	return (lvol | (rvol << 8));
}

void VoodooHDADevice::mixerSetDefaults(PcmDevice *pcmDevice)
{
	//IOLog("VoodooHDADevice::mixerSetDefaults\n");
	for (int n = 0; n < SOUND_MIXER_NRDEVICES; n++) {
		uint32_t def = mMixerDefaults[n];
		if (def > 100)
			def = 100;
		// For HDMI/DP: start at unity gain; no hardware attenuation needed.
		if (pcmDevice->digital >= 2 && (n == SOUND_MIXER_PCM || n == SOUND_MIXER_VOLUME))
			def = 100;
		audioCtlOssMixerSet(pcmDevice, n, def, def);
	}
//Slice - attention!
//	if (audioCtlOssMixerSetRecSrc(pcmDevice, SOUND_MASK_INPUT) == 0)
		//errorMsg("warning: couldn't set recording source to input\n");
		return;
}

void VoodooHDADevice::mixerResume(PcmDevice *pcmDevice)
{
	if (!pcmDevice)
		return;

	for (int n = 0; n < SOUND_MIXER_NRDEVICES; n++)
		audioCtlOssMixerSet(pcmDevice, n, pcmDevice->left[n], pcmDevice->right[n]);
	audioCtlOssMixerSetRecSrc(pcmDevice, SOUND_MASK_INPUT);	// ignore error
}

/*******************************************************************************************/
/*******************************************************************************************/

Channel *VoodooHDADevice::channelInit(PcmDevice *pcmDevice, int direction)
{
	if (!pcmDevice || !pcmDevice->funcGroup)
		return NULL;

	FunctionGroup *funcGroup = pcmDevice->funcGroup;
	Channel *channel;
	int ord = 0, chid;

	chid = (direction == PCMDIR_PLAY) ? pcmDevice->playChanId : pcmDevice->recChanId;
	if (!mChannels || chid < 0 || chid >= mNumChannels) {
		errorMsg("error: invalid channel id %d for direction %d\n", chid, direction);
		return NULL;
	}
	channel = &mChannels[chid];
	for (int i = 0; i < mNumChannels && i < chid; i++)
		if (channel->direction == mChannels[i].direction)
			ord++;
	if (direction == PCMDIR_PLAY)
		channel->off = (mInStreamsSup + ord) << 5;
	else
		channel->off = ord << 5;

	if (funcGroup->audio.quirks & HDA_QUIRK_FIXEDRATE) {
		channel->caps.minSpeed = channel->caps.maxSpeed = 48000;
		channel->pcmRates[0] = 48000;
		channel->pcmRates[1] = 0;
	}
	/* AMD/ATI GPU HDA controllers (vendor 0x1002) must use SDLPIB for position,
	 * mirroring Apple's IOGFXHDAStream::getLinkPositionInBuffer() which reads
	 * SDLPIB directly — NOT the DMA Position Buffer.  Enabling DMAPOS on AMD
	 * GPU HDA produces unreliable values that corrupt eraseOutputSamples
	 * decisions and cause high-frequency aliasing. */
	if (mDmaPosMem && (mDeviceId & 0xffff) != ATI_VENDORID)
		channel->dmaPos = (UInt32 *) (mDmaPosMem->virtAddr + (mStreamCount * 8));
	else
		channel->dmaPos = NULL;
	channel->streamId = ++mStreamCount;
	channel->direction = direction;

	if (direction == PCMDIR_PLAY && pcmDevice->digital >= 2) {
		if (!mGFXController) {
			errorMsg("error: VoodooGFXHDAController missing during digital channelInit (codec=%04x nid=%d streamId=%d)\n",
				 channel->funcGroup->codec->deviceId, pcmDevice->funcGroup->nid, channel->streamId);
			return NULL;
		}
		return mGFXController->initializeStreamDMA(channel) ? channel : NULL;
	}

	if (!pcmDevice->chanSize || pcmDevice->chanNumBlocks < HDA_BDL_MIN ||
	    pcmDevice->chanNumBlocks > HDA_BDL_MAX ||
	    (pcmDevice->chanSize / pcmDevice->chanNumBlocks) < HDA_BLK_MIN) {
		errorMsg("error: invalid PCM DMA geometry size=%u blocks=%u\n",
				(unsigned)pcmDevice->chanSize, (unsigned)pcmDevice->chanNumBlocks);
		return NULL;
	}

	channel->blockSize = pcmDevice->chanSize / pcmDevice->chanNumBlocks;
	channel->numBlocks = pcmDevice->chanNumBlocks;

	if (bdlAlloc(channel) != 0) {
		channel->numBlocks = 0;
		return NULL;
	}

//	logMsg("block size: %ld, block count: %ld, buffer size: %ld\n", channel->blockSize, channel->numBlocks,
//			pcmDevice->chanSize);

	channel->buffer = allocateDmaMemory(pcmDevice->chanSize, "buffer");
	if (!channel->buffer) {
		errorMsg("can't allocate sound buffer!\n");
		if (channel->bdlMem) {
			freeDmaMemory(channel->bdlMem);
			channel->bdlMem = NULL;
		}
		channel->numBlocks = 0;
		return NULL;
	}
	ASSERT(channel->buffer->size == pcmDevice->chanSize);

	ASSERT(channel->blockSize <= (pcmDevice->chanSize / HDA_BDL_MIN));
	ASSERT(channel->blockSize >= HDA_BLK_MIN);
	ASSERT(channel->numBlocks <= HDA_BDL_MAX);
	ASSERT(channel->numBlocks >= HDA_BDL_MIN);

	return channel;
}

int VoodooHDADevice::channelSetFormat(Channel *channel, UInt32 format)
{
	if (!channel || !channel->caps.formats)
		return -1;

	for (int i = 0; channel->caps.formats[i] != 0; i++) {
		if (format == channel->caps.formats[i]) {
			channel->format = format;
			return 0;
		}
	}

	return -1;
}

int VoodooHDADevice::channelSetSpeed(Channel *channel, UInt32 reqSpeed)
{
	UInt32 speed = 0;

	if (!channel)
		return 0;

	for (int i = 0; channel->pcmRates[i] != 0; i++) {
		UInt32 threshold;
		speed = channel->pcmRates[i];
		threshold = speed + ((channel->pcmRates[i + 1] != 0) ? ((channel->pcmRates[i + 1] - speed) >> 1) : 0);
		if (reqSpeed < threshold)
			break;
	}

	if (speed == 0) /* impossible */
		channel->speed = 48000;
	else
		channel->speed = speed;

	return channel->speed;
}

void VoodooHDADevice::channelStop(Channel *channel, const bool shouldLock)
{
	if (!channel || !channel->funcGroup || !channel->funcGroup->codec || !mRegBase)
		return;

	FunctionGroup *funcGroup = channel->funcGroup;
	nid_t cad = funcGroup->codec->cad;

	if (shouldLock)
		LOCK();

	if (mGFXController)
		mGFXController->updateTiming(channel, false, false);

	if (channel->pcmDevice && channel->pcmDevice->digital >= 2) {
		nid_t pin = getHDMIPinForChannel(channel);
		if (mVerbose >= 2) {
			IOLog("VoodooHDA DBG: channelStop HDMI pin=%d streamId=%d\n",
			      pin, channel->streamId);
		}
		if (pin != (nid_t)-1 && mFBNotifier)
			mFBNotifier->notifyStreamingState(cad, pin, false);
		mirrorHDMIStreamToCandidatePins(channel, false);
	}

	if (mGFXController && mGFXController->ownsChannel(channel))
		mGFXController->stopStream(channel);
	else {
		streamStop(channel);

		/* Zero the DMA buffer after stopping so no stale audio leaks into the
		 * next session.  This mirrors the bzero in channelStart and ensures the
		 * buffer is clean even if the next start arrives without a full reset. */
		if (channel->buffer && channel->buffer->virtAddr && channel->buffer->size)
			bzero(reinterpret_cast<void *>(channel->buffer->virtAddr), channel->buffer->size);
	}

	for (int i = 0; i < 16 && channel->io[i] != -1; i++) {
		Widget *widget = widgetGet(channel->funcGroup, channel->io[i]);
		if (!widget)
			continue;
		if (HDA_PARAM_AUDIO_WIDGET_CAP_DIGITAL(widget->params.widgetCap))
			sendCommand(HDA_CMD_SET_DIGITAL_CONV_FMT1(cad, channel->io[i], 0), cad);
		sendCommand(HDA_CMD_SET_CONV_STREAM_CHAN(cad, channel->io[i], 0), cad);
	}

	if (shouldLock)
		UNLOCK();
}

void VoodooHDADevice::channelStart(Channel *channel, const bool shouldLock)
{
	if (!channel || !channel->funcGroup || !channel->funcGroup->codec || !mRegBase)
		return;

	if (shouldLock)
		LOCK();

	if (channel->pcmDevice && channel->pcmDevice->digital >= 2) {
		nid_t pin = getHDMIPinForChannel(channel);
		if (mVerbose >= 2) {
			IOLog("VoodooHDA DBG: channelStart HDMI pin=%d streamId=%d speed=%d\n",
			      pin, channel->streamId, (int)channel->speed);
		}
	}

	if (mGFXController && mGFXController->ownsChannel(channel))
		mGFXController->prepareStreamDMA(channel);
	else {
		streamStop(channel);
		streamReset(channel);

		/* Verify SDLPIB is zero after stream reset.  AMD GPU HDA controllers
		 * sometimes leave a stale position → IOAudioEngine writes/erases at the
		 * wrong offset → "sound starts from the middle" / residual sounds. */
		{
			UInt32 posAfterReset = readData32(channel->off + HDAC_SDLPIB);
			if (posAfterReset != 0)
				IOLog("VoodooHDA WARN: SDLPIB=0x%x after streamReset (stream off=0x%x), expected 0\n",
				      posAfterReset, channel->off);
		}

		/* Zero the DMA sample buffer before each playback session so that wrap-around
		 * never replays stale audio from a prior clip (old data → elongation + crackling). */
		if (channel->buffer && channel->buffer->virtAddr && channel->buffer->size)
			bzero(reinterpret_cast<void *>(channel->buffer->virtAddr), channel->buffer->size);
		bdlSetup(channel);
		streamSetId(channel);
	}
	streamSetup(channel);
	mirrorHDMIStreamToCandidatePins(channel, true);
	restoreAnalogPlaybackPath(channel, false);
	if (mGFXController && mGFXController->ownsChannel(channel))
		mGFXController->startStream(channel);
	else
		streamStart(channel);
	if (mGFXController)
		mGFXController->updateTiming(channel, true, true);

	if (channel->pcmDevice && channel->pcmDevice->digital >= 2 && mFBNotifier) {
		nid_t pin = getHDMIPinForChannel(channel);
		if (pin != (nid_t)-1)
			mFBNotifier->notifyStreamingState(channel->funcGroup->codec->cad, pin, true);
	}

	if (shouldLock)
		UNLOCK();
}

int VoodooHDADevice::channelGetPosition(Channel *channel)
{
	UInt32 position;
	UInt32 bufferBytes;

	if (!channel || !mRegBase)
		return 0;

	LOCK();

	if (channel->dmaPos)
		position = *(channel->dmaPos);
	else
		position = readData32(channel->off + HDAC_SDLPIB);

	UNLOCK();

	/* Round to available space and force 128 bytes aligment. */
	bufferBytes = channel->blockSize * channel->numBlocks;
	if (bufferBytes <= channel->slack)
		return 0;
	position %= (bufferBytes - channel->slack);
#if 0
	/* Since mSampleSize may be non-power of 2 */
	position &= HDA_BLK_ALIGN;
#endif

	return position;
}

static
UInt8 calculateStripectl(UInt8 globalSDO, UInt8 stripecap, UInt16 format)
{
#define EXTRACT_CHAN(fmt) (fmt & 15)
#define EXTRACT_BITS(fmt) ((fmt >> 4) & 7)
#define EXTRACT_MULT(fmt) ((fmt >> 11) & 7)
	switch (globalSDO) {
		case 2:
			/* Controller supports 4-stripe */
			if (stripecap & 4) {
				/* Codec audio output widget supports 4-stripe */
				/* Need at least 32-bit per sample frame for 4-stripe */
				if (EXTRACT_BITS(format) == 4)
					return 2;	/* Have 32-bit samples, allow 4-stripe */
				if (EXTRACT_BITS(format)) {
					/* Using 16 to 24-bit samples, check if CHAN > 1 or MULT > 1 */
					if (EXTRACT_CHAN(format))
						return 2;	/* CHAN > 1 */
					if (EXTRACT_MULT(format))
						return 2;	/* MULT > 1 */
				}
				/* Using 8-bit samples, check if CHAN * MULT >= 4 */
				if ((EXTRACT_CHAN(format) + 1) * (EXTRACT_MULT(format) + 1) >= 4)
					return 2;
				/* else can't do 4-stripe */
			} /* else can't do 4-stripe */
			/* fall through and try 2-stripe */
		case 1:
			/* Controller supports 2-stripe */
			if (stripecap & 2) {
				/* Codec audio output widget supports 2-stripe */
				/* Need at least 16-bit per sample frame for 2-stripe */
				if (EXTRACT_BITS(format))
					return 1;	/* Have at least 16-bit samples, allow 2-stripe */
				/* Using 8-bit samples, check if CHAN > 1 or MULT > 1 */
				if (EXTRACT_CHAN(format))
					return 1;	/* CHAN > 1 */
				if (EXTRACT_MULT(format))
					return 1;	/* MULT > 1 */
				/* else can't do 2-stripe */
			} /* else can't do 2-stripe */
			break; /* revert to no-stripe */
	} /* default: controller does not support striping */
	return 0;
#undef EXTRACT_MULT
#undef EXTRACT_BITS
#undef EXTRACT_CHAN
}

/*******************************************************************************************/
/*******************************************************************************************/

void VoodooHDADevice::programHDMIMirrorChannel(Channel *source, Channel *target, bool enable)
{
	if (!target || !target->funcGroup || !target->funcGroup->codec ||
	    !target->funcGroup->audio.assocs)
		return;
	if (target->assocNum < 0 || target->assocNum >= target->funcGroup->audio.numAssocs)
		return;

	AudioAssoc *assoc = &target->funcGroup->audio.assocs[target->assocNum];
	nid_t cad = target->funcGroup->codec->cad;
	nid_t targetPin = getHDMIPinForChannel(target);

	if (!enable) {
		for (int i = 0; i < 16 && target->io[i] != -1; i++) {
			Widget *widget = widgetGet(target->funcGroup, target->io[i]);
			if (!widget)
				continue;
			if (HDA_PARAM_AUDIO_WIDGET_CAP_DIGITAL(widget->params.widgetCap))
				sendCommand(HDA_CMD_SET_DIGITAL_CONV_FMT1(cad, target->io[i], 0), cad);
			sendCommand(HDA_CMD_SET_CONV_STREAM_CHAN(cad, target->io[i], 0), cad);
		}
		return;
	}

	if (!source || !source->funcGroup || !source->funcGroup->codec ||
	    source->streamId <= 0)
		return;
	if (source->funcGroup->codec->cad != cad)
		return;

	int totalchn = AFMT_CHANNEL(source->format);
	int totalextchn = AFMT_EXTCHANNEL(source->format);
	if (!totalchn) {
		if (source->format & (AFMT_STEREO | AFMT_AC3))
			totalchn = 2;
		else
			totalchn = 1;
	}
	if (totalchn < 1)
		totalchn = 1;
	if (totalchn > 8)
		totalchn = 8;

	/*
	 * Hidden Polaris/RX5xx mirror pins need the same display-side nudge as the
	 * visible CoreAudio engine.  Without this, the converter can receive the
	 * stream tag while the GPU/framebuffer audio pipe for the real physical pin
	 * remains idle, which presents as a valid Sound output with no HDMI audio.
	 */
	if (targetPin != (nid_t)-1) {
		if (mFBNotifier) {
			mFBNotifier->injectELDIntoPinIfReady(cad, targetPin);
			mFBNotifier->ensureAudioPipeEnabled(cad, targetPin);
			mFBNotifier->notifyStreamingState(cad, targetPin, true);
		}
		Widget *pinWidget = widgetGet(target->funcGroup, targetPin);
		if (pinWidget && (HDA_PARAM_PIN_CAP_HDMI(pinWidget->pin.cap) ||
		    HDA_PARAM_PIN_CAP_DP(pinWidget->pin.cap))) {
			UInt32 pinCtrl = pinWidget->pin.ctrl | HDA_CMD_SET_PIN_WIDGET_CTRL_OUT_ENABLE;
			if (pinCtrl != pinWidget->pin.ctrl) {
				pinWidget->pin.ctrl = pinCtrl;
				sendCommand(HDA_CMD_SET_PIN_WIDGET_CTRL(cad, targetPin, pinWidget->pin.ctrl), cad);
			}
		}
	}

	UInt16 format = 0;
	if (source->format & AFMT_S16_LE)
		format |= source->bit16 << 4;
	else if (source->format & AFMT_S32_LE)
		format |= source->bit32 << 4;
	else
		format |= 1 << 4;

	for (int i = 0; gRateTable[i].rate; i++) {
		if (gRateTable[i].valid && (source->speed == gRateTable[i].rate)) {
			format |= gRateTable[i].base;
			format |= gRateTable[i].mul;
			format |= gRateTable[i].div;
			break;
		}
	}
	format |= (totalchn - 1);

	const static UInt16 chmap[2][5] = {{ 0x0010, 0x0001, 0x0201, 0x0321, 0x0321 },
	                                  { 0x0010, 0x0001, 0x2201, 0x3321, 0x4321 }};
	int map = -1;
	if (assoc->pinset == 0x0007 || assoc->pinset == 0x0013)
		map = 0;
	else if (assoc->pinset == 0x0017)
		map = 1;

	UInt16 digFormat = HDA_CMD_SET_DIGITAL_CONV_FMT1_DIGEN | HDA_CMD_SET_DIGITAL_CONV_FMT1_COPY;
	if (source->format & AFMT_AC3)
		digFormat |= HDA_CMD_SET_DIGITAL_CONV_FMT1_NAUDIO;

	if (mVerbose >= 2) {
		IOLog("VoodooHDA ATI DBG: HDMI mirror setup sourceStream=%d targetAssoc=%d targetPin=%d totalchn=%d pipeNotified=%d\n",
		      source->streamId, target->assocNum, targetPin, totalchn,
		      (targetPin != (nid_t)-1 && mFBNotifier) ? 1 : 0);
	}

	for (int i = 0, chn = 0; i < 16 && target->io[i] != -1; i++) {
		Widget *widget = widgetGet(target->funcGroup, target->io[i]);
		if (!widget)
			continue;

		int c;
		int cchn;
		if (assoc->fakeredir && i == (assoc->pincnt - 1)) {
			c = (source->streamId << 4);
			chn = 0;
		} else {
			if (map >= 0)
				chn = (((chmap[map][(totalchn / 2) > 4 ? 4 : (totalchn / 2)] >> i * 4) & 0xf) - 1) * 2;
			if (chn < 0 || chn >= totalchn)
				c = 0;
			else
				c = (source->streamId << 4) | chn;
		}

		sendCommand(HDA_CMD_SET_CONV_FMT(cad, target->io[i], format), cad);
		if (HDA_PARAM_AUDIO_WIDGET_CAP_DIGITAL(widget->params.widgetCap)) {
			UInt8 digFmt2 = (source->format & AFMT_AC3) ? 0x19 : 0x01;
			sendCommand(HDA_CMD_12BIT(cad, target->io[i], 0x70e, digFmt2), cad);
			sendCommand(HDA_CMD_SET_DIGITAL_CONV_FMT1(cad, target->io[i], digFormat), cad);
		}
		sendCommand(HDA_CMD_SET_CONV_STREAM_CHAN(cad, target->io[i], c), cad);
		if (!c)
			continue;
		if (HDA_PARAM_AUDIO_WIDGET_CAP_STRIPE(widget->params.widgetCap))
			sendCommand(HDA_CMD_SET_STRIPE_CONTROL(cad, target->io[i], 0), cad);
		cchn = HDA_PARAM_AUDIO_WIDGET_CAP_CC(widget->params.widgetCap);
		if (cchn > 1 && chn < totalchn) {
			int avail = totalchn - chn - 1;
			if (avail < 0)
				avail = 0;
			if (cchn > avail)
				cchn = avail;
			sendCommand(HDA_CMD_SET_CONV_CHAN_COUNT(cad, target->io[i], cchn), cad);
		}
		if (HDA_PARAM_AUDIO_WIDGET_CAP_DIGITAL(widget->params.widgetCap) && mGFXController) {
			UInt32 savedFormat = target->format;
			UInt32 savedSpeed = target->speed;
			int savedStreamId = target->streamId;
			int savedBit16 = target->bit16;
			int savedBit32 = target->bit32;
			UInt16 savedDiagnosticFlags = target->diagnosticFlags;

			target->format = source->format;
			target->speed = source->speed;
			target->streamId = source->streamId;
			target->bit16 = source->bit16;
			target->bit32 = source->bit32;
			target->diagnosticFlags = source->diagnosticFlags;
			mGFXController->setupStream(target, target->io[i], assoc, totalchn, totalextchn);
			target->format = savedFormat;
			target->speed = savedSpeed;
			target->streamId = savedStreamId;
			target->bit16 = savedBit16;
			target->bit32 = savedBit32;
			target->diagnosticFlags = savedDiagnosticFlags;
		}

		chn += cchn + 1;
	}
}

void VoodooHDADevice::mirrorHDMIStreamToCandidatePins(Channel *source, bool enable)
{
	if (!source || !source->funcGroup || !source->funcGroup->codec ||
	    !source->pcmDevice || source->pcmDevice->digital < 2)
		return;

	int sourceCad = source->funcGroup->codec->cad;
	int hdmiEngineCount = (mNumHDMIEngines > 16) ? 16 : mNumHDMIEngines;
	if (hdmiEngineCount < 0)
		hdmiEngineCount = 0;

	for (int i = 0; i < hdmiEngineCount; i++) {
		HDMIEngineSlot *slot = &mHDMIEngines[i];
		if (!slot->channel || slot->channel == source || slot->cad != sourceCad)
			continue;
		if (enable) {
			if (!slot->mirrorCandidate)
				continue;
			programHDMIMirrorChannel(source, slot->channel, true);
			slot->mirroredActive = true;
		} else {
			if (!slot->mirroredActive)
				continue;
			programHDMIMirrorChannel(source, slot->channel, false);
			slot->mirroredActive = false;
		}
	}
}

void VoodooHDADevice::streamSetup(Channel *channel)
{
	if (!channel || !channel->funcGroup || !channel->funcGroup->codec ||
	    !channel->funcGroup->audio.assocs || !mRegBase)
		return;
	if (channel->assocNum < 0 || channel->assocNum >= channel->funcGroup->audio.numAssocs)
		return;

	AudioAssoc *assoc = &channel->funcGroup->audio.assocs[channel->assocNum];
	int totalchn;
  int totalextchn;
	nid_t cad = channel->funcGroup->codec->cad;
	UInt16 format, digFormat;
	const static
	UInt16 chmap[2][5] = {{ 0x0010, 0x0001, 0x0201, 0x0321, 0x0321 }, /* 5.1 */
                        { 0x0010, 0x0001, 0x2201, 0x3321, 0x4321 }};/* 7.1 */
	int map = -1;
	
	totalchn = AFMT_CHANNEL(channel->format);
  totalextchn = AFMT_EXTCHANNEL(channel->format);
	if (!totalchn) {
		if (channel->format & (AFMT_STEREO | AFMT_AC3)) { //Slice - AC3 supports more then Stereo, but here we force 2
			totalchn = 2;
		} else
			totalchn = 1;
	}

	format = 0;
	if (channel->format & AFMT_S16_LE)
		format |= channel->bit16 << 4;
	else if (channel->format & AFMT_S32_LE)
		format |= channel->bit32 << 4;
	else
		format |= 1 << 4;

	for (int i = 0; gRateTable[i].rate; i++) {
		if (gRateTable[i].valid && (channel->speed == gRateTable[i].rate)) {
			format |= gRateTable[i].base;
			format |= gRateTable[i].mul;
			format |= gRateTable[i].div;
			break;
		}
	}

	format |= (totalchn - 1);
	//Slice - from BSD
	/* Set channel mapping for known speaker setups. */
	if (assoc->pinset == 0x0007 || assoc->pinset == 0x0013) // Standard 5.1 
		map = 0;
	 else if (assoc->pinset == 0x0017) // Standard 7.1 
		map = 1;
	
	digFormat = HDA_CMD_SET_DIGITAL_CONV_FMT1_DIGEN | HDA_CMD_SET_DIGITAL_CONV_FMT1_COPY;
	if (channel->format & AFMT_AC3)
		digFormat |= HDA_CMD_SET_DIGITAL_CONV_FMT1_NAUDIO;
	
	writeData16(channel->off + HDAC_SDFMT, format);
    
	/* AppleGFXHDA never uses stripe mode for HDMI audio.  Stripe causes
	 * FIFO errors (SDSTS_FIFOE) on AMD/ATI GPU HDA controllers, producing
	 * distorted / crackling audio.  Disable stripe for digital (HDMI/DP)
	 * outputs; keep it only for analog multi-channel on Intel PCH. */
	if (assoc->digital)
		channel->stripectl = 0;
	else
		channel->stripectl = calculateStripectl(static_cast<UInt8>(mSDO), channel->stripecap, format);

	for (int i = 0, chn = 0; i < 16 && channel->io[i] != -1; i++) {
		Widget *widget;
		int c, cchn;

		widget = widgetGet(channel->funcGroup, channel->io[i]);
		if (!widget)
			continue;

//		if ((assoc->hpredir >= 0) && (i == assoc->pincnt))
//			chn = 0;
		/* If HP redirection is enabled, but failed to use same DAC make last DAC one to duplicate first one. */
		if (assoc->fakeredir && i == (assoc->pincnt - 1)) {
			c = (channel->streamId << 4);
			chn = 0;
			} else {
				if (map >= 0) /* Map known speaker setups. */
					chn = (((chmap[map][(totalchn / 2) > 4 ? 4 : (totalchn / 2)] >> i * 4) & 0xf) - 1) * 2;
				if (chn < 0 || chn >= totalchn) {
				/* This is until OSS will support multichannel. Should be: c = 0; to disable unused DAC */
				c = 0;
			} else {
				c = (channel->streamId << 4) | chn;
			}			
		}		
		if(mVerbose >= 2)
			logMsg("PCMDIR_%s: Stream setup nid=%d format=%08lx speed=%ld , dfmt=0x%04x, chan=0x%04x, stripe=%d\n",
				   (channel->direction == PCMDIR_PLAY) ?"PLAY" : "REC", channel->io[i], 
				   (long unsigned int)channel->format, (long int)channel->speed, digFormat, c, channel->stripectl);
		
		
//		logMsg("PCMDIR_%s: Stream setup nid=%d: format=0x%04x, digFormat=0x%04x\n",
//				(channel->direction == PCMDIR_PLAY) ? "PLAY" : "REC", channel->io[i], format, digFormat);
		sendCommand(HDA_CMD_SET_CONV_FMT(cad, channel->io[i], format), cad);
		if (HDA_PARAM_AUDIO_WIDGET_CAP_DIGITAL(widget->params.widgetCap)) {
			/* Send BOTH digital converter verbs matching AppleGFXHDA:
			 * 0x70e (FMT2/IEC60958 category) FIRST, then 0x70d (FMT1/enable) */
			UInt8 digFmt2 = (channel->format & AFMT_AC3) ? 0x19 : 0x01; /* category code */
			sendCommand(HDA_CMD_12BIT(cad, channel->io[i], 0x70e, digFmt2), cad);
			sendCommand(HDA_CMD_SET_DIGITAL_CONV_FMT1(cad, channel->io[i], digFormat), cad);
		}
		sendCommand(HDA_CMD_SET_CONV_STREAM_CHAN(cad, channel->io[i], c), cad);
		if (!c)
			continue;
		if (HDA_PARAM_AUDIO_WIDGET_CAP_STRIPE(widget->params.widgetCap))
			sendCommand(HDA_CMD_SET_STRIPE_CONTROL(cad, channel->io[i], channel->stripectl), cad);
		cchn = HDA_PARAM_AUDIO_WIDGET_CAP_CC(widget->params.widgetCap);
		if (cchn > 1 && chn < totalchn) {
			cchn = min(cchn, totalchn - chn - 1);
			sendCommand(HDA_CMD_SET_CONV_CHAN_COUNT(cad, channel->io[i], cchn), cad);
		}
		if (HDA_PARAM_AUDIO_WIDGET_CAP_DIGITAL(widget->params.widgetCap) && mGFXController)
			mGFXController->setupStream(channel, channel->io[i], assoc, totalchn, totalextchn);

		chn += cchn + 1;
	}
}

void VoodooHDADevice::streamStop(Channel *channel)
{
	UInt32 ctl;
	int streamIndex;

	if (!channel || !mRegBase || channel->off < 0)
		return;

	streamIndex = channel->off >> 5;
	if (streamIndex < 0 || streamIndex >= 32)
		return;

	ctl = readData8(channel->off + HDAC_SDCTL0);
	ctl &= ~(HDAC_SDCTL_IOCE | HDAC_SDCTL_FEIE | HDAC_SDCTL_DEIE | HDAC_SDCTL_RUN);
	writeData8(channel->off + HDAC_SDCTL0, ctl);

	channel->flags &= ~HDAC_CHN_RUNNING;

	ctl = readData32(HDAC_INTCTL);
	ctl &= ~(1U << streamIndex);
	writeData32(HDAC_INTCTL, ctl);
}

void VoodooHDADevice::streamStart(Channel *channel)
{
	UInt32 ctl;
	int streamIndex;

	if (!channel || !mRegBase || channel->off < 0)
		return;

	streamIndex = channel->off >> 5;
	if (streamIndex < 0 || streamIndex >= 32)
		return;

	channel->flags |= HDAC_CHN_RUNNING;

	ctl = readData32(HDAC_INTCTL);
	ctl |= 1U << streamIndex;
	writeData32(HDAC_INTCTL, ctl);
  
  //FreeBSD update
//  HDAC_WRITE_1(&sc->mem, off + HDAC_SDSTS, HDAC_SDSTS_DESE | HDAC_SDSTS_FIFOE | HDAC_SDSTS_BCIS);
  writeData8(channel->off + HDAC_SDSTS, HDAC_SDSTS_DESE | HDAC_SDSTS_FIFOE | HDAC_SDSTS_BCIS);
  //

	if (channel->stripectl) {
		ctl = readData8(channel->off + HDAC_SDCTL2);
		ctl &= ~HDAC_SDCTL2_STRIPE_MASK;
		ctl |= channel->stripectl << HDAC_SDCTL2_STRIPE_SHIFT;
		writeData8(channel->off + HDAC_SDCTL2, ctl);
	}

	ctl = readData8(channel->off + HDAC_SDCTL0);
	ctl |= HDAC_SDCTL_IOCE | HDAC_SDCTL_FEIE | HDAC_SDCTL_DEIE | HDAC_SDCTL_RUN;
	writeData8(channel->off + HDAC_SDCTL0, ctl);
}

void VoodooHDADevice::streamReset(Channel *channel)
{
	int timeout = 1000;
	int to = timeout;
	UInt32 ctl;

	if (!channel || !mRegBase || channel->off < 0)
		return;

	ctl = readData8(channel->off + HDAC_SDCTL0);
	ctl |= HDAC_SDCTL_SRST;
	writeData8(channel->off + HDAC_SDCTL0, ctl);
	do {
		ctl = readData8(channel->off + HDAC_SDCTL0);
		if (ctl & HDAC_SDCTL_SRST)
			break;
		IODelay(10);
	} while (--to);
	if (!(ctl & HDAC_SDCTL_SRST))
		errorMsg("timeout in reset\n");
	ctl &= ~HDAC_SDCTL_SRST;
	writeData8(channel->off + HDAC_SDCTL0, ctl);
	to = timeout;
	do {
		ctl = readData8(channel->off + HDAC_SDCTL0);
		if (!(ctl & HDAC_SDCTL_SRST))
			break;
		IODelay(10);
	} while (--to);
	if (ctl & HDAC_SDCTL_SRST)
		errorMsg("can't reset!\n");
}

void VoodooHDADevice::streamSetId(Channel *channel)
{
	UInt32 ctl;

	if (!channel || !mRegBase || channel->off < 0)
		return;

	ctl = readData8(channel->off + HDAC_SDCTL2);
	ctl &= ~(HDAC_SDCTL2_STRM_MASK | HDAC_SDCTL2_STRIPE_MASK);
	ctl |= channel->streamId << HDAC_SDCTL2_STRM_SHIFT;
	writeData8(channel->off + HDAC_SDCTL2, ctl);
}

/*******************************************************************************************/
/*******************************************************************************************/

void VoodooHDADevice::bdlSetup(Channel *channel)
{
	BdlEntry *bdlEntry;
	UInt64 addr;
	UInt32 blockSize, numBlocks;

	if (!channel || !mRegBase || !channel->buffer || !channel->bdlMem ||
	    !channel->buffer->physAddr || !channel->bdlMem->virtAddr) {
		errorMsg("error: invalid BDL setup state\n");
		return;
	}

	addr = (UInt64) channel->buffer->physAddr;
	bdlEntry = (BdlEntry *) channel->bdlMem->virtAddr;

	blockSize = channel->blockSize;
	numBlocks = channel->numBlocks;

	for (UInt32 n = 1; n <= numBlocks; n++, bdlEntry++) {
		bdlEntry->addrl = (UInt32) addr;
		bdlEntry->addrh = (UInt32) (addr >> 32);
		bdlEntry->len = ((n == numBlocks) ? (blockSize - channel->slack) : blockSize);
		/* After moving HDMI timing to SDLPIB polling, digital streams can use
		 * the same final-entry IOC cadence as AppleGFXHDA. */
		bdlEntry->ioc = (n == numBlocks);
		addr += bdlEntry->len;
	}

	writeData32(channel->off + HDAC_SDCBL, blockSize * numBlocks - channel->slack);
	writeData16(channel->off + HDAC_SDLVI, numBlocks - 1);
	addr = channel->bdlMem->physAddr;
	writeData32(channel->off + HDAC_SDBDPL, (UInt32) addr);
	writeData32(channel->off + HDAC_SDBDPU, (UInt32) (addr >> 32));
	if (channel->dmaPos && !(readData32(HDAC_DPIBLBASE) & 0x00000001)) {
		addr = mDmaPosMem->physAddr;
		writeData32(HDAC_DPIBLBASE, ((UInt32) addr & HDAC_DPLBASE_DPLBASE_MASK) | 0x00000001);
		writeData32(HDAC_DPIBUBASE, (UInt32) (addr >> 32));
	}
}

int VoodooHDADevice::bdlAlloc(Channel *channel)
{
	if (!channel || !channel->pcmDevice ||
	    channel->pcmDevice->chanNumBlocks < HDA_BDL_MIN ||
	    channel->pcmDevice->chanNumBlocks > HDA_BDL_MAX) {
		errorMsg("error: invalid BDL allocation state\n");
		return -1;
	}

	PcmDevice *pcmDevice = channel->pcmDevice;

	channel->bdlMem = allocateDmaMemory(sizeof (BdlEntry) * pcmDevice->chanNumBlocks, "bdlMem", kIOMapWriteThruCache);
	if (!channel->bdlMem) {
		errorMsg("error: couldn't allocate bdl\n");
		return -1;
	}

	return 0;
}

/*******************************************************************************************/
/*******************************************************************************************/

int VoodooHDADevice::pcmAttach(PcmDevice *pcmDevice)
{
	if (!pcmDevice || !pcmDevice->funcGroup || !pcmDevice->funcGroup->codec) {
		errorMsg("error: invalid PCM device attach request\n");
		return -1;
	}

	char buf[256];
	snprintf(buf, sizeof (buf), "HDA %s PCM #%d %s at cad %d nid %d",
			findCodecName(pcmDevice->funcGroup->codec), pcmDevice->index,
	//			 pcmDevice->digital ?"Digital" : "Analog",
			 (pcmDevice->digital == 3)?"DisplayPort":
			 ((pcmDevice->digital == 2)?"HDMI":
			  ((pcmDevice->digital)?"Digital":"Analog")),
			 pcmDevice->funcGroup->codec->cad, pcmDevice->funcGroup->nid);
	dumpMsg("pcmAttach: %s\n", buf);

	/*
	 * Keep HDMI/DP on the larger default DMA buffer for stability, but use
	 * a smaller analog buffer to reduce perceived latency. On the common
	 * 2ch/32-bit CoreAudio path this changes the published buffer from
	 * 32768 frames to 16384 frames without touching digital outputs.
	 */
	pcmDevice->chanSize = pcmDevice->digital ? HDA_BUFSZ_DEFAULT : HDA_BUFSZ_ANALOG_LOW_LATENCY;
	/* Digital outputs use page-sized BDL entries like AppleGFXHDA. */
	if (pcmDevice->digital)
		pcmDevice->chanNumBlocks = pcmDevice->chanSize / HDA_BUFSZ_MIN; /* 4KB per entry */
	else
		pcmDevice->chanNumBlocks = HDA_BDL_DEFAULT;
	if (mVerbose >= 1) {
		IOLog("VoodooHDA DBG: pcmAttach %s chanSize=%d chanNumBlocks=%d blockSize=%d digital=%d\n",
		      buf, pcmDevice->chanSize, pcmDevice->chanNumBlocks,
		      pcmDevice->chanSize / pcmDevice->chanNumBlocks, pcmDevice->digital);
	}

	dumpMsg("+--------------------------------------+\n");
	dumpMsg("| DUMPING PCM Playback/Record Channels |\n");
	dumpMsg("+--------------------------------------+\n");
	dumpPcmChannels(pcmDevice);
	dumpMsg("\n");
	dumpMsg("+-------------------------------+\n");
	dumpMsg("| DUMPING Playback/Record Paths |\n");
	dumpMsg("+-------------------------------+\n");
	dumpDac(pcmDevice);
	dumpAdc(pcmDevice);
	dumpMix(pcmDevice);
	dumpMsg("\n");
	dumpMsg("+-------------------------+\n");
	dumpMsg("| DUMPING Volume Controls |\n");
	dumpMsg("+-------------------------+\n");
	dumpCtls(pcmDevice, "Master Volume", SOUND_MASK_VOLUME);
	dumpCtls(pcmDevice, "PCM Volume", SOUND_MASK_PCM);
	dumpCtls(pcmDevice, "CD Volume", SOUND_MASK_CD);
	dumpCtls(pcmDevice, "Microphone Volume", SOUND_MASK_MIC);
	dumpCtls(pcmDevice, "Microphone2 Volume", SOUND_MASK_MONITOR);
	dumpCtls(pcmDevice, "Line-in Volume", SOUND_MASK_LINE);
	dumpCtls(pcmDevice, "Speaker/Beep Volume", SOUND_MASK_SPEAKER);
	dumpCtls(pcmDevice, "Recording Level", SOUND_MASK_RECLEV);
	dumpCtls(pcmDevice, "Input Mix Level", SOUND_MASK_IMIX);
	dumpCtls(pcmDevice, "Input Monitoring Level", SOUND_MASK_IGAIN);
	dumpCtls(pcmDevice, NULL, 0);
	dumpMsg("\n");

	dumpMsg("OSS mixer initialization...\n");
	
	if (audioCtlOssMixerInit(pcmDevice) != 0) {
		errorMsg("warning: mixer initialization failed\n");
	}
	
	//logMsg("VoodooHDADevice::mixerSetDefaults begin\n");
	UNLOCK(); // xxx
	//logMsg("VoodooHDADevice::mixerSetDefaults mid\n");
	mixerSetDefaults(pcmDevice);
	LOCK(); // xxx
	//logMsg("VoodooHDADevice::mixerSetDefaults end\n");

	dumpMsg("Registering PCM channels...\n");
	if (pcmDevice->playChanId >= 0)
		channelInit(pcmDevice, PCMDIR_PLAY); // mChannels[pcmDevice->playChanId]
	if (pcmDevice->recChanId >= 0)
		channelInit(pcmDevice, PCMDIR_REC); // mChannels[pcmDevice->recChanId]
	pcmDevice->registered = true;

	return 0;
}

//Создаем разделяемую область памяти, откуда будет брать информацию PrefPanel
void VoodooHDADevice::createPrefPanelMemoryBuf(FunctionGroup *funcGroup)
{

	//logMsg("VoodooHDADevice::createPrefPanelMemoryBuf\n");

	createPrefPanelStruct(funcGroup);

	//logMsg("VoodooHDADevice::createPrefPanelMemoryBuf mPrefPanelMemoryBuf = 0x%08X\n", mPrefPanelMemoryBuf);

	if(mPrefPanelMemoryBuf == 0) {
		//logMsg("VoodooHDADevice::createPrefPanelMemoryBuf allocate memory\n");
		//mPrefPanelMemoryBufSize = nSliderTabsCount*sizeof(sliders);
			mPrefPanelMemoryBufSize = kVoodooHDAPrefPanelMaxChannels * sizeof(ChannelInfo);
		mPrefPanelMemoryBuf = (ChannelInfo*)allocMem(mPrefPanelMemoryBufSize);
		if (!mPrefPanelMemoryBuf) {
			errorMsg("error: couldn't allocate pref panel memory buffer (%ld bytes)\n", mPrefPanelMemoryBufSize);
			return;
		}
		bzero(mPrefPanelMemoryBuf, mPrefPanelMemoryBufSize); 

		mPrefPanelMemoryBufLock = IOLockAlloc();
		if (!mPrefPanelMemoryBufLock) {
			errorMsg("error: couldn't allocate pref panel memory lock\n");
			FREE(mPrefPanelMemoryBuf);
			mPrefPanelMemoryBufSize = 0;
			return;
		}
		mPrefPanelMemoryBufEnabled = false;
	}

		int tabCount = nSliderTabsCount;
		if (tabCount > kVoodooHDAPrefPanelMaxChannels)
			tabCount = kVoodooHDAPrefPanelMaxChannels;
		for(int i = 0; i < tabCount; i++) {
			VoodooHDAEngine *engine = lookupEngine(i);
			strlcpy(mPrefPanelMemoryBuf[i].name, sliderTabs[i].name, MAX_SLIDER_TAB_NAME_LENGTH);
			mPrefPanelMemoryBuf[i].numChannels = tabCount;
		for(int j = 1; j < 25; j++) {
			if(sliderTabs[i].volSliders[j].enabled == 0) 
				continue;

			mPrefPanelMemoryBuf[i].mixerValues[j - 1].mixId = j;
			strlcpy(mPrefPanelMemoryBuf[i].mixerValues[j - 1].name, sliderTabs[i].volSliders[j].name, 32);
			mPrefPanelMemoryBuf[i].mixerValues[j - 1].enabled = 1;
			mPrefPanelMemoryBuf[i].mixerValues[j - 1].value = mMixerDefaults[j];
		}
		mPrefPanelMemoryBuf[i].mixerValues[24].mixId = 0;
		mPrefPanelMemoryBuf[i].mixerValues[24].enabled = true;
		mPrefPanelMemoryBuf[i].mixerValues[24].value = mMixerDefaults[0];
		mPrefPanelMemoryBuf[i].vectorize = vectorize;
		mPrefPanelMemoryBuf[i].noiseLevel = noiseLevel;
		mPrefPanelMemoryBuf[i].useStereo = useStereo;
		mPrefPanelMemoryBuf[i].StereoBase = StereoBase;
		mPrefPanelMemoryBuf[i].digital = sliderTabs[i].pcmDevice ? sliderTabs[i].pcmDevice->digital : 0;
			mPrefPanelMemoryBuf[i].direction = (engine && engine->mChannel) ?
			    static_cast<SInt8>(engine->mChannel->direction) : 0;
			mPrefPanelMemoryBuf[i].diagnosticFlags = (engine && engine->mChannel) ?
			    engine->mChannel->diagnosticFlags : 0;
		mPrefPanelMemoryBuf[i].debugLevel = static_cast<UInt8>(mVerbose & 0xff);
		mPrefPanelMemoryBuf[i].buildFlags = VOODOO_HDA_DEBUG_BUILD ? kVoodooHDABuildSupportsDebug : 0;
	}
}

//AutumnRain
//Создаем структуру в которой запомним какие объекты AudioControl каким регуляторам на панели PrefPanel соотвествуют
void VoodooHDADevice::createPrefPanelStruct(FunctionGroup *funcGroup)
{
	//logMsg("createPrefPanelStruct: codec %d have %d assocNum\n", funcGroup->codec->cad, funcGroup->audio.numAssocs);
	if (!funcGroup || !funcGroup->audio.assocs)
		return;
		
	//Перебираем все ассоциации которые были созданы ранее
	for(int i = 0; i < funcGroup->audio.numAssocs; i++) {
		if (nSliderTabsCount >= kVoodooHDAPrefPanelMaxChannels) {
			errorMsg("warning: pref pane channel list truncated at %d associations\n",
			    kVoodooHDAPrefPanelMaxChannels);
			break;
		}
		//Получаем ноду которая является главной в ассоциации - это, как правило, устройство к которому или от которого приходит сигнал
		nid_t mainNid = funcGroup->audio.assocs[i].pins[0];
		Widget *mainWidget = widgetGet(funcGroup, mainNid);
		if(mainWidget) {
			//logMsg("createPrefPanelStruct:    Assoc %d have main nid 0x%X %s\n", i, mainNid, mainWidget->name);
			//logMsg("createPrefPanelStruct:    ctrl = %d  ossmask = 0x%08X\n", mainWidget->pin.ctrl, mainWidget->ossmask); 
			 //В соответствии с названием устройства называем вкладку
			//catPinName(mainWidget); //->pin.config, sliderTabs[nSliderTabsCount].name, MAX_SLIDER_TAB_NAME_LENGTH);
			//sliderTabs[nSliderTabsCount].name = (char *)&mainWidget->name[5];
			strlcpy(sliderTabs[nSliderTabsCount].name, &mainWidget->name[5],
			    MAX_SLIDER_TAB_NAME_LENGTH);
		}
		AudioControl *control;
		UInt32 ossmask = 0;
		PcmDevice* pcmDevice = 0;
		PcmDevice* curPCMDevice = 0;
		char buf[255];
		//Теперь ищем OSS устройства которые влияют на сигнал проходящий по всем нодам текущей ассоциации
		for(int j = 0; (control = audioCtlEach(funcGroup, j)); j++) {
			if((control->enable == 0) || (control->widget->enable == 0))
				continue;
			
			if(control->widget->bindAssoc == i) {
				ossmask |= control->ossmask;
				//Slice - dangerous mode
				if(control->ossmask & SOUND_MASK_MONITOR)
					ossmask |= SOUND_MASK_MIC;
				
				//logMsg("createPrefPanelStruct:        audioControl %d ossmask = 0x%08lx\n", j, (long unsigned int)ossmask);
				
			}
			//Ищем PCM устройство к которому принадлежит OSS устройство
			for(int pcmDeviceIndex = 0; pcmDeviceIndex < funcGroup->audio.numPcmDevices; pcmDeviceIndex++) {
				curPCMDevice = &funcGroup->audio.pcmDevices[pcmDeviceIndex];
					if(curPCMDevice->playChanId >= 0 && curPCMDevice->playChanId < mNumChannels &&
					    mChannels[curPCMDevice->playChanId].assocNum == i)
						pcmDevice = curPCMDevice;
					if(curPCMDevice->recChanId >= 0 && curPCMDevice->recChanId < mNumChannels &&
					    mChannels[curPCMDevice->recChanId].assocNum == i)
						pcmDevice = curPCMDevice;
			}
		}
		//logMsg("createPrefPanelStruct:         ossdev %s, pcmDev = %d\n", audioCtlMixerMaskToString(ossmask, buf, sizeof(buf)), pcmDeviceNum);
		
		if (pcmDevice && pcmDevice->digital >= 2) {
			// HDMI/DP: no hardware amp controls — expose PCM slider only (for soft volume)
			ossmask = SOUND_MASK_PCM;
		} else if (ossmask & SOUND_MASK_PCM) {
			ossmask |= SOUND_MASK_IMIX;
		}

		sliderTabs[nSliderTabsCount].pcmDevice = pcmDevice;
		//Создаем регуляторы на текущей вкладке
		for(int j = 0; j < 32; j++) {
			if(ossmask & (1 << j)) {
				//logMsg("%d (%s), ", j, audioCtlMixerMaskToString(1 << j, buf, sizeof(buf)));
				sliderTabs[nSliderTabsCount].volSliders[j].enabled = true;
				strlcpy(sliderTabs[nSliderTabsCount].volSliders[j].name, audioCtlMixerMaskToString(1 << j, buf, sizeof(buf)), 32);
				sliderTabs[nSliderTabsCount].volSliders[j].ossdev = j;
			}
		}
		//logMsg("\n");
		nSliderTabsCount++;
	}
}

//Считываем текущее настройки усиления и сохраняем их в разделяемой памяти
void VoodooHDADevice::updatePrefPanelMemoryBuf(void)
{

	//logMsg("VoodooHDADevice::updatePrefPanelMemoryBuf\n");
	if (!mPrefPanelMemoryBuf)
		return;

	int tabCount = nSliderTabsCount;
	if (tabCount > kVoodooHDAPrefPanelMaxChannels)
		tabCount = kVoodooHDAPrefPanelMaxChannels;
	for(int i = 0; i < tabCount; i++) {
		if(sliderTabs[i].pcmDevice == 0) continue;
		VoodooHDAEngine *engine = lookupEngine(i);

		for(int j = 1; j < 25; j++) {
			if(sliderTabs[i].volSliders[j].enabled == 0)
				continue;

			mPrefPanelMemoryBuf[i].mixerValues[j - 1].value = sliderTabs[i].pcmDevice->left[j];
		}
		mPrefPanelMemoryBuf[i].mixerValues[24].value = sliderTabs[i].pcmDevice->left[0];// mMixerDefaults[0];
		if (engine && engine->mChannel) {
			mPrefPanelMemoryBuf[i].vectorize = engine->mChannel->vectorize;
			mPrefPanelMemoryBuf[i].noiseLevel = engine->mChannel->noiseLevel;
			mPrefPanelMemoryBuf[i].useStereo = engine->mChannel->useStereo;
			mPrefPanelMemoryBuf[i].StereoBase = engine->mChannel->StereoBase;
			mPrefPanelMemoryBuf[i].digital = engine->mChannel->pcmDevice ? engine->mChannel->pcmDevice->digital : 0;
			mPrefPanelMemoryBuf[i].direction = static_cast<SInt8>(engine->mChannel->direction);
			mPrefPanelMemoryBuf[i].diagnosticFlags = engine->mChannel->diagnosticFlags;
			mPrefPanelMemoryBuf[i].debugLevel = static_cast<UInt8>(mVerbose & 0xff);
			mPrefPanelMemoryBuf[i].buildFlags = VOODOO_HDA_DEBUG_BUILD ? kVoodooHDABuildSupportsDebug : 0;
		}
	}
}

//Функция меняет значения усиления для регулятора
void VoodooHDADevice::changeSliderValue(UInt8 tabNum, UInt8 sliderNum, UInt8 newValue)
{
	//logMsg("change for Device (%d) ossSlider (%d) value to %d\n", tabNum, sliderNum, newValue);
	if(tabNum < nSliderTabsCount) {
		PcmDevice* _pcmDevice = sliderTabs[tabNum].pcmDevice;
		if(_pcmDevice)
			audioCtlOssMixerSet(_pcmDevice, sliderNum, newValue, newValue);
	}
}

//Slice
void VoodooHDADevice::setMath(UInt8 tabNum, UInt8 sliderNum, UInt8 newValue)
{
	VoodooHDAEngine *engine;
	engine = lookupEngine(tabNum);
	if (!engine || !engine->mChannel) return;
	UInt8 n, b;
	bool v = ((sliderNum & 1) == 1);
	bool s = ((sliderNum & 2) == 2);
	n = newValue & 0x0f;
	b = (newValue & 0xf0) >> 4;
	engine->mChannel->vectorize = v;
	engine->mChannel->useStereo = s;
	engine->mChannel->noiseLevel = n;
	engine->mChannel->StereoBase = b;
	
}

void VoodooHDADevice::setDiagnosticFlags(UInt8 tabNum, UInt16 flags)
{
	VoodooHDAEngine *engine;

	engine = lookupEngine(tabNum);
	if (!engine || !engine->mChannel)
		return;

	engine->mChannel->diagnosticFlags = flags;
	engine->resetDiagnosticState();
}

UInt16 VoodooHDADevice::diagnosticFlagsForPin(int cad, nid_t pinNid) const
{
	int hdmiEngineCount = (mNumHDMIEngines > 16) ? 16 : mNumHDMIEngines;
	if (hdmiEngineCount < 0)
		hdmiEngineCount = 0;

	for (int i = 0; i < hdmiEngineCount; i++) {
		const HDMIEngineSlot *slot = &mHDMIEngines[i];
		if (slot->cad != cad || slot->pinNid != pinNid || !slot->channel)
			continue;
		return slot->channel->diagnosticFlags;
	}
	return 0;
}

void VoodooHDADevice::setDebugLevel(UInt8 level)
{
#if VOODOO_HDA_DEBUG_BUILD
	if (level > 4)
		level = 4;
	mVerbose = level;
	updatePrefPanelMemoryBuf();
#else
	(void)level;
#endif
}

bool VoodooHDADevice::getDiagnosticTelemetry(UInt8 tabNum, VoodooHDADiagTelemetry *telemetry)
{
#if !VOODOO_HDA_DEBUG_BUILD
	(void)tabNum;
	(void)telemetry;
	return false;
#else
	VoodooHDAEngine *engine;
	Channel *channel;
	PcmDevice *pcmDevice;
	Codec *codec = NULL;
	bool linkValid = false;
	bool streamActive = false;
	UInt32 linkPosition = 0;
	UInt32 clippedPosition = 0;

	if (!telemetry)
		return false;

	engine = lookupEngine(tabNum);
	if (!engine || !engine->mChannel)
		return false;

	channel = engine->mChannel;
	pcmDevice = channel->pcmDevice;
	if (channel->funcGroup)
		codec = channel->funcGroup->codec;

	bzero(telemetry, sizeof(*telemetry));
	telemetry->magic = kVoodooHDADiagTelemetryMagic;
	telemetry->version = kVoodooHDADiagTelemetryVersion;
	telemetry->size = sizeof(*telemetry);
	telemetry->channel = tabNum;
	telemetry->numChannels = mNumChannels;
	telemetry->buildFlags = kVoodooHDABuildSupportsDebug;
	telemetry->debugLevel = static_cast<UInt8>(mVerbose & 0xff);
	telemetry->digital = pcmDevice ? pcmDevice->digital : 0;
	telemetry->direction = static_cast<SInt8>(channel->direction);
	telemetry->running = (channel->flags & HDAC_CHN_RUNNING) ? 1 : 0;
	telemetry->diagnosticFlags = channel->diagnosticFlags;
	telemetry->codecVendor = codec ? codec->vendorId : 0;
	telemetry->codecDevice = codec ? codec->deviceId : 0;
	telemetry->codecFamily = codec ? static_cast<UInt16>(appleGfxHdaAmdCodecFamily(codec->deviceId)) : 0;
	telemetry->speed = channel->speed;
	telemetry->format = channel->format;
	telemetry->streamId = static_cast<UInt32>(channel->streamId);
	telemetry->streamOffset = static_cast<UInt32>(channel->off);
	telemetry->numBlocks = channel->numBlocks;
	telemetry->blockSize = channel->blockSize;
	telemetry->bufferSize = channel->buffer ? static_cast<UInt32>(channel->buffer->size) : 0;
	telemetry->slack = channel->slack;
	telemetry->numSampleFrames = engine->mNumSampleFrames;
	telemetry->sampleSize = engine->mSampleSize;
	telemetry->diagnosticBufferPrimed = channel->diagnosticBufferPrimed ? 1 : 0;
	telemetry->diagnosticClipCalls = channel->diagnosticClipCalls;
	telemetry->diagnosticMixToneFills = channel->diagnosticMixToneFills;
	telemetry->diagnosticDirectToneFills = channel->diagnosticDirectToneFills;
	telemetry->diagnosticEraseCalls = channel->diagnosticEraseCalls;
	telemetry->diagnosticEraseSkips = channel->diagnosticEraseSkips;
	telemetry->diagnosticLastFirstFrame = channel->diagnosticLastFirstFrame;
	telemetry->diagnosticLastNumFrames = channel->diagnosticLastNumFrames;

	if (engine->mPortName)
		strlcpy(telemetry->channelName, engine->mPortName, sizeof(telemetry->channelName));

	if (mGFXController && mGFXController->ownsChannel(channel)) {
		linkPosition = mGFXController->getLinkPosition(channel, &linkValid);
		clippedPosition = mGFXController->getClippedPosition(channel, &streamActive);
	} else {
		linkPosition = static_cast<UInt32>(channelGetPosition(channel));
		linkValid = true;
	}

	telemetry->streamActive = streamActive ? 1 : 0;
	telemetry->linkPosition = linkPosition;
	telemetry->linkPositionValid = linkValid ? 1 : 0;
	telemetry->currentSampleFrame = engine->getCurrentSampleFrame();
	telemetry->clippedPosition = clippedPosition;

	int hdmiEngineCount = (mNumHDMIEngines > 16) ? 16 : mNumHDMIEngines;
	if (hdmiEngineCount < 0)
		hdmiEngineCount = 0;

	for (int i = 0; i < hdmiEngineCount; i++) {
		const HDMIEngineSlot *slot = &mHDMIEngines[i];
		if (slot->channel != channel)
			continue;
		telemetry->pinNid = slot->pinNid;
		telemetry->cad = slot->cad;
		break;
	}

	return true;
#endif
}

void VoodooHDADevice::freePrefPanelMemoryBuf(void)
{
	if (mPrefPanelMemoryBufLock)
		IOLockLock(mPrefPanelMemoryBufLock);
	mPrefPanelMemoryBufEnabled = false;
	FREE(mPrefPanelMemoryBuf);
	mPrefPanelMemoryBufSize = 0;
	if (mPrefPanelMemoryBufLock)
		IOLockUnlock(mPrefPanelMemoryBufLock);
}

void VoodooHDADevice::lockPrefPanelMemoryBuf()
{
	if (mPrefPanelMemoryBufLock)
		IOLockLock(mPrefPanelMemoryBufLock);
}

void VoodooHDADevice::unlockPrefPanelMemoryBuf()
{
	if (mPrefPanelMemoryBufLock)
		IOLockUnlock(mPrefPanelMemoryBufLock);
}

void VoodooHDADevice::lockExtMsgBuffer()
{
	if (mExtMessageLock)
		IOLockLock(mExtMessageLock);
}

void VoodooHDADevice::unlockExtMsgBuffer()
{
	if (mExtMessageLock)
		IOLockUnlock(mExtMessageLock);
}

void VoodooHDADevice::dumpExtMsg(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	bool lockExists;
	int length;
	
	lockExists = (!isInactive() && mExtMessageLock);
	if (lockExists)
		lockExtMsgBuffer();
	
	if (mExtMsgBuffer && mExtMsgBufferSize > 1) {
		if (mExtMsgBufferPos >= (mExtMsgBufferSize - 1)) {
			mExtMsgBufferPos = mExtMsgBufferSize - 1;
			mExtMsgBuffer[mExtMsgBufferPos] = '\0';
		} else {
			length = vsnprintf(mExtMsgBuffer + mExtMsgBufferPos,
			                   mExtMsgBufferSize - mExtMsgBufferPos,
			                   format, args);
			if (length > 0) {
				size_t remaining = mExtMsgBufferSize - mExtMsgBufferPos;
				if ((size_t)length >= remaining)
					mExtMsgBufferPos = mExtMsgBufferSize - 1;
				else
					mExtMsgBufferPos += length;
			} else if (length < 0) {
				IOLog("warning: vsnprintf in dumpExtMsg failed\n");
			}
		}
	}
	
	if (lockExists)
		unlockExtMsgBuffer();
	
	va_end(args);
}
