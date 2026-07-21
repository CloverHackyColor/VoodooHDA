#include "License.h"

#include "VoodooHDAFramebufferNotifier.h"
#include "VoodooHDADevice.h"
#include "VoodooHDAEngine.h"
#include "Verbs.h"

#include <IOKit/pci/IOPCIDevice.h>

OSDefineMetaClassAndStructors(VoodooHDAFramebufferNotifier, OSObject)

#undef super
#define super OSObject

#define FBLOG(fmt, ...) IOLog("VoodooHDA FB: " fmt "\n", ##__VA_ARGS__)

static VoodooHDAAMDGPUFamily classifyAMDGPUDevice(UInt16 gpuDeviceId);
static const char *amdHdmiGpuFamilyNameForFamily(VoodooHDAAMDGPUFamily family);
//static const char *amdHdmiGpuFamilyName(UInt16 gpuDeviceId);

static bool vhdaIsLegacyPolarisHDADevice(IOPCIDevice *hdaDevice)
{
  if (!hdaDevice)
    return false;
  if (hdaDevice->configRead16(kIOPCIConfigVendorID) != 0x1002)
    return false;
  
  switch (hdaDevice->configRead16(kIOPCIConfigDeviceID)) {
    case 0xaae0:
    case 0xab00:
    case 0xaaf0:
    case 0xaaf8:
      return true;
    default:
      return false;
  }
}

static bool vhdaReadAMDAudioCodecInfoPin(IOService *framebuffer, nid_t *outPin)
{
  if (!framebuffer || !outPin)
    return false;
  OSData *codecInfo = OSDynamicCast(OSData, framebuffer->getProperty("audio-codec-info"));
  if (!codecInfo || codecInfo->getLength() < 4)
    return false;
  const UInt8 *bytes = (const UInt8 *)codecInfo->getBytesNoCopy();
  if (!bytes)
    return false;
  
  /*
   * AMDFramebuffer publishes audio-codec-info as four bytes.  On Polaris
   * RX580 logs we saw <00 01 09 00>, where byte[2] is the HDA pin NID
   * used by the physical display connector.  Accept only real HDMI pin NIDs
   * from the normal ATI table range to avoid treating garbage as a pin.
   */
  nid_t pin = (nid_t)bytes[2];
  if (pin < 3 || pin > 31)
    return false;
  *outPin = pin;
  return true;
}


static bool vhdaIsIONDRVFramebuffer(IOService *framebuffer)
{
  if (!framebuffer)
    return false;
  
  OSString *publisher = OSDynamicCast(OSString, framebuffer->getProperty("IOPersonalityPublisher"));
  if (publisher && publisher->isEqualTo("com.apple.iokit.IONDRVSupport"))
    return true;
  
  OSString *className = OSDynamicCast(OSString, framebuffer->getProperty("IOClass"));
  if (className && className->isEqualTo("IONDRVFramebuffer"))
    return true;
  
  return false;
}

static bool vhdaParseRegistryAtIndex(const char *name, int *outIndex)
{
  if (!name || !outIndex)
    return false;
  
  for (const char *p = name; *p; p++) {
    if (*p != '@')
      continue;
    p++;
    int value = 0;
    bool haveDigit = false;
    while (*p >= '0' && *p <= '9') {
      haveDigit = true;
      value = (value * 10) + (*p - '0');
      p++;
    }
    if (haveDigit) {
      *outIndex = value;
      return true;
    }
  }
  return false;
}

static bool vhdaGetFramebufferConnectorIndex(IOService *framebuffer, int *outIndex)
{
  if (!framebuffer || !outIndex)
    return false;
  
  IOService *service = framebuffer;
  for (int depth = 0; depth < 6 && service; depth++) {
    if (vhdaParseRegistryAtIndex(service->getName(), outIndex))
      return true;
    service = service->getProvider();
  }
  return false;
}

static const char *amdHdmiGpuFamilyNameForFamily(VoodooHDAAMDGPUFamily family)
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

static VoodooHDAAMDGPUFamily classifyAMDGPUDevice(UInt16 gpuDeviceId)
{
  /* Polaris RX4xx/RX5xx: Ellesmere/Baffin/Lexa/Polaris30.  Keep this
   * narrower than the old broad 0x66xx rule so Radeon VII/Vega20 (0x66af)
   * is not accidentally treated as Polaris. */
  if ((gpuDeviceId & 0xFF00) == 0x6700 ||
      (gpuDeviceId & 0xFF00) == 0x6F00 ||
      (gpuDeviceId & 0xFFF0) == 0x6980 ||
      (gpuDeviceId & 0xFFF0) == 0x6990)
    return kVoodooHDAAMDGPUClassicPolaris;
  
  /* Vega 10/12 family: RX Vega 56/64 and related boards. */
  if ((gpuDeviceId & 0xFFF0) == 0x6860 ||
      (gpuDeviceId & 0xFFF0) == 0x6870 ||
      (gpuDeviceId & 0xFFF0) == 0x69A0)
    return kVoodooHDAAMDGPUVega;
  
  /* Vega 20: AMD Radeon VII. */
  if ((gpuDeviceId & 0xFFF0) == 0x66A0)
    return kVoodooHDAAMDGPUVega20RadeonVII;
  
  /* Navi/RDNA1/RDNA2: RX5500/5600/5700 and RX6600/6650/6700/6750/6800/
   * 6900/6950 families.  These use the EDID/framebuffer-driven policy that
   * was confirmed working on RX6600. */
  if ((gpuDeviceId & 0xFF00) == 0x7300 ||
      (gpuDeviceId & 0xFF00) == 0x7400)
    return kVoodooHDAAMDGPUModernNavi;
  
  return kVoodooHDAAMDGPUGenericAMD;
}

/* ---------- lifecycle ---------- */

VoodooHDAFramebufferNotifier *
VoodooHDAFramebufferNotifier::withDevice(VoodooHDADevice *device)
{
	VoodooHDAFramebufferNotifier *me = new VoodooHDAFramebufferNotifier;
	if (me && !me->init(device)) {
		me->release();
		return NULL;
	}
	return me;
}

bool VoodooHDAFramebufferNotifier::init(VoodooHDADevice *device)
{
	if (!super::init())
		return false;

	mDevice = device;
	mGFXMatchNotifier = NULL;
	mGFXTermNotifier = NULL;
	mDisplayMatchNotifier = NULL;
	mNumConnections = 0;
	mATIPinCount = 0;
	mATIPinCad = -1;
	mHDAPciDevice = device->mPciNub;
	mGPUPciDevice = NULL;
	mGPUMMIOMap = NULL;
	mGPUMMIO = NULL;
	mGPUMMIOSize = 0;
	mGPUAudioInitDone = false;
	mRegs = NULL;
	mLock = IOLockAlloc();
	if (!mLock) return false;

	bzero(mConnections, sizeof(mConnections));
	bzero(mATIPinNids, sizeof(mATIPinNids));

	FBLOG("init: HDA PCI device=%p", mHDAPciDevice);
	return true;
}

void VoodooHDAFramebufferNotifier::free()
{
	stopMatching();
	unmapGPUMMIO();
	if (mLock) {
		IOLockFree(mLock);
		mLock = NULL;
	}
	super::free();
}

bool VoodooHDAFramebufferNotifier::detectedAMDGPUFamily(VoodooHDAAMDGPUFamily *outFamily, UInt16 *outDeviceId)
{
  if (!mLock)
    return false;
  IOLockLock(mLock);
  VoodooHDAAMDGPUFamily family = mAMDGPUFamily;  // not defined
  UInt16 deviceId = mAMDGPUDeviceId;
  IOLockUnlock(mLock);
  if (family == kVoodooHDAAMDGPUUnknown)
    return false;
  if (deviceId == 0 && family != kVoodooHDAAMDGPUClassicPolaris)
    return false;
  if (outFamily)
    *outFamily = family;
  if (outDeviceId)
    *outDeviceId = deviceId;
  return true;
}



const char *VoodooHDAFramebufferNotifier::detectedAMDGPUFamilyName()
{
  VoodooHDAAMDGPUFamily family = kVoodooHDAAMDGPUUnknown;
  if (!detectedAMDGPUFamily(&family, NULL))
    return "Unknown AMD HDMI";
  return amdHdmiGpuFamilyNameForFamily(family);
}

/* ---------- pin registration ---------- */

void VoodooHDAFramebufferNotifier::registerATIPins(int cad, nid_t *pinNids, int count)
{
	IOLockLock(mLock);
	mATIPinCad = cad;
	mATIPinCount = (count > VHDA_FB_MAX_PINS) ? VHDA_FB_MAX_PINS : count;
	for (int i = 0; i < mATIPinCount; i++)
		mATIPinNids[i] = pinNids[i];
	FBLOG("registerATIPins: cad=%d count=%d nids=[%d,%d,%d,%d,%d,%d]",
	      cad, mATIPinCount,
	      mATIPinCount > 0 ? mATIPinNids[0] : -1,
	      mATIPinCount > 1 ? mATIPinNids[1] : -1,
	      mATIPinCount > 2 ? mATIPinNids[2] : -1,
	      mATIPinCount > 3 ? mATIPinNids[3] : -1,
	      mATIPinCount > 4 ? mATIPinNids[4] : -1,
	      mATIPinCount > 5 ? mATIPinNids[5] : -1);
	IOLockUnlock(mLock);
}

/* ---------- IOService matching ---------- */

void VoodooHDAFramebufferNotifier::startMatching()
{
	/*
	 * Match IOFramebuffer services from AMD GPUs.
	 * We use string-based serviceMatching("IOFramebuffer") which does NOT
	 * require linking against IOGraphicsFamily — only IOKit core.
	 */
	OSDictionary *matchDict = IOService::serviceMatching("IOFramebuffer");
	if (!matchDict) {
		FBLOG("startMatching: failed to create matching dict");
		return;
	}

	FBLOG("startMatching: registering for IOFramebuffer notifications");

	mGFXMatchNotifier = IOService::addMatchingNotification(
		gIOMatchedNotification, matchDict,
		&VoodooHDAFramebufferNotifier::gfxMatchedHandler,
		this, NULL, 0);

	matchDict = IOService::serviceMatching("IOFramebuffer");
	if (matchDict) {
		mGFXTermNotifier = IOService::addMatchingNotification(
			gIOTerminatedNotification, matchDict,
			&VoodooHDAFramebufferNotifier::gfxTerminatedHandler,
			this, NULL, 0);
	}

	/*
	 * IODisplay is created AFTER IOFramebuffer and carries the IODisplayEDID
	 * property.  Listen for it separately so we can read EDID when it appears.
	 */
	matchDict = IOService::serviceMatching("IODisplay");
	if (matchDict) {
		mDisplayMatchNotifier = IOService::addMatchingNotification(
			gIOMatchedNotification, matchDict,
			&VoodooHDAFramebufferNotifier::displayMatchedHandler,
			this, NULL, 0);
	}
}

void VoodooHDAFramebufferNotifier::stopMatching()
{
	IOLockLock(mLock);
	for (int i = 0; i < mNumConnections; i++) {
		FBConnectionState *conn = &mConnections[i];
		if (conn->fbNotifier) {
			conn->fbNotifier->remove();
			conn->fbNotifier = NULL;
		}
		if (conn->edidData) {
			IOFree(conn->edidData, conn->edidLen);
			conn->edidData = NULL;
		}
		if (conn->eld) {
			IOFree(conn->eld, conn->eldLen);
			conn->eld = NULL;
		}
		conn->framebuffer = NULL;
	}
	mNumConnections = 0;
	IOLockUnlock(mLock);

	if (mGFXMatchNotifier) {
		mGFXMatchNotifier->remove();
		mGFXMatchNotifier = NULL;
	}
	if (mGFXTermNotifier) {
		mGFXTermNotifier->remove();
		mGFXTermNotifier = NULL;
	}
	if (mDisplayMatchNotifier) {
		mDisplayMatchNotifier->remove();
		mDisplayMatchNotifier = NULL;
	}
	FBLOG("stopMatching: all notifiers removed");
}

/* ---------- PCI topology matching ---------- */

bool VoodooHDAFramebufferNotifier::isSameGPU(IOService *fbService)
{
	if (!mHDAPciDevice || !fbService) return false;

	/* Walk up the provider chain from the framebuffer to find its IOPCIDevice */
	IOService *provider = fbService;
	IOPCIDevice *fbPCI = NULL;
	for (int depth = 0; depth < 10 && provider; depth++) {
		fbPCI = OSDynamicCast(IOPCIDevice, provider);
		if (fbPCI) break;
		provider = provider->getProvider();
	}
	if (!fbPCI) return false;

	/* Check vendor is AMD/ATI */
	UInt16 fbVendor = fbPCI->configRead16(kIOPCIConfigVendorID);
	if (fbVendor != 0x1002) return false;

	/*
	 * GPU display is PCI function 0, HDA audio is function 1.
	 * They share the same parent PCI bridge.
	 */
	IOService *hdaParent = mHDAPciDevice->getProvider();
	IOService *fbParent = fbPCI->getProvider();

	bool same = (hdaParent && fbParent && hdaParent == fbParent);
	FBLOG("isSameGPU: fbPCI=%p vendor=0x%04x hdaParent=%p fbParent=%p -> %s",
	      fbPCI, fbVendor, hdaParent, fbParent, same ? "YES" : "NO");
	return same;
}

/* ---------- matching callbacks ---------- */

bool VoodooHDAFramebufferNotifier::gfxMatchedHandler(
	void *target, __unused void *refCon, IOService *newService, __unused IONotifier *notifier)
{
	VoodooHDAFramebufferNotifier *self = (VoodooHDAFramebufferNotifier *)target;
	if (!newService) return true;

	FBLOG("gfxMatchedHandler: service=%p class=%s",
	      newService, newService->getMetaClass()->getClassName());

	if (!self->isSameGPU(newService)) return true;

	self->handleFramebufferMatched(newService);
	return true;
}

bool VoodooHDAFramebufferNotifier::gfxTerminatedHandler(
	void *target, __unused void *refCon, IOService *service, __unused IONotifier *notifier)
{
	VoodooHDAFramebufferNotifier *self = (VoodooHDAFramebufferNotifier *)target;
	if (service) self->handleFramebufferTerminated(service);
	return true;
}

bool VoodooHDAFramebufferNotifier::displayMatchedHandler(
	void *target, __unused void *refCon, IOService *newService, __unused IONotifier *notifier)
{
	VoodooHDAFramebufferNotifier *self = (VoodooHDAFramebufferNotifier *)target;
	if (!newService) return true;

	OSData *edidProp = OSDynamicCast(OSData, newService->getProperty(kIODisplayEDIDKey));
	if (!edidProp || edidProp->getLength() < 128) return true;

	/*
	 * Walk up: IODisplay -> IODisplayConnect -> IOFramebuffer
	 * Match the IOFramebuffer to one of our registered connections.
	 */
	IOService *parent = newService->getProvider();           /* IODisplayConnect */
	IOService *fb = parent ? parent->getProvider() : NULL;   /* IOFramebuffer   */
	if (!fb) return true;

	IOLockLock(self->mLock);
	FBConnectionState *conn = self->findConnection(fb);
	if (conn && !conn->edidValid) {
		FBLOG("displayMatched: IODisplay for pin=%d, reading EDID", conn->mappedPinNid);

		if (conn->edidData) {
			IOFree(conn->edidData, conn->edidLen);
			conn->edidData = NULL;
		}
		conn->edidLen = edidProp->getLength();
		conn->edidData = (uint8_t *)IOMalloc(conn->edidLen);
		if (conn->edidData) {
			memcpy(conn->edidData, edidProp->getBytesNoCopy(), conn->edidLen);
			FBLOG("displayMatched: pin=%d got %d bytes EDID", conn->mappedPinNid, conn->edidLen);

			if (self->parseEDIDAudio(conn)) {
				self->buildELDFromEDID(conn);
				conn->edidValid = true;
				conn->displayOnline = true;
				self->enableAudioPipe(conn);

				/*
				 * The linear connIndex→pin mapping may be wrong — framebuffer
				 * connIndex=0 may correspond to HDA pin=11, not pin=3.
				 * Inject ELD into the mapped pin AND all pins with presence,
				 * so the correct pin always gets EDID-based audio capabilities.
				 */
				self->injectELDIntoWidget(conn);
				self->injectELDIntoAllPinsWithPresence(conn);

				FBLOG("displayMatched: pin=%d spkalloc=0x%02x nsads=%d pipe enabled",
				      conn->mappedPinNid, conn->speakerAllocation, conn->numSADs);

				/* Try to enable GPU audio engine via direct MMIO */
				self->initGPUAudioIfNeeded();
			}
		}
	}
	IOLockUnlock(self->mLock);
	return true;
}

/* ---------- framebuffer attach/detach ---------- */

void VoodooHDAFramebufferNotifier::handleFramebufferMatched(IOService *fb)
{
	IOLockLock(mLock);

	if (mNumConnections >= VHDA_FB_MAX_CONNECTIONS) {
		FBLOG("handleFBMatched: max connections reached");
		IOLockUnlock(mLock);
		return;
	}

	for (int i = 0; i < mNumConnections; i++) {
		if (mConnections[i].framebuffer == fb) {
			IOLockUnlock(mLock);
			return;
		}
	}

	int idx = mNumConnections++;
	FBConnectionState *conn = &mConnections[idx];
	bzero(conn, sizeof(*conn));
	conn->framebuffer = fb;
	conn->mappedPinNid = -1;
	conn->mappedCodecCad = -1;

	mapConnectionToPin(conn, idx);

	/*
	 * We do NOT call addFramebufferNotification() here because it requires
	 * linking against IOGraphicsFamily, which may not be available on all
	 * macOS versions. Instead, we read EDID on-demand at stream start.
	 *
	 * IOInterestNotification (general interest) is used for lifecycle events.
	 */
	conn->fbNotifier = fb->registerInterest(gIOGeneralInterest,
		&VoodooHDAFramebufferNotifier::interestHandler, this, conn);

	FBLOG("handleFBMatched: fb=%p slot=%d notifier=%p pin=%d",
	      fb, idx, conn->fbNotifier, conn->mappedPinNid);

	/*
	 * Try enabling audio pipe on every framebuffer.  Using
	 * setAttributeForConnectionExt (with IOFramebuffer lock) is safe.
	 * Most will return kIOReturnUnsupported before IODisplay exists,
	 * but the one with the physical monitor may accept it.
	 */
	enableAudioPipe(conn);

	/* Try reading EDID immediately (usually too early — displayMatchedHandler will retry) */
	if (readEDID(conn) && parseEDIDAudio(conn)) {
		buildELDFromEDID(conn);
		conn->edidValid = true;
		conn->displayOnline = true;
		FBLOG("handleFBMatched: EDID valid, spkalloc=0x%02x nsads=%d",
		      conn->speakerAllocation, conn->numSADs);
		injectELDIntoWidget(conn);
    
    // Принудительно включаем аудио-движок GPU через MMIO,
    // так как setAttributeForConnectionExt вернул ошибку.
    initGPUAudioIfNeeded();
	}

	IOLockUnlock(mLock);
}

void VoodooHDAFramebufferNotifier::handleFramebufferTerminated(IOService *fb)
{
	IOLockLock(mLock);
	for (int i = 0; i < mNumConnections; i++) {
		FBConnectionState *conn = &mConnections[i];
		if (conn->framebuffer != fb) continue;

		FBLOG("handleFBTerminated: fb=%p slot=%d", fb, i);
		if (conn->fbNotifier) {
			conn->fbNotifier->remove();
			conn->fbNotifier = NULL;
		}
		clearWidgetELD(conn);
		if (conn->edidData) { IOFree(conn->edidData, conn->edidLen); conn->edidData = NULL; }
		if (conn->eld) { IOFree(conn->eld, conn->eldLen); conn->eld = NULL; }

		for (int j = i; j < mNumConnections - 1; j++)
			mConnections[j] = mConnections[j + 1];
		mNumConnections--;
		break;
	}
	IOLockUnlock(mLock);
}

/* ---------- IOService interest handler (replaces IOFramebuffer notification) ---------- */

IOReturn VoodooHDAFramebufferNotifier::interestHandler(
	void *target, void *refCon, UInt32 messageType, IOService *provider,
  __unused void *messageArgument, __unused vm_size_t argSize)
{
	VoodooHDAFramebufferNotifier *self = (VoodooHDAFramebufferNotifier *)target;
	FBConnectionState *conn = (FBConnectionState *)refCon;
	if (!self || !conn) return kIOReturnSuccess;

	/*
	 * kIOMessageServicePropertyChange fires when IOFramebuffer properties change
	 * (including IODisplayEDID). This is our trigger to re-read EDID.
	 */
	if (messageType == kIOMessageServicePropertyChange) {
		FBLOG("interestHandler: PropertyChange fb=%p pin=%d", provider, conn->mappedPinNid);
		IOLockLock(self->mLock);
		if (self->readEDID(conn) && self->parseEDIDAudio(conn)) {
			self->buildELDFromEDID(conn);
			conn->edidValid = true;
			conn->displayOnline = true;
			self->enableAudioPipe(conn);
			self->injectELDIntoWidget(conn);
			FBLOG("interestHandler: EDID updated, spkalloc=0x%02x nsads=%d",
			      conn->speakerAllocation, conn->numSADs);
		}
		IOLockUnlock(self->mLock);
	} else if (messageType == kIOMessageServiceIsTerminated) {
		FBLOG("interestHandler: service terminated fb=%p pin=%d", provider, conn->mappedPinNid);
	}

	return kIOReturnSuccess;
}

/* ---------- pin mapping ---------- */

void VoodooHDAFramebufferNotifier::mapConnectionToPin(FBConnectionState *conn, int connIndex)
{
  if (!conn || mATIPinCount <= 0)
    return;
  
  /*
   * AMD framebuffer exposes an audio-codec-info property on each connector.
   * On Polaris/RX4xx/RX5xx this is much more reliable than assuming that
   * framebuffer index N maps to HDA pin N, especially when every ATI pin
   * reports stale presence/ELD.  Example seen on RX580 2048SP:
   *   framebuffer@1 with IODisplayEDID has audio-codec-info <00 01 09 00>
   *   so the physical HDMI audio pin is nid 9, not the first pin nid 3.
   */
  
  if (conn->framebuffer) {
    nid_t codecPin = -1;
    if (vhdaReadAMDAudioCodecInfoPin(conn->framebuffer, &codecPin)) {
      for (int i = 0; i < mATIPinCount; i++) {
        if (mATIPinNids[i] == codecPin) {
          conn->mappedPinNid = codecPin;
          conn->mappedCodecCad = mATIPinCad;
          FBLOG("mapConnectionToPin: connIndex=%d audio-codec-info pin nid=%d cad=%d",
                connIndex, conn->mappedPinNid, conn->mappedCodecCad);
          return;
        }
      }
      FBLOG("mapConnectionToPin: connIndex=%d audio-codec-info pin nid=%d not in ATI pin table",
            connIndex, codecPin);
    }
  }
  /*
   * Legacy Polaris without the native framebuffer driver may expose only
   * IONDRV/boot framebuffers and no audio-codec-info.  In that mode every HDA
   * HDMI pin can report stale presence, so first-pin fallback selects pin 3
   * even when the active display path is wired to the high pin.  RX580 2048SP
   * debug traces with the monitor on ATY,Radeon@0 showed working stream setup
   * on pin 13; reverse the classic 3,5,7,9,11,13 table only for this IONDRV
   * no-codec-info path.  Prefer the real ATY,Radeon@N index from the provider
   * chain over IOKit match order so the connected framebuffer drives the HDAU
   * pin selection.  Native AMDFramebuffer/audio-codec-info remains the
   * authoritative mapping above.
   */
  if (conn->framebuffer && vhdaIsIONDRVFramebuffer(conn->framebuffer) &&
      vhdaIsLegacyPolarisHDADevice(mHDAPciDevice) &&
      mATIPinCount >= 6 &&
      mATIPinNids[0] == 3 && mATIPinNids[1] == 5 &&
      mATIPinNids[2] == 7 && mATIPinNids[3] == 9 &&
      mATIPinNids[4] == 11 && mATIPinNids[5] == 13) {
    int fbIndex = -1;
    if (!vhdaGetFramebufferConnectorIndex(conn->framebuffer, &fbIndex))
      fbIndex = connIndex;
    int reverseIndex = (mATIPinCount - 1) - fbIndex;
    if (reverseIndex >= 0 && reverseIndex < mATIPinCount) {
      conn->mappedPinNid = mATIPinNids[reverseIndex];
      conn->mappedCodecCad = mATIPinCad;
      FBLOG("mapConnectionToPin: connIndex=%d fbIndex=%d IONDRV reverse fallback -> pin nid=%d cad=%d",
            connIndex, fbIndex, conn->mappedPinNid, conn->mappedCodecCad);
      return;
    }
  }
  
  if (connIndex < mATIPinCount) {
    conn->mappedPinNid = mATIPinNids[connIndex];
    conn->mappedCodecCad = mATIPinCad;
    FBLOG("mapConnectionToPin: connIndex=%d fallback -> pin nid=%d cad=%d",
          connIndex, conn->mappedPinNid, conn->mappedCodecCad);
  }
}

/* ---------- EDID reading ---------- */
//const uint8_t sampleEDID[256] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x4c, 0x2d, 0x59, 0x06, 0x00, 0x00, 0x00, 0x00, 0x2d, 0x13, 0x01, 0x03, 0x80, 0x10, 0x09, 0x78, 0x0a, 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54, 0xbd, 0xef, 0x80, 0x71, 0x4f, 0x81, 0x00, 0x81, 0x40, 0x81, 0x80, 0x95, 0x00, 0x95, 0x0f, 0xb3, 0x00, 0xa9, 0x40, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c, 0x45, 0x00, 0xa0, 0x5a, 0x00, 0x00, 0x00, 0x1e, 0x66, 0x21, 0x50, 0xb0, 0x51, 0x00, 0x1b, 0x30, 0x40, 0x70, 0x36, 0x00, 0xa0, 0x5a, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x18, 0x4b, 0x1a, 0x51, 0x17, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x53, 0x41, 0x4d, 0x53, 0x55, 0x4e, 0x47, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0x3a, 0x02, 0x03, 0x23, 0xf1, 0x4b, 0x90, 0x1f, 0x04, 0x13, 0x05, 0x14, 0x03, 0x12, 0x20, 0x21, 0x22, 0x23, 0x09, 0x07, 0x07, 0x83, 0x01, 0x00, 0x00, 0xe2, 0x00, 0x0f, 0x67, 0x03, 0x0c, 0x00, 0x20, 0x00, 0xb8, 0x2d, 0x01, 0x1d, 0x00, 0x72, 0x51, 0xd0, 0x1e, 0x20, 0x6e, 0x28, 0x55, 0x00, 0xa0, 0x5a, 0x00, 0x00, 0x00, 0x1e, 0x01, 0x1d, 0x00, 0xbc, 0x52, 0xd0, 0x1e, 0x20, 0xb8, 0x28, 0x55, 0x40, 0xa0, 0x5a, 0x00, 0x00, 0x00, 0x1e, 0x01, 0x1d, 0x80, 0x18, 0x71, 0x1c, 0x16, 0x20, 0x58, 0x2c, 0x25, 0x00, 0xa0, 0x5a, 0x00, 0x00, 0x00, 0x9e, 0x01, 0x1d, 0x80, 0xd0, 0x72, 0x1c, 0x16, 0x20, 0x10, 0x2c, 0x25, 0x80, 0xa0, 0x5a, 0x00, 0x00, 0x00, 0x9e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfd};

/* -------------------------------------------------------------------------- */
/* 1. Абсолютно безопасное чтение EDID (без вложенных циклов, без зависаний)  */
/* -------------------------------------------------------------------------- */
bool VoodooHDAFramebufferNotifier::readEDID(FBConnectionState *conn)
{
  if (!conn || !conn->framebuffer) return false;
  
  // Очищаем старый EDID, если он был
  if (conn->edidData) {
    IOFree(conn->edidData, conn->edidLen);
    conn->edidData = NULL;
    conn->edidLen = 0;
  }
  
  OSData *edidProp = NULL;
  
  // Проверяем ТОЛЬКО сам фреймбуфер. Никаких getChildIterator!
  // Это на 100% безопасно и никогда не вызовет черный экран.
  edidProp = OSDynamicCast(OSData, conn->framebuffer->getProperty(kIODisplayEDIDKey));
  
  // Если нашли и размер корректный (минимум 128 байт)
  if (edidProp && edidProp->getLength() >= 128 && edidProp->getLength() <= 512) {
    conn->edidLen = edidProp->getLength();
    conn->edidData = (uint8_t *)IOMalloc(conn->edidLen);
    if (conn->edidData) {
      memcpy(conn->edidData, edidProp->getBytesNoCopy(), conn->edidLen);
      FBLOG("readEDID: SAFE SUCCESS! pin=%d got %d bytes", conn->mappedPinNid, conn->edidLen);
      return true;
    }
  }
  
  // Если не нашли, просто возвращаем false. Это нормально.
  // Мы не используем sampleEDID, чтобы не путать DVI и HDMI порты.
  FBLOG("readEDID: No EDID found for pin=%d. Skipping safely.", conn->mappedPinNid);
  return false;
}

/* -------------------------------------------------------------------------- */
/* 2. "Мягкая" инициализация аудио (Gentle Audio Init)                        */
/*    Мы НЕ трогаем видеодвижок (DIG_MODE, DIG_BE_EN), чтобы не вызвать       */
/*    черный экран. Мы включаем ТОЛЬКО аудио-пакеты (AFMT, DP_SEC) на том     */
/*    порту, который УЖЕ активен и УЖЕ работает в режиме HDMI.                */
/* -------------------------------------------------------------------------- */
void VoodooHDAFramebufferNotifier::initGPUAudioIfNeeded()
{

  if (mGPUAudioInitDone) return;
  if (!mapGPUMMIO()) {
    FBLOG("initGPUAudio: cannot map GPU MMIO");
    return;
  }
  mGPUAudioInitDone = true;

  FBLOG("initGPUAudio: === GPU AZ STATE (Final) ===");
  dumpAZState();
}

/* ---------- EDID CEA audio parsing ---------- */

bool VoodooHDAFramebufferNotifier::parseEDIDAudio(FBConnectionState *conn)
{
  if (!conn->edidData || conn->edidLen < 128) {
    FBLOG("parseEDIDAudio: pin=%d invalid EDID data (len=%d)", conn->mappedPinNid, conn->edidLen);
    return false;
  }
  
  conn->speakerAllocation = 0;
  conn->numSADs = 0;
  bzero(conn->sads, sizeof(conn->sads));
  
  int numExtensions = conn->edidData[126];
  
  if (numExtensions == 0 || conn->edidLen < 256) {
    FBLOG("parseEDIDAudio: pin=%d no CEA extension, using default", conn->mappedPinNid);
    conn->speakerAllocation = 0x01;
    conn->sads[0] = 0x09; /* LPCM, 2ch */
    conn->sads[1] = 0x07; /* 32/44.1/48 kHz */
    conn->sads[2] = 0x05; /* 16/24-bit */
    conn->numSADs = 1;
    return true;
  }
  
  uint8_t *cea = &conn->edidData[128];
  if (cea[0] != 0x02) {
    FBLOG("parseEDIDAudio: pin=%d CEA tag=0x%02x (expected 0x02)", conn->mappedPinNid, cea[0]);
    return false;
  }
  
  int dtdOffset = cea[2];
  bool basicAudio = (cea[3] & 0x40) != 0;
  FBLOG("parseEDIDAudio: pin=%d CEA rev=%d dtdOffset=%d basicAudio=%d",
        conn->mappedPinNid, cea[1], dtdOffset, basicAudio);
  
  if (!basicAudio) {
    FBLOG("parseEDIDAudio: pin=%d no basic audio", conn->mappedPinNid);
    return false;
  }
  
  int pos = 4;
  while (pos < dtdOffset && pos < 127) {
    int tag = (cea[pos] >> 5) & 0x07;
    int blockLen = cea[pos] & 0x1f;
    pos++;
    if (pos + blockLen > dtdOffset) break;
    
    if (tag == 1) {  // Audio Data Block
      int nSADs = blockLen / 3;
      FBLOG("parseEDIDAudio: pin=%d Audio Data Block: %d SADs", conn->mappedPinNid, nSADs);
      for (int i = 0; i < nSADs && conn->numSADs < VHDA_FB_MAX_SADS; i++) {
        int off = conn->numSADs * 3;
        conn->sads[off + 0] = cea[pos + i * 3 + 0];
        conn->sads[off + 1] = cea[pos + i * 3 + 1];
        conn->sads[off + 2] = cea[pos + i * 3 + 2];
        int fmt = (conn->sads[off + 0] >> 3) & 0x0f;
        int nch = (conn->sads[off + 0] & 0x07) + 1;
        int rates = conn->sads[off + 1];
        int bits = conn->sads[off + 2];
        FBLOG("parseEDIDAudio: pin=%d SAD[%d]: fmt=%d ch=%d rates=0x%02x bits=0x%02x",
              conn->mappedPinNid, conn->numSADs, fmt, nch, rates, bits);
        conn->numSADs++;
      }
    } else if (tag == 3) {  // Speaker Allocation Data Block
      conn->speakerAllocation = cea[pos];
      FBLOG("parseEDIDAudio: pin=%d Speaker Allocation: 0x%02x", conn->mappedPinNid, conn->speakerAllocation);
    }
    pos += blockLen;
  }
  
  // Workaround для старых ТВ с некорректным spkalloc
  // Если заявлен spkalloc=0x03 (2.1), но SADs содержат только стерео
  if (conn->speakerAllocation == 0x03 && conn->numSADs > 0) {
    bool allStereo = true;
    for (int i = 0; i < conn->numSADs; i++) {
      int fmt = (conn->sads[i*3] >> 3) & 0x0f;
      int nch = (conn->sads[i*3] & 0x07) + 1;
      // Если есть LPCM с >2 каналами, значит ТВ действительно поддерживает многоканал
      if (fmt == 1 && nch > 2) {
        allStereo = false;
        break;
      }
    }
    if (allStereo) {
      FBLOG("parseEDIDAudio: pin=%d fixing spkalloc 0x03->0x01 (TV claims 2.1 but only stereo SADs)",
            conn->mappedPinNid);
      conn->speakerAllocation = 0x01;
    }
  }
  
  // Если не нашли speaker allocation, но есть SAD, используем значение по умолчанию
  if (conn->speakerAllocation == 0 && conn->numSADs > 0) {
    conn->speakerAllocation = 0x01;  // FL/FR only
    FBLOG("parseEDIDAudio: pin=%d using default spkalloc=0x01", conn->mappedPinNid);
  }
  
  FBLOG("parseEDIDAudio: pin=%d result: spkalloc=0x%02x nsads=%d",
        conn->mappedPinNid, conn->speakerAllocation, conn->numSADs);
  return (conn->numSADs > 0);
}


/* ---------- ELD construction ---------- */

void VoodooHDAFramebufferNotifier::buildELDFromEDID(FBConnectionState *conn)
{
  if (!conn->edidData || conn->edidLen < 128) return;
  
  int mnl = 0; // Monitor name length (пока не используем)
  
  // ПРАВИЛЬНЫЙ расчёт Baseline ELD по HDA Spec 7.3.2:
  // Byte 4: Speaker Allocation (1 byte)
  // Bytes 5-12: Port ID (8 bytes)
  // Byte 13: Audio Sync Delay (1 byte)
  // Byte 14: Reserved + HDCP + AI_CP (1 byte)
  // Byte 15: SAD count + CEA_EDID_Version (1 byte)
  // Bytes 16+: SADs (3 bytes × numSADs)
  // После SADs: Monitor Name (MNL bytes)
  // Итого Baseline = 1 + 8 + 1 + 1 + 1 + 3*numSADs + MNL = 12 + 3*numSADs + MNL
  
  int baselineLen = 12 + 3 * conn->numSADs + mnl;
  
  // Округляем ВВЕРХ до кратного 4 (до целого числа DWORD)
  int baselineLenAligned = ((baselineLen + 3) / 4) * 4;
  int totalLen = 4 + baselineLenAligned;
  
  // Освобождаем старый ELD
  if (conn->eld) {
    IOFree(conn->eld, conn->eldLen);
    conn->eld = NULL;
    conn->eldLen = 0;
  }
  
  conn->eld = (uint8_t *)IOMalloc(totalLen);
  if (!conn->eld) return;
  
  conn->eldLen = totalLen;
  bzero(conn->eld, totalLen); // Заполняем нулями (включая padding и Port ID)
  
  // === HEADER (bytes 0-3) ===
  conn->eld[0] = 0x02 << 3;  // ELD version 2 (bits 7:3 = 0x10)
  conn->eld[1] = 0x00;       // Reserved
  conn->eld[2] = baselineLenAligned / 4;  // Baseline ELD length in DWORDs
  conn->eld[3] = (0x03 << 5) | (mnl & 0x1F);  // CEA_EDID_Version=3 + MNL
  
  // === BASELINE ELD (bytes 4+) ===
  conn->eld[4] = conn->speakerAllocation;  // Speaker Allocation (ПРАВИЛЬНОЕ МЕСТО!)
                                           // Bytes 5-12: Port ID — остаются нулями (bzero)
  conn->eld[13] = 0x00;  // Audio Sync Delay
  conn->eld[14] = 0x00;  // Reserved + HDCP + AI_CP
  conn->eld[15] = (conn->numSADs << 4) | 0x00;  // SAD count (bits 7:4) + CEA_EDID_Version
  
  // === SADs (bytes 16+) ===
  for (int i = 0; i < conn->numSADs * 3; i++) {
    conn->eld[16 + i] = conn->sads[i];
  }
  
  FBLOG("buildELDFromEDID: pin=%d baselineLen=%d aligned=%d totalLen=%d eld[2]=%d spkalloc=0x%02x nsads=%d",
        conn->mappedPinNid, baselineLen, baselineLenAligned, totalLen,
        conn->eld[2], conn->eld[4], conn->numSADs);
}

/* ---------- audio pipe control ---------- */

bool VoodooHDAFramebufferNotifier::enableAudioPipe(FBConnectionState *conn)
{
	if (!conn->framebuffer) return false;

	/*
	 * Activate the GPU audio pipe via IOFramebuffer::setAttributeForConnection().
	 * This is a virtual method — the call dispatches through the vtable at runtime
	 * and does NOT require linking against IOGraphicsFamily.  The header is only
	 * needed for the vtable layout (compile-time constant).
	 *
	 * Safety: we verified the object is an AMDFramebuffer (IOFramebuffer subclass)
	 * in isSameGPU() before creating the connection.
	 */
	IOFramebuffer *fb = reinterpret_cast<IOFramebuffer *>(conn->framebuffer);

	/*
	 * AppleGFXHDA uses setAttributeForConnectionExt (non-virtual, acquires
	 * IOFramebuffer lock) — NOT setAttributeForConnection (virtual, no lock).
	 * Calling the virtual version bypasses the lock and corrupts display state.
	 * AppleGFXHDA only uses kConnectionEnableAudio, never kConnectionAudioStreaming.
	 */
	IOReturn ret = fb->setAttributeForConnectionExt(0, kConnectionEnableAudio, 1);
	FBLOG("enableAudioPipe: pin=%d setAttributeForConnectionExt(kConnectionEnableAudio)=%x", conn->mappedPinNid, ret);

	conn->audioPipeEnabled = (ret == kIOReturnSuccess);

	/* Update engine name to show which port has audio enabled */
	if (conn->audioPipeEnabled && mDevice && conn->mappedPinNid >= 0) {
		for (int i = 0; i < mDevice->mNumHDMIEngines; i++) {
			VoodooHDADevice::HDMIEngineSlot *slot = &mDevice->mHDMIEngines[i];
			if (slot->engine && slot->pinNid == conn->mappedPinNid) {
				char desc[80];
				snprintf(desc, sizeof(desc), "%s: HDMI %d (audio enabled)",
				         mDevice->mControllerName ? mDevice->mControllerName : "GPU",
				         slot->pinNid);
				slot->engine->setProperty("IOAudioEngineDescription", desc);
				FBLOG("enableAudioPipe: updated engine name for pin=%d", conn->mappedPinNid);
			}
		}
	}

	return conn->audioPipeEnabled;
}

void VoodooHDAFramebufferNotifier::retryEnableAudioPipeAll()
{
	IOLockLock(mLock);
	for (int i = 0; i < mNumConnections; i++) {
		FBConnectionState *conn = &mConnections[i];
		if (conn->audioPipeEnabled || !conn->framebuffer) continue;
		enableAudioPipe(conn);
	}
	IOLockUnlock(mLock);
}

void VoodooHDAFramebufferNotifier::disableAudioPipe(FBConnectionState *conn)
{
	conn->audioPipeEnabled = false;
}

/* ---------- ELD injection into VoodooHDA widget ---------- */

void VoodooHDAFramebufferNotifier::injectELDIntoWidget(FBConnectionState *conn)
{
	if (!mDevice || conn->mappedPinNid < 0 || !conn->eld || conn->eldLen == 0)
		return;

	Codec *codec = mDevice->mCodecs[conn->mappedCodecCad];
	if (!codec) return;

	for (int fg = 0; fg < codec->numFuncGroups; fg++) {
		FunctionGroup *funcGroup = &codec->funcGroups[fg];
		if (funcGroup->nodeType != HDA_PARAM_FCT_GRP_TYPE_NODE_TYPE_AUDIO)
			continue;
		Widget *w = mDevice->widgetGet(funcGroup, conn->mappedPinNid);
		if (!w) continue;

		if (w->eld) {
			VoodooHDADevice::freeMem(w->eld);
			w->eld = NULL;
			w->eld_len = 0;
		}

		w->eld = (uint8_t *)VoodooHDADevice::allocMem(conn->eldLen);
		if (w->eld) {
			memcpy(w->eld, conn->eld, conn->eldLen);
			w->eld_len = conn->eldLen;
      // ПРАВИЛЬНОЕ место для spkalloc — это eld[4], а не eld[7]!
      uint8_t spkalloc = (w->eld_len > 4) ? w->eld[4] : 0;
      FBLOG("injectELD: nid=%d eld_len=%d spkalloc=0x%02x (from eld[4])",
            conn->mappedPinNid, w->eld_len, spkalloc);
      
      // Отладочный вывод первых 20 байт ELD для проверки
//      if (w->eld_len > 0) {
//        FBLOG("injectELD: ELD dump (first %d bytes):",
//              (w->eld_len < 20) ? w->eld_len : 20);
//        for (int i = 0; i < (w->eld_len < 20 ? w->eld_len : 20); i++) {
//          FBLOG("  eld[%d] = 0x%02x", i, w->eld[i]);
//        }
//      }
      // КРИТИЧЕСКИ ВАЖНО: Если ELD был отложен, обновляем его сейчас
//      if (w->needELDUpdate) {
//        w->needELDUpdate = false;
//        // Вызываем hdaa_eld_handler для обновления всех связанных настроек
//        //mDevice->hdaa_eld_handler(w);
//        FBLOG("injectELD: triggered hdaa_eld_handler for delayed nid=%d", conn->mappedPinNid);
//      }
		}
		return;
	}
}

void VoodooHDAFramebufferNotifier::injectELDIntoAllPinsWithPresence(FBConnectionState *conn)
{
  if (!mDevice || !conn->eld || conn->eldLen == 0 || mATIPinCad < 0)
    return;
  
  Codec *codec = mDevice->mCodecs[mATIPinCad];
  if (!codec) return;
  
  for (int fg = 0; fg < codec->numFuncGroups; fg++) {
    FunctionGroup *funcGroup = &codec->funcGroups[fg];
    if (funcGroup->nodeType != HDA_PARAM_FCT_GRP_TYPE_NODE_TYPE_AUDIO)
      continue;
    
    for (int i = 0; i < mATIPinCount; i++) {
      nid_t nid = mATIPinNids[i];
      if (nid == conn->mappedPinNid) continue;
      
      UInt32 pinSense = mDevice->sendCommand(
                                             HDA_CMD_GET_PIN_SENSE(mATIPinCad, nid), mATIPinCad);
      if (!(pinSense & (1U << 31))) continue;
      
      Widget *w = mDevice->widgetGet(funcGroup, nid);
      if (!w) continue;
      
      if (w->eld) {
        VoodooHDADevice::freeMem(w->eld);
        w->eld = NULL;
        w->eld_len = 0;
      }
      
      w->eld = (uint8_t *)VoodooHDADevice::allocMem(conn->eldLen);
      if (w->eld) {
        memcpy(w->eld, conn->eld, conn->eldLen);
        w->eld_len = conn->eldLen;
        uint8_t spkalloc = (w->eld_len > 4) ? w->eld[4] : 0;
        FBLOG("injectELD(presence): nid=%d pinSense=0x%08x eld_len=%d spkalloc=0x%02x",
              nid, pinSense, w->eld_len, spkalloc);
      }
    }
    return;
  }
}

void VoodooHDAFramebufferNotifier::injectELDIntoPinIfReady(int cad, nid_t pinNid)
{
	IOLockLock(mLock);

	/* Find any connection with valid EDID-based ELD */
	FBConnectionState *src = NULL;
  for (int i = 0; i < mNumConnections; i++) {
    if (mConnections[i].edidValid && mConnections[i].eld && mConnections[i].eldLen > 0) {
      src = &mConnections[i];
      break;
    }
  }

	if (src && mDevice) {
		Codec *codec = mDevice->mCodecs[cad];
		if (codec) {
			for (int fg = 0; fg < codec->numFuncGroups; fg++) {
				FunctionGroup *funcGroup = &codec->funcGroups[fg];
				if (funcGroup->nodeType != HDA_PARAM_FCT_GRP_TYPE_NODE_TYPE_AUDIO)
					continue;
				Widget *w = mDevice->widgetGet(funcGroup, pinNid);
				if (!w) continue;
     
        if (w->eld) {
          VoodooHDADevice::freeMem(w->eld);
          w->eld = NULL;
          w->eld_len = 0;
        }
        w->eld = (uint8_t *)VoodooHDADevice::allocMem(src->eldLen);
        if (w->eld) {
          memcpy(w->eld, src->eld, src->eldLen);
          w->eld_len = src->eldLen;
          uint8_t spkalloc = (w->eld_len > 4) ? w->eld[4] : 0;
          FBLOG("injectELDIntoPinIfReady: nid=%d eld_len=%d spkalloc=0x%02x (from eld[4])",
                pinNid, w->eld_len, spkalloc);
        }
				break;
			}
		}
  } else {
    FBLOG("injectELDIntoPinIfReady: no valid ELD source for pin=%d", pinNid);
  }

	IOLockUnlock(mLock);
}

void VoodooHDAFramebufferNotifier::clearWidgetELD(FBConnectionState *conn)
{
	if (!mDevice || conn->mappedPinNid < 0) return;

	Codec *codec = mDevice->mCodecs[conn->mappedCodecCad];
	if (!codec) return;

	for (int fg = 0; fg < codec->numFuncGroups; fg++) {
		FunctionGroup *funcGroup = &codec->funcGroups[fg];
		if (funcGroup->nodeType != HDA_PARAM_FCT_GRP_TYPE_NODE_TYPE_AUDIO) continue;
		Widget *w = mDevice->widgetGet(funcGroup, conn->mappedPinNid);
		if (!w) continue;
		if (w->eld) {
			VoodooHDADevice::freeMem(w->eld);
			w->eld = NULL;
			w->eld_len = 0;
		}
		return;
	}
}

/* ---------- streaming state notification ---------- */

void VoodooHDAFramebufferNotifier::notifyStreamingState(int cad, nid_t pinNid, bool streaming)
{
	/*
	 * AppleGFXHDA does NOT call any framebuffer attribute at stream start/stop.
	 * Audio pipe is enabled once via setAttributeForConnectionExt(kConnectionEnableAudio)
	 * during display init.  The GPU handles InfoFrame encoding automatically.
	 */
	(void)cad;
	(void)pinNid;
	(void)streaming;
}

/* ---------- public query interface ---------- */

bool VoodooHDAFramebufferNotifier::getFramebufferELD(
	int cad, nid_t pinNid, uint8_t **outELD, int *outLen)
{
	IOLockLock(mLock);
	for (int i = 0; i < mNumConnections; i++) {
		FBConnectionState *conn = &mConnections[i];
		if (conn->mappedCodecCad == cad && conn->mappedPinNid == pinNid &&
		    conn->eld && conn->eldLen > 0) {
			*outELD = conn->eld;
			*outLen = conn->eldLen;
			IOLockUnlock(mLock);
      FBLOG("getFramebufferELD: SUCCESS cad=%d pin=%d eld_len=%d spkalloc=0x%02x",
            cad, pinNid, conn->eldLen, conn->eldLen > 7 ? conn->eld[4] : 0);

			return true;
		}
	}
	IOLockUnlock(mLock);
  FBLOG("getFramebufferELD: FAILED cad=%d pin=%d (no matching connection)", cad, pinNid);
	return false;
}

void VoodooHDAFramebufferNotifier::ensureAudioPipeEnabled(int cad, nid_t pinNid)
{
	IOLockLock(mLock);
	for (int i = 0; i < mNumConnections; i++) {
		FBConnectionState *conn = &mConnections[i];
		if (conn->mappedCodecCad == cad && conn->mappedPinNid == pinNid) {
			if (!conn->edidValid && conn->framebuffer) {
				if (readEDID(conn) && parseEDIDAudio(conn)) {
					buildELDFromEDID(conn);
					conn->edidValid = true;
					enableAudioPipe(conn);
					injectELDIntoWidget(conn);
				}
			}
			break;
		}
	}
	IOLockUnlock(mLock);
}

FBConnectionState *
VoodooHDAFramebufferNotifier::findConnectionForPin(int cad, nid_t pinNid)
{
	for (int i = 0; i < mNumConnections; i++) {
		if (mConnections[i].mappedCodecCad == cad &&
		    mConnections[i].mappedPinNid == pinNid)
			return &mConnections[i];
	}
	return NULL;
}

FBConnectionState *
VoodooHDAFramebufferNotifier::findConnection(IOService *fb)
{
	for (int i = 0; i < mNumConnections; i++) {
		if (mConnections[i].framebuffer == fb)
			return &mConnections[i];
	}
	return NULL;
}

/* ---------- GPU MMIO mapping ---------- */

bool VoodooHDAFramebufferNotifier::mapGPUMMIO()
{
	if (mGPUMMIO) return true;  /* already mapped */
	if (!mHDAPciDevice) return false;

	/* GPU is PCI function 0, HDA audio is function 1.
	 * They share the same parent PCI bridge. */
	IOService *parent = mHDAPciDevice->getProvider();
	if (!parent) {
		FBLOG("mapGPUMMIO: no parent bridge");
		return false;
	}

	/* Find function 0 among siblings */
	OSIterator *iter = parent->getChildIterator(gIOServicePlane);
	if (!iter) return false;

	IOPCIDevice *gpuDev = NULL;
	IOService *child;
  UInt16 vendor = 0;
	while ((child = OSDynamicCast(IOService, iter->getNextObject()))) {
		IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, child);
		if (!pci) continue;
		vendor = pci->configRead16(kIOPCIConfigVendorID);
		if (vendor != 0x1002) continue;
		/* Function 0 is the GPU display controller */
		UInt8 funcNum = pci->getFunctionNumber();
		if (funcNum == 0) {
			gpuDev = pci;
			break;
		}
	}
	iter->release();

	if (!gpuDev) {
		FBLOG("mapGPUMMIO: GPU function 0 not found");
		return false;
	}

	UInt16 gpuDeviceId = gpuDev->configRead16(kIOPCIConfigDeviceID);
	FBLOG("mapGPUMMIO: found GPU device=%p vendor=%04x device=%04x",
	      gpuDev, gpuDev->configRead16(kIOPCIConfigVendorID), gpuDeviceId);
  
  VoodooHDAAMDGPUFamily family = classifyAMDGPUDevice(gpuDeviceId);
  mAMDGPUDeviceId = gpuDeviceId;
  mAMDGPUFamily = family;
  
  FBLOG("isSameGPU: vendor=0x%04x device=0x%04x family=%s -> YES",
        vendor, gpuDeviceId, amdHdmiGpuFamilyNameForFamily(family));

	/* Select register offset table based on GPU generation.
	 * Polaris (DCE 11.x): 67xx device IDs (Ellesmere/Baffin/Lexa)
	 * Vega 10 (DCE 12.0): 687x/686x device IDs */
	if ((gpuDeviceId & 0xFF00) == 0x6700 || /* Polaris 10/11/12 */
	    (gpuDeviceId & 0xFF00) == 0x6600 || /* Polaris 12/Lexa */
	    (gpuDeviceId & 0xFF00) == 0x6900)   /* Tonga/Fiji */
		mRegs = &kPolarisRegs;
	else
		mRegs = &kVega10Regs;  /* Vega and newer — best guess */

	FBLOG("mapGPUMMIO: using %s register offsets",
	      mRegs == &kPolarisRegs ? "Polaris DCE11" : "Vega DCE12");

	/* Find the MMIO BAR (typically BAR5, ~256KB-512KB).
	 * BAR0/1 = VRAM (huge), BAR2/3 = doorbell, BAR5 = MMIO registers */
	static const UInt8 barRegs[] = {
		kIOPCIConfigBaseAddress5,
		kIOPCIConfigBaseAddress4,
		kIOPCIConfigBaseAddress2,
	};

	for (int b = 0; b < 3; b++) {
		IODeviceMemory *mem = gpuDev->getDeviceMemoryWithRegister(barRegs[b]);
		if (!mem) continue;
		uint64_t len = mem->getLength();
		/* MMIO BAR is typically 256KB-512KB; skip huge VRAM BARs */
		if (len >= 0x40000 && len <= 0x100000) {
			mGPUMMIOMap = mem->createMappingInTask(kernel_task, 0,
				kIOMapAnywhere | kIOMapInhibitCache);
			if (mGPUMMIOMap) {
				mGPUMMIO = (volatile uint32_t *)mGPUMMIOMap->getVirtualAddress();
				mGPUMMIOSize = (uint32_t)len;
				mGPUPciDevice = gpuDev;
				FBLOG("mapGPUMMIO: mapped BAR%d size=0x%x vaddr=%p",
				      (barRegs[b] - kIOPCIConfigBaseAddress0) / 4,
				      mGPUMMIOSize, mGPUMMIO);
				return true;
			}
		}
		FBLOG("mapGPUMMIO: BAR%d size=0x%llx skipped",
		      (barRegs[b] - kIOPCIConfigBaseAddress0) / 4, len);
	}

	FBLOG("mapGPUMMIO: no suitable MMIO BAR found");
	return false;
}

void VoodooHDAFramebufferNotifier::unmapGPUMMIO()
{
	if (mGPUMMIOMap) {
		mGPUMMIOMap->release();
		mGPUMMIOMap = NULL;
	}
	mGPUMMIO = NULL;
	mGPUMMIOSize = 0;
	mGPUPciDevice = NULL;
}

uint32_t VoodooHDAFramebufferNotifier::gpuRead32(uint32_t byteOffset)
{
	if (!mGPUMMIO || byteOffset + 4 > mGPUMMIOSize) return 0xFFFFFFFF;
	return mGPUMMIO[byteOffset / 4];
}

void VoodooHDAFramebufferNotifier::gpuWrite32(uint32_t byteOffset, uint32_t value)
{
	if (!mGPUMMIO || byteOffset + 4 > mGPUMMIOSize) return;
	mGPUMMIO[byteOffset / 4] = value;
}

uint32_t VoodooHDAFramebufferNotifier::azEndpointRead(int ep, uint32_t index)
{
	const AZRegOffsets *r = (const AZRegOffsets *)mRegs;
	gpuWrite32(r->azEpIndex0 + ep * r->azEpStride, index);
	return gpuRead32(r->azEpData0 + ep * r->azEpStride);
}

void VoodooHDAFramebufferNotifier::azEndpointWrite(int ep, uint32_t index, uint32_t value)
{
	const AZRegOffsets *r = (const AZRegOffsets *)mRegs;
	gpuWrite32(r->azEpIndex0 + ep * r->azEpStride, index);
	gpuWrite32(r->azEpData0 + ep * r->azEpStride, value);
}

/* ---------- Diagnostic dump of AZ state ---------- */

void VoodooHDAFramebufferNotifier::dumpAZState()
{
	if (!mGPUMMIO) return;

	for (int ep = 0; ep < 7; ep++) {
		/* Read AZ endpoint registers (read-only, no clock gating toggle) */
		uint32_t hpc = azEndpointRead(ep, AZ_REG_PIN_CONTROL_HOT_PLUG_CONTROL);
		uint32_t cs = azEndpointRead(ep, AZ_REG_PIN_CONTROL_CHANNEL_SPEAKER);
		uint32_t desc0 = azEndpointRead(ep, AZ_REG_PIN_CONTROL_AUDIO_DESCRIPTOR(0));
		uint32_t hbr = azEndpointRead(ep, AZ_REG_PIN_CONTROL_RESPONSE_HBR);

		FBLOG("AZ EP%d: HPC=0x%08x CS=0x%08x DESC0=0x%08x HBR=0x%08x audioEnabled=%d",
		      ep, hpc, cs, desc0, hbr, (hpc & AZ_HPC_AUDIO_ENABLED) ? 1 : 0);
	}

	const AZRegOffsets *r = (const AZRegOffsets *)mRegs;

	/* Dump DIG encoder status + AFMT/DP_SEC for DIG0-5 + DIG6 (irregular offset) */
	/* Polaris DIG6=0x5400, DIG7=0x5600, DIG8=0x5700 */
	static const uint32_t digExtraBase[] = { 0x5400, 0x5600, 0x5700 };
	int numDigs = 9; /* DIG0-5 (regular) + DIG6-8 (irregular) */
	for (int d = 0; d < numDigs; d++) {
		uint32_t base;
		if (d < 6)
			base = r->digFeCntl0 + d * r->digStride;
		else
			base = DW2B(digExtraBase[d - 6]); /* DIG6/7/8 */

		/* Compute register offsets relative to DIG base */
		uint32_t feCntlOff = base;
		uint32_t beCntlOff = base + (r->digBeCntl0 - r->digFeCntl0);
		uint32_t beEnOff = base + (r->digBeEnCntl0 - r->digFeCntl0);
		uint32_t dpLinkOff = base + (r->dpLinkCntl0 - r->digFeCntl0);
		uint32_t afmtSrcOff = base + (r->afmtSrcCtl0 - r->digFeCntl0);
		uint32_t afmtCntlOff = base + (r->afmtCntl0 - r->digFeCntl0);
		uint32_t dpSecOff = base + (r->dpSecCntl0 - r->digFeCntl0);
		uint32_t pktCtlOff = base + (r->afmtPktCtl0 - r->digFeCntl0);

		if (afmtCntlOff + 4 > mGPUMMIOSize) continue; /* bounds check */

		uint32_t feCntl = gpuRead32(feCntlOff);
		uint32_t beCntl = gpuRead32(beCntlOff);
		uint32_t beEn = gpuRead32(beEnOff);
		uint32_t dpLink = gpuRead32(dpLinkOff);
		uint32_t afmtSrc = gpuRead32(afmtSrcOff);
		uint32_t afmtCntl = gpuRead32(afmtCntlOff);
		uint32_t dpSec = gpuRead32(dpSecOff);
		uint32_t pktCtl = gpuRead32(pktCtlOff);

		bool digEnabled = (beEn & 0x1) != 0;
		int digMode = (beCntl >> 16) & 0x7;
		bool feStarted = (feCntl & (1 << 10)) != 0;
		bool dpTrained = (dpLink & 0x10) != 0;
		bool dpActive = (dpLink & 0x100) != 0;
		int crtcSrc = feCntl & 0x7;

		FBLOG("AZ DIG%d: BE_EN=%d mode=%d FE_START=%d crtc=%d dpTrained=%d dpActive=%d | AFMT_SRC=%x AFMT_CNTL=%x DP_SEC=%x PKT=%x",
		      d, digEnabled, digMode, feStarted, crtcSrc,
		      dpTrained, dpActive,
		      afmtSrc, afmtCntl, dpSec, pktCtl);
	}

	/* DCCG DTO */
	uint32_t dtoSrc = gpuRead32(r->dccgDtoSource);
	uint32_t dto1Phase = gpuRead32(r->dccgDto1Phase);
	uint32_t dto1Mod = gpuRead32(r->dccgDto1Module);
	FBLOG("AZ DCCG: DTO_SRC=0x%08x DTO1_PHASE=0x%08x DTO1_MOD=0x%08x",
	      dtoSrc, dto1Phase, dto1Mod);
}

/* ---------- Enable GPU audio engine for a DP output ---------- */

bool VoodooHDAFramebufferNotifier::enableGPUAudioEngine(
	int endpoint, int digIndex, bool isDP,
	uint8_t speakerAlloc, int numChannels)
{
	if (!mGPUMMIO) return false;

	FBLOG("enableGPUAudio: ep=%d dig=%d isDP=%d spkalloc=0x%02x ch=%d",
	      endpoint, digIndex, isDP, speakerAlloc, numChannels);

	/* Phase 1: Configure Azalia endpoint */

	/* 1a. Disable clock gating */
	uint32_t hpc = azEndpointRead(endpoint, AZ_REG_PIN_CONTROL_HOT_PLUG_CONTROL);
	azEndpointWrite(endpoint, AZ_REG_PIN_CONTROL_HOT_PLUG_CONTROL,
	                hpc | AZ_HPC_CLOCK_GATING_DISABLE);

	/* 1b. Set CHANNEL_SPEAKER: speaker allocation + connection type */
	uint32_t cs = azEndpointRead(endpoint, AZ_REG_PIN_CONTROL_CHANNEL_SPEAKER);
	cs &= ~(AZ_CS_SPEAKER_ALLOC_MASK | AZ_CS_HDMI_CONNECTION | AZ_CS_DP_CONNECTION);
	cs |= (speakerAlloc & AZ_CS_SPEAKER_ALLOC_MASK);
	if (isDP)
		cs |= AZ_CS_DP_CONNECTION;
	else
		cs |= AZ_CS_HDMI_CONNECTION;
	azEndpointWrite(endpoint, AZ_REG_PIN_CONTROL_CHANNEL_SPEAKER, cs);

	/* 1c. Write LPCM audio descriptor (format 0):
	 *   MAX_CHANNELS=1 (stereo), SUPPORTED_FREQUENCIES=0x07 (32/44.1/48),
	 *   DESCRIPTOR_BYTE_2=0x01 (16-bit), STEREO_FREQS=0x07 */
	uint32_t desc0 = ((numChannels - 1) & 0x07) |
	                 (0x07 << 8) |    /* 32, 44.1, 48 kHz */
	                 (0x07 << 16) |   /* 16, 20, 24-bit */
	                 (0x07 << 24);    /* stereo frequencies */
	azEndpointWrite(endpoint, AZ_REG_PIN_CONTROL_AUDIO_DESCRIPTOR(0), desc0);

	/* 1d. Clear other audio descriptors */
	for (int i = 1; i < 14; i++)
		azEndpointWrite(endpoint, AZ_REG_PIN_CONTROL_AUDIO_DESCRIPTOR(i), 0);

	/* 1e. Re-enable clock gating */
	hpc = azEndpointRead(endpoint, AZ_REG_PIN_CONTROL_HOT_PLUG_CONTROL);
	hpc &= ~AZ_HPC_CLOCK_GATING_DISABLE;
	azEndpointWrite(endpoint, AZ_REG_PIN_CONTROL_HOT_PLUG_CONTROL, hpc);

	const AZRegOffsets *r = (const AZRegOffsets *)mRegs;

	/* Phase 2: Map DIG encoder to audio endpoint */

	/* 2a. Set audio source: map DIG to this endpoint */
	gpuWrite32(r->afmtSrcCtl0 + digIndex * r->digStride, endpoint & 0x07);

	/* 2b. Enable audio channels on this DIG */
	uint32_t pktCtl2 = gpuRead32(r->afmtPktCtl2_0 + digIndex * r->digStride);
	pktCtl2 &= ~0xFF00u;  /* clear AUDIO_CHANNEL_ENABLE */
	pktCtl2 |= (0x03 << 8);  /* enable 2 channels (stereo) */
	gpuWrite32(r->afmtPktCtl2_0 + digIndex * r->digStride, pktCtl2);

	/* Phase 3: Setup DTO clock for DP */
	if (isDP) {
		uint32_t dtoSrc = gpuRead32(r->dccgDtoSource);
		dtoSrc &= ~0x30u;  /* clear DTO_SEL */
		dtoSrc |= (1u << 4);  /* DTO_SEL = 1 for DP */
		gpuWrite32(r->dccgDtoSource, dtoSrc);

		/* DP ref clock ~100 MHz */
		gpuWrite32(r->dccgDto1Module, 1000000);  /* 100 MHz in 100Hz units */
		gpuWrite32(r->dccgDto1Phase, 240000);    /* 24 MHz in 100Hz units */
  } else {
    // HDMI audio setup через AFMT
    uint32_t afmtCntl = gpuRead32(r->afmtCntl0 + digIndex * r->digStride);
    afmtCntl |= AFMT_AUDIO_CLOCK_EN;
    gpuWrite32(r->afmtCntl0 + digIndex * r->digStride, afmtCntl);
    
    uint32_t pktCtl = gpuRead32(r->afmtPktCtl0 + digIndex * r->digStride);
    pktCtl |= AFMT_AUDIO_SAMPLE_SEND;
    gpuWrite32(r->afmtPktCtl0 + digIndex * r->digStride, pktCtl);
    FBLOG("AFMT: afmtCntl=%x pktCtl=%x", afmtCntl, pktCtl);
  }

	/* Phase 4: Enable AZ audio */
	hpc = azEndpointRead(endpoint, AZ_REG_PIN_CONTROL_HOT_PLUG_CONTROL);
	azEndpointWrite(endpoint, AZ_REG_PIN_CONTROL_HOT_PLUG_CONTROL,
	                hpc | AZ_HPC_CLOCK_GATING_DISABLE | AZ_HPC_AUDIO_ENABLED);
	/* Re-enable clock gating while keeping AUDIO_ENABLED */
	azEndpointWrite(endpoint, AZ_REG_PIN_CONTROL_HOT_PLUG_CONTROL,
	                (hpc & ~AZ_HPC_CLOCK_GATING_DISABLE) | AZ_HPC_AUDIO_ENABLED);

	FBLOG("enableGPUAudio: AZ AUDIO_ENABLED set for ep=%d", endpoint);

	/* Phase 5: Enable DIG audio formatter and DP secondary packets */

	/* 5a. Enable AFMT clock */
	gpuWrite32(r->afmtCntl0 + digIndex * r->digStride,
	           gpuRead32(r->afmtCntl0 + digIndex * r->digStride) | AFMT_AUDIO_CLOCK_EN);

	if (isDP) {
		/* 5b. DP audio setup */
		gpuWrite32(r->dpSecAudN0 + digIndex * r->digStride, 0x8000);  /* default N */

		uint32_t timestamp = gpuRead32(r->dpSecTimestamp0 + digIndex * r->digStride);
		timestamp &= ~0x01u;  /* AUTO_CALC mode */
		gpuWrite32(r->dpSecTimestamp0 + digIndex * r->digStride, timestamp);

		/* 5c. Enable DP secondary packet types */
		uint32_t dpSec = gpuRead32(r->dpSecCntl0 + digIndex * r->digStride);
		dpSec |= DP_SEC_ASP_ENABLE | DP_SEC_ATP_ENABLE | DP_SEC_AIP_ENABLE;
		gpuWrite32(r->dpSecCntl0 + digIndex * r->digStride, dpSec);

		/* Master enable LAST */
		dpSec |= DP_SEC_STREAM_ENABLE;
		gpuWrite32(r->dpSecCntl0 + digIndex * r->digStride, dpSec);
	}

	/* 5d. Unmute audio */
	uint32_t pktCtl = gpuRead32(r->afmtPktCtl0 + digIndex * r->digStride);
	pktCtl |= AFMT_AUDIO_SAMPLE_SEND;
	gpuWrite32(r->afmtPktCtl0 + digIndex * r->digStride, pktCtl);

	FBLOG("enableGPUAudio: DIG%d DP_SEC + AFMT enabled", digIndex);

	return true;
}

/* Called from updateHDMIEnginePresence() when a pin loses presence (cable removed).
 * Tells the GPU it can stop the audio pipe for that output → allows GPU power gating. */
void VoodooHDAFramebufferNotifier::disableAudioPipeForPin(int cad, nid_t pinNid)
{
  IOLockLock(mLock);
  for (int i = 0; i < mNumConnections; i++) {
    FBConnectionState *conn = &mConnections[i];
    if (conn->mappedCodecCad == cad && conn->mappedPinNid == pinNid) {
      disableAudioPipe(conn);
      /* Do NOT reset edidValid here: the EDID/ELD data must remain available
       * so that injectELDIntoPinIfReady() can still serve other pins (e.g.
       * the FB connector maps to nid=3 but the display is on nid=7).
       * edidValid is cleared only when the display truly disconnects
       * (handleFramebufferTerminated / kIOMessageServiceIsTerminated). */
      break;
    }
  }
  IOLockUnlock(mLock);
}


bool VoodooHDAFramebufferNotifier::getPreferredConnectedPin(int cad, const nid_t *pinNids, int pinCount, nid_t *outPin)
{
  if (!mLock || !pinNids || pinCount <= 0 || !outPin)
    return false;
  
  IOLockLock(mLock);
  
  /*
   * Strongest rule: if an online/EDID-backed AMDFramebuffer publishes
   * audio-codec-info, trust that NID even if the earlier slot/index mapping
   * was fallback-based.  This fixes RX580/Polaris cards where the connected
   * display is on framebuffer@1 with audio-codec-info <00 01 09 00>, while
   * the HDA stale-presence list would otherwise select pin 3 or pin 5.
   */
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < mNumConnections; i++) {
      FBConnectionState *conn = &mConnections[i];
      if (conn->mappedCodecCad != cad || !conn->framebuffer)
        continue;
      bool eligible = (pass == 0) ? (conn->displayOnline && conn->edidValid)
      : (conn->displayOnline || conn->edidValid);
      if (!eligible)
        continue;
      nid_t codecPin = -1;
      if (!vhdaReadAMDAudioCodecInfoPin(conn->framebuffer, &codecPin))
        continue;
      for (int p = 0; p < pinCount; p++) {
        if (pinNids[p] == codecPin) {
          *outPin = codecPin;
          IOLockUnlock(mLock);
          return true;
        }
      }
    }
  }
  
  /* Prefer a framebuffer connection with valid EDID and an enabled audio pipe. */
  for (int pass = 0; pass < 3; pass++) {
    for (int i = 0; i < mNumConnections; i++) {
      FBConnectionState *conn = &mConnections[i];
      if (conn->mappedCodecCad != cad || conn->mappedPinNid < 0)
        continue;
      
      bool eligible = false;
      switch (pass) {
        case 0:
          eligible = conn->displayOnline && conn->edidValid && conn->audioPipeEnabled;
          break;
        case 1:
          eligible = conn->displayOnline && conn->edidValid;
          break;
        default:
          eligible = conn->edidValid || conn->displayOnline;
          break;
      }
      if (!eligible)
        continue;
      
      for (int p = 0; p < pinCount; p++) {
        if (pinNids[p] == conn->mappedPinNid) {
          *outPin = conn->mappedPinNid;
          IOLockUnlock(mLock);
          return true;
        }
      }
    }
  }
  
  IOLockUnlock(mLock);
  return false;
}

