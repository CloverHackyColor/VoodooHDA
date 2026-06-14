#include "License.h"

#include "VoodooHDAFramebufferNotifier.h"
#include "VoodooHDADevice.h"
#include "VoodooHDAEngine.h"
#include "Verbs.h"

/*
 * AMD HDMI safety mode.
 *
 * Target compatibility matrix covered by the HDMI policy:
 *   Polaris: RX460, RX470, RX480, RX550, RX560, RX570, RX580
 *   Navi/RDNA1: RX5500/XT, RX5600/XT, RX5700/XT
 *   Navi/RDNA2: RX6600/XT, RX6650/XT, RX6700/XT, RX6750/XT,
 *               RX6800/XT, RX6900/XT, RX6950/XT
 *
 * Keep dangerous direct GPU/MMIO diagnostics disabled in the normal path.
 * HDMI audio is configured through HDA codec verbs, ELD and a late framebuffer
 * audio-pipe enable only when CoreAudio starts the selected connected HDMI.
 */
#ifndef VHDA_AMD_FB_SAFE_MODE
#define VHDA_AMD_FB_SAFE_MODE 1
#endif

#include <IOKit/pci/IOPCIDevice.h>

OSDefineMetaClassAndStructors(VoodooHDAFramebufferNotifier, OSObject)

/* Forward declarations: these helpers are used by early matching code before
 * their implementations near the AMD register table section. Keeping the
 * definitions later avoids moving larger HDMI policy blocks, while preventing
 * Xcode/clang from failing with undeclared identifier errors. */
static VoodooHDAAMDGPUFamily classifyAMDGPUDevice(UInt16 gpuDeviceId);
static const char *amdHdmiGpuFamilyNameForFamily(VoodooHDAAMDGPUFamily family);
static const char *amdHdmiGpuFamilyName(UInt16 gpuDeviceId);

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

static Codec *vhdaGetCodecIfValid(VoodooHDADevice *device, int cad)
{
	if (!device || cad < 0 || cad >= HDAC_CODEC_MAX)
		return NULL;
	return device->mCodecs[cad];
}

static bool vhdaCopyEDIDPropertyToConnection(OSData *edidProp, FBConnectionState *conn)
{
	if (!edidProp || !conn)
		return false;

	UInt32 edidLen = edidProp->getLength();
	const void *edidBytes = edidProp->getBytesNoCopy();
	if (!edidBytes || edidLen < 128 || edidLen > 1024)
		return false;

	if (conn->edidData) {
		IOFree(conn->edidData, conn->edidLen);
		conn->edidData = NULL;
		conn->edidLen = 0;
	}

	conn->edidData = (uint8_t *)IOMalloc(edidLen);
	if (!conn->edidData)
		return false;

	memcpy(conn->edidData, edidBytes, edidLen);
	conn->edidLen = edidLen;
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

#undef super
#define super OSObject

#if VOODOO_HDA_DEBUG_BUILD
#define FBLOG_OWNER(owner, fmt, ...) do { \
	if ((owner) && (owner)->mDevice && (owner)->mDevice->mVerbose >= 1) \
		IOLog("VoodooHDA FB: " fmt "\n", ##__VA_ARGS__); \
} while (0)
#define FBLOG(fmt, ...) FBLOG_OWNER(this, fmt, ##__VA_ARGS__)
#else
#define FBLOG_OWNER(owner, fmt, ...) do { \
	(void)(owner); \
	if (false) IOLog("VoodooHDA FB: " fmt "\n", ##__VA_ARGS__); \
} while (0)
#define FBLOG(fmt, ...) do { \
	if (false) IOLog("VoodooHDA FB: " fmt "\n", ##__VA_ARGS__); \
} while (0)
#endif

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
	if (!device)
		return false;

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
	mAMDGPUDeviceId = 0;
	mAMDGPUFamily = kVoodooHDAAMDGPUUnknown;
	mStopping = false;
	mLock = IOLockAlloc();
	if (!mLock) {
		mDevice = NULL;
		mHDAPciDevice = NULL;
		return false;
	}

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

/* ---------- pin registration ---------- */

void VoodooHDAFramebufferNotifier::registerATIPins(int cad, nid_t *pinNids, int count)
{
	if (!mLock || !pinNids || count <= 0 || cad < 0 || cad >= HDAC_CODEC_MAX)
		return;
	bool shouldUpdatePresence = false;
	IOLockLock(mLock);
	mATIPinCad = cad;
	mATIPinCount = (count > VHDA_FB_MAX_PINS) ? VHDA_FB_MAX_PINS : count;
	for (int i = 0; i < mATIPinCount; i++)
		mATIPinNids[i] = pinNids[i];
	for (int i = 0; i < mNumConnections; i++) {
		FBConnectionState *conn = &mConnections[i];
		if (!conn->framebuffer)
			continue;
		conn->mappedPinNid = -1;
		conn->mappedCodecCad = -1;
		mapConnectionToPin(conn, i);
		if (conn->mappedPinNid >= 0)
			shouldUpdatePresence = true;
	}
	FBLOG("registerATIPins: cad=%d count=%d nids=[%d,%d,%d,%d,%d,%d]",
	      cad, mATIPinCount,
	      mATIPinCount > 0 ? mATIPinNids[0] : -1,
	      mATIPinCount > 1 ? mATIPinNids[1] : -1,
	      mATIPinCount > 2 ? mATIPinNids[2] : -1,
	      mATIPinCount > 3 ? mATIPinNids[3] : -1,
	      mATIPinCount > 4 ? mATIPinNids[4] : -1,
	      mATIPinCount > 5 ? mATIPinNids[5] : -1);
	IOLockUnlock(mLock);
	if (shouldUpdatePresence && mDevice)
		mDevice->updateHDMIEnginePresence();
}

/* ---------- IOService matching ---------- */

void VoodooHDAFramebufferNotifier::startMatching()
{
	if (!mLock || !mDevice)
		return;

	IOLockLock(mLock);
	mStopping = false;
	if (mGFXMatchNotifier || mGFXTermNotifier || mDisplayMatchNotifier) {
		IOLockUnlock(mLock);
		FBLOG("startMatching: notifications already registered");
		return;
	}
	IOLockUnlock(mLock);

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
	if (!mLock)
		return;

	/* Do not call IONotifier::remove() while holding mLock.  CoreDisplay/
	 * IOGraphics can synchronously deliver a final callback during remove(), and
	 * that callback can re-enter this object.  Mark the notifier as stopping,
	 * detach all pointers under the lock, then remove the notifiers after the
	 * lock is released. */
	IONotifier *gfxMatchNotifier = NULL;
	IONotifier *gfxTermNotifier = NULL;
	IONotifier *displayMatchNotifier = NULL;
	IONotifier *fbNotifiers[VHDA_FB_MAX_CONNECTIONS];
	int fbNotifierCount = 0;
	bzero(fbNotifiers, sizeof(fbNotifiers));

	IOLockLock(mLock);
	mStopping = true;

	for (int i = 0; i < mNumConnections; i++) {
		FBConnectionState *conn = &mConnections[i];
		if (conn->fbNotifier && fbNotifierCount < VHDA_FB_MAX_CONNECTIONS) {
			fbNotifiers[fbNotifierCount++] = conn->fbNotifier;
			conn->fbNotifier = NULL;
		}
		if (conn->edidData) {
			IOFree(conn->edidData, conn->edidLen);
			conn->edidData = NULL;
			conn->edidLen = 0;
		}
		if (conn->eld) {
			IOFree(conn->eld, conn->eldLen);
			conn->eld = NULL;
			conn->eldLen = 0;
		}
		conn->framebuffer = NULL;
		conn->displayOnline = false;
		conn->edidValid = false;
		conn->audioPipeEnabled = false;
	}
	mNumConnections = 0;

	gfxMatchNotifier = mGFXMatchNotifier;
	gfxTermNotifier = mGFXTermNotifier;
	displayMatchNotifier = mDisplayMatchNotifier;
	mGFXMatchNotifier = NULL;
	mGFXTermNotifier = NULL;
	mDisplayMatchNotifier = NULL;
	IOLockUnlock(mLock);

	for (int i = 0; i < fbNotifierCount; i++) {
		if (fbNotifiers[i])
			fbNotifiers[i]->remove();
	}
	if (gfxMatchNotifier)
		gfxMatchNotifier->remove();
	if (gfxTermNotifier)
		gfxTermNotifier->remove();
	if (displayMatchNotifier)
		displayMatchNotifier->remove();

	FBLOG("stopMatching: all notifiers removed");
}

/* ---------- PCI topology matching ---------- */

bool VoodooHDAFramebufferNotifier::isSameGPU(IOService *fbService)
{
	if (!mHDAPciDevice || !fbService) return false;

	/* Walk up the provider chain from the framebuffer to find its IOPCIDevice */
	IOService *provider = fbService;
	IOPCIDevice *fbPCI = NULL;
	for (int depth = 0; depth < 32 && provider; depth++) {
		fbPCI = OSDynamicCast(IOPCIDevice, provider);
		if (fbPCI) break;
		provider = provider->getProvider();
	}
	if (!fbPCI) {
		int fbIndex = -1;
		if (vhdaIsIONDRVFramebuffer(fbService) &&
		    vhdaGetFramebufferConnectorIndex(fbService, &fbIndex) &&
		    vhdaIsLegacyPolarisHDADevice(mHDAPciDevice)) {
			mAMDGPUDeviceId = 0;
			mAMDGPUFamily = kVoodooHDAAMDGPUClassicPolaris;
			FBLOG("isSameGPU: accepting legacy Polaris IONDRV framebuffer index=%d without PCI provider", fbIndex);
			return true;
		}
		return false;
	}

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
	if (!same && vhdaIsIONDRVFramebuffer(fbService) && vhdaIsLegacyPolarisHDADevice(mHDAPciDevice) &&
	    classifyAMDGPUDevice(fbPCI->configRead16(kIOPCIConfigDeviceID)) == kVoodooHDAAMDGPUClassicPolaris) {
		int fbIndex = -1;
		if (vhdaGetFramebufferConnectorIndex(fbService, &fbIndex)) {
			FBLOG("isSameGPU: accepting legacy Polaris IONDRV framebuffer index=%d hdaParent=%p fbParent=%p",
			      fbIndex, hdaParent, fbParent);
			same = true;
		}
	}
	if (same) {
		UInt16 gpuDeviceId = fbPCI->configRead16(kIOPCIConfigDeviceID);
		VoodooHDAAMDGPUFamily family = classifyAMDGPUDevice(gpuDeviceId);
		mAMDGPUDeviceId = gpuDeviceId;
		mAMDGPUFamily = family;
		FBLOG("isSameGPU: fbPCI=%p vendor=0x%04x device=0x%04x family=%s hdaParent=%p fbParent=%p -> YES",
		      fbPCI, fbVendor, gpuDeviceId, amdHdmiGpuFamilyNameForFamily(family), hdaParent, fbParent);
	} else {
		FBLOG("isSameGPU: fbPCI=%p vendor=0x%04x hdaParent=%p fbParent=%p -> NO",
		      fbPCI, fbVendor, hdaParent, fbParent);
	}
	return same;
}

/* ---------- matching callbacks ---------- */

bool VoodooHDAFramebufferNotifier::gfxMatchedHandler(
	void *target, void *refCon, IOService *newService, IONotifier *notifier)
{
	VoodooHDAFramebufferNotifier *self = (VoodooHDAFramebufferNotifier *)target;
	if (!self || self->mStopping || !newService) return true;

	FBLOG_OWNER(self, "gfxMatchedHandler: service=%p class=%s",
		    newService, newService->getMetaClass()->getClassName());

	if (!self->isSameGPU(newService)) return true;

	self->handleFramebufferMatched(newService);
	return true;
}

bool VoodooHDAFramebufferNotifier::gfxTerminatedHandler(
	void *target, void *refCon, IOService *service, IONotifier *notifier)
{
	VoodooHDAFramebufferNotifier *self = (VoodooHDAFramebufferNotifier *)target;
	if (!self || self->mStopping || !service) return true;
	self->handleFramebufferTerminated(service);
	return true;
}

bool VoodooHDAFramebufferNotifier::displayMatchedHandler(
	void *target, void *refCon, IOService *newService, IONotifier *notifier)
{
	VoodooHDAFramebufferNotifier *self = (VoodooHDAFramebufferNotifier *)target;
	if (!self || self->mStopping || !self->mLock || !newService) return true;

	OSData *edidProp = OSDynamicCast(OSData, newService->getProperty(kIODisplayEDIDKey));
	if (!edidProp || edidProp->getLength() < 128) return true;

	/*
	 * Walk up: IODisplay -> IODisplayConnect -> IOFramebuffer
	 * Match the IOFramebuffer to one of our registered connections.
	 */
	IOService *parent = newService->getProvider();           /* IODisplayConnect */
	IOService *fb = parent ? parent->getProvider() : NULL;   /* IOFramebuffer   */
	if (!fb) return true;

	bool updatePresenceAfterUnlock = false;
	IOLockLock(self->mLock);
	FBConnectionState *conn = self->findConnection(fb);
	if (conn && !conn->edidValid) {
		FBLOG_OWNER(self, "displayMatched: IODisplay for pin=%d, reading EDID", conn->mappedPinNid);

		if (vhdaCopyEDIDPropertyToConnection(edidProp, conn)) {
			FBLOG_OWNER(self, "displayMatched: pin=%d got %d bytes EDID", conn->mappedPinNid, conn->edidLen);

			if (self->parseEDIDAudio(conn)) {
				self->buildELDFromEDID(conn);
				conn->edidValid = true;
				conn->displayOnline = true;

				/*
				 * RX4xx/RX5xx/RX580 black-screen mitigation:
				 * do NOT enable kConnectionEnableAudio from the IODisplay match
				 * callback.  EDID availability only means the display exists; on
				 * Polaris this can still happen while the display pipe is training.
				 * The framebuffer audio pipe is enabled later, at HDMI stream start,
				 * after CoreAudio actually opens the selected engine.
				 */
				FBLOG_OWNER(self, "displayMatched: EDID ready, deferring audio pipe enable until stream start for pin=%d",
				            conn->mappedPinNid);

				/*
				 * Inject ELD into the mapped pin AND all pins with presence so the
				 * correct HDA pin receives the EDID-based capabilities even when the
				 * framebuffer connector order differs between RX460/470/480/550/560/570/580
				 * and RX5500/5600/5700/RX6xxx boards.
				 */
				self->injectELDIntoWidget(conn);
				self->injectELDIntoAllPinsWithPresence(conn);
				updatePresenceAfterUnlock = true;

				FBLOG_OWNER(self, "displayMatched: pin=%d spkalloc=0x%02x nsads=%d ELD ready",
					    conn->mappedPinNid, conn->speakerAllocation, conn->numSADs);
			}
		} else {
			FBLOG_OWNER(self, "displayMatched: invalid or oversized EDID, ignoring");
		}
	}
	IOLockUnlock(self->mLock);
	if (updatePresenceAfterUnlock && self->mDevice)
		self->mDevice->updateHDMIEnginePresence();
	return true;
}

/* ---------- framebuffer attach/detach ---------- */

void VoodooHDAFramebufferNotifier::handleFramebufferMatched(IOService *fb)
{
	IOLockLock(mLock);

	int idx = -1;

	for (int i = 0; i < mNumConnections; i++) {
		if (mConnections[i].framebuffer == fb) {
			IOLockUnlock(mLock);
			return;
		}
		if (!mConnections[i].framebuffer && idx < 0)
			idx = i;
	}

	if (idx < 0) {
		if (mNumConnections >= VHDA_FB_MAX_CONNECTIONS) {
			FBLOG("handleFBMatched: max connections reached");
			IOLockUnlock(mLock);
			return;
		}
		idx = mNumConnections++;
	}

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
	 * AMD HDMI black-screen safety: do not call setAttributeForConnectionExt() during
	 * framebuffer matching. At this stage the display pipe may still be training;
	 * touching kConnectionEnableAudio can blank the display on some cards.
	 */

	/* Try reading EDID immediately (usually too early — displayMatchedHandler will retry) */
	if (readEDID(conn) && parseEDIDAudio(conn)) {
		buildELDFromEDID(conn);
		conn->edidValid = true;
		conn->displayOnline = true;
		FBLOG("handleFBMatched: EDID valid, spkalloc=0x%02x nsads=%d",
		      conn->speakerAllocation, conn->numSADs);
		injectELDIntoWidget(conn);
	}

	IOLockUnlock(mLock);
}

void VoodooHDAFramebufferNotifier::handleFramebufferTerminated(IOService *fb)
{
	if (!mLock) return;
	IONotifier *fbNotifier = NULL;

	IOLockLock(mLock);
	for (int i = 0; i < mNumConnections; i++) {
		FBConnectionState *conn = &mConnections[i];
		if (conn->framebuffer != fb) continue;

		FBLOG("handleFBTerminated: fb=%p slot=%d", fb, i);
		if (conn->fbNotifier) {
			fbNotifier = conn->fbNotifier;
			conn->fbNotifier = NULL;
		}
		disableAudioPipe(conn);
		clearWidgetELD(conn);
		if (conn->edidData) { IOFree(conn->edidData, conn->edidLen); conn->edidData = NULL; }
		if (conn->eld) { IOFree(conn->eld, conn->eldLen); conn->eld = NULL; }

		/* Do not compact mConnections here.  IOService interest notifications keep
		 * refCon pointers to entries in this fixed array; shifting entries can make
		 * late CoreDisplay/IOGraphics callbacks reference the wrong connection and
		 * can race with VoodooHDAEngine teardown during kext replacement.  Clear the
		 * slot in place and trim only trailing empty slots. */
		bzero(conn, sizeof(*conn));
		while (mNumConnections > 0 && !mConnections[mNumConnections - 1].framebuffer)
			mNumConnections--;
		break;
	}
	IOLockUnlock(mLock);

	/* IONotifier::remove() can synchronously re-enter our interest handler.
	 * Keep it outside mLock, matching stopMatching() teardown. */
	if (fbNotifier)
		fbNotifier->remove();
}

/* ---------- IOService interest handler (replaces IOFramebuffer notification) ---------- */

IOReturn VoodooHDAFramebufferNotifier::interestHandler(
	void *target, void *refCon, UInt32 messageType, IOService *provider,
	void *messageArgument, vm_size_t argSize)
{
	VoodooHDAFramebufferNotifier *self = (VoodooHDAFramebufferNotifier *)target;
	if (!self || self->mStopping || !self->mLock) return kIOReturnSuccess;
	FBConnectionState *conn = (FBConnectionState *)refCon;
	if (!conn) return kIOReturnSuccess;

	/*
	 * kIOMessageServicePropertyChange fires when IOFramebuffer properties change
	 * (including IODisplayEDID). This is our trigger to re-read EDID.
	 */
	if (messageType == kIOMessageServicePropertyChange) {
		bool updatePresenceAfterUnlock = false;
		IOLockLock(self->mLock);
		if (conn->framebuffer == provider) {
			FBLOG_OWNER(self, "interestHandler: PropertyChange fb=%p pin=%d", provider, conn->mappedPinNid);
			if (self->readEDID(conn) && self->parseEDIDAudio(conn)) {
				self->buildELDFromEDID(conn);
				conn->edidValid = true;
				conn->displayOnline = true;
				self->injectELDIntoWidget(conn);
				self->injectELDIntoAllPinsWithPresence(conn);
				updatePresenceAfterUnlock = true;
				FBLOG_OWNER(self, "interestHandler: EDID updated, spkalloc=0x%02x nsads=%d",
					    conn->speakerAllocation, conn->numSADs);
			}
		}
		IOLockUnlock(self->mLock);
		if (updatePresenceAfterUnlock && self->mDevice)
			self->mDevice->updateHDMIEnginePresence();
	} else if (messageType == kIOMessageServiceIsTerminated) {
#if VOODOO_HDA_DEBUG_BUILD
		nid_t pinNid = -1;
		bool validProvider = false;
		IOLockLock(self->mLock);
		if (conn->framebuffer == provider) {
			pinNid = conn->mappedPinNid;
			validProvider = true;
		}
		IOLockUnlock(self->mLock);
		if (validProvider)
			FBLOG_OWNER(self, "interestHandler: service terminated fb=%p pin=%d", provider, pinNid);
#endif
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

bool VoodooHDAFramebufferNotifier::readEDID(FBConnectionState *conn)
{
	if (!conn->framebuffer) return false;

	if (conn->edidData) {
		IOFree(conn->edidData, conn->edidLen);
		conn->edidData = NULL;
		conn->edidLen = 0;
	}

	/*
	 * IODisplayEDID lives on the IODisplay child, not on the IOFramebuffer itself.
	 * IOKit tree: IOFramebuffer -> IODisplayConnect -> IODisplay (has IODisplayEDID).
	 * Search up to 2 levels deep without requiring IOGraphicsFamily headers.
	 */
	OSData *edidProp = OSDynamicCast(OSData, conn->framebuffer->getProperty(kIODisplayEDIDKey));

	if (!edidProp) {
		OSIterator *iter = conn->framebuffer->getChildIterator(gIOServicePlane);
		if (iter) {
			IOService *child;
			while (!edidProp && (child = OSDynamicCast(IOService, iter->getNextObject()))) {
				edidProp = OSDynamicCast(OSData, child->getProperty(kIODisplayEDIDKey));
				if (!edidProp) {
					/* IODisplayConnect -> IODisplay */
					OSIterator *iter2 = child->getChildIterator(gIOServicePlane);
					if (iter2) {
						IOService *grandChild;
						while (!edidProp && (grandChild = OSDynamicCast(IOService, iter2->getNextObject())))
							edidProp = OSDynamicCast(OSData, grandChild->getProperty(kIODisplayEDIDKey));
						iter2->release();
					}
				}
			}
			iter->release();
		}
	}

	if (edidProp && vhdaCopyEDIDPropertyToConnection(edidProp, conn)) {
		FBLOG("readEDID: pin=%d got %d bytes", conn->mappedPinNid, conn->edidLen);
		return true;
	}

	FBLOG("readEDID: pin=%d no EDID available", conn->mappedPinNid);
	return false;
}

/* ---------- EDID CEA audio parsing ---------- */

bool VoodooHDAFramebufferNotifier::parseEDIDAudio(FBConnectionState *conn)
{
	if (!conn->edidData || conn->edidLen < 128) return false;

	conn->speakerAllocation = 0;
	conn->numSADs = 0;
	bzero(conn->sads, sizeof(conn->sads));

	int numExtensions = conn->edidData[126];
	if (numExtensions == 0 || conn->edidLen < 256) {
		/* No CEA block — default to stereo LPCM */
		conn->speakerAllocation = 0x01;
		conn->sads[0] = 0x09; /* LPCM, 2ch */
		conn->sads[1] = 0x07; /* 32/44.1/48 kHz */
		conn->sads[2] = 0x01; /* 16-bit */
		conn->numSADs = 1;
		FBLOG("parseEDIDAudio: pin=%d no CEA extension, using default stereo LPCM", conn->mappedPinNid);
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
		FBLOG("parseEDIDAudio: pin=%d no basic audio support", conn->mappedPinNid);
		return false;
	}

	if (dtdOffset == 0)
		dtdOffset = 127;
	if (dtdOffset < 4 || dtdOffset > 127) {
		FBLOG("parseEDIDAudio: pin=%d invalid CEA dtdOffset=%d", conn->mappedPinNid, dtdOffset);
		return false;
	}

	int pos = 4;
	while (pos < dtdOffset && pos < 127) {
		int tag = (cea[pos] >> 5) & 0x07;
		int blockLen = cea[pos] & 0x1f;
		pos++;
		if (pos + blockLen > dtdOffset || pos + blockLen > 127) break;

		if (tag == 1) {
			int nSADs = blockLen / 3;
			for (int i = 0; i < nSADs && conn->numSADs < VHDA_FB_MAX_SADS; i++) {
				int off = conn->numSADs * 3;
				conn->sads[off + 0] = cea[pos + i * 3 + 0];
				conn->sads[off + 1] = cea[pos + i * 3 + 1];
				conn->sads[off + 2] = cea[pos + i * 3 + 2];
#if VOODOO_HDA_DEBUG_BUILD
				int fmt = (conn->sads[off + 0] >> 3) & 0x0f;
				int nch = (conn->sads[off + 0] & 0x07) + 1;
				FBLOG("parseEDIDAudio: pin=%d SAD[%d]: fmt=%d ch=%d rates=0x%02x bits=0x%02x",
				      conn->mappedPinNid, conn->numSADs, fmt, nch,
				      conn->sads[off + 1], conn->sads[off + 2]);
#endif
				conn->numSADs++;
			}
			FBLOG("parseEDIDAudio: pin=%d Audio Data Block: %d SADs", conn->mappedPinNid, nSADs);
		} else if (tag == 4) {
			if (blockLen >= 1) {
				conn->speakerAllocation = cea[pos];
				FBLOG("parseEDIDAudio: pin=%d Speaker Allocation: 0x%02x", conn->mappedPinNid, cea[pos]);
			}
		}
		pos += blockLen;
	}

	if (conn->numSADs > 0 && conn->speakerAllocation == 0)
		conn->speakerAllocation = 0x01;

	FBLOG("parseEDIDAudio: pin=%d result: spkalloc=0x%02x nsads=%d",
	      conn->mappedPinNid, conn->speakerAllocation, conn->numSADs);
	return (conn->numSADs > 0);
}

/* ---------- ELD construction ---------- */

void VoodooHDAFramebufferNotifier::buildELDFromEDID(FBConnectionState *conn)
{
	if (conn->eld) {
		IOFree(conn->eld, conn->eldLen);
		conn->eld = NULL;
		conn->eldLen = 0;
	}

	int mnl = 0;
	int baselineLen = 4 + mnl + conn->numSADs * 3;
	int totalLen = 4 + baselineLen;

	conn->eld = (uint8_t *)IOMalloc(totalLen);
	if (!conn->eld) return;
	conn->eldLen = totalLen;
	bzero(conn->eld, totalLen);

	conn->eld[0] = 0x02 << 3; /* ELD version 2 */
	conn->eld[2] = baselineLen / 4;
	conn->eld[4] = (conn->numSADs << 4) | mnl;
	conn->eld[5] = 0x00; /* HDMI */
	conn->eld[6] = 0;    /* audio sync delay */
	conn->eld[7] = conn->speakerAllocation;

	for (int i = 0; i < conn->numSADs * 3; i++)
		conn->eld[8 + i] = conn->sads[i];

	FBLOG("buildELD: pin=%d eldLen=%d spkalloc=0x%02x nsads=%d",
	      conn->mappedPinNid, totalLen, conn->speakerAllocation, conn->numSADs);
}

/* ---------- audio pipe control ---------- */

bool VoodooHDAFramebufferNotifier::enableAudioPipe(FBConnectionState *conn)
{
	if (!conn) return false;

#if VHDA_AMD_FB_SAFE_MODE
	/*
	 * RX5xx/Polaris black-screen protection: never touch
	 * IOFramebuffer::setAttributeForConnectionExt(kConnectionEnableAudio)
	 * automatically. The HDA codec is programmed by verbs only.
	 */
	FBLOG("enableAudioPipe: pin=%d skipped by RX5xx safe mode", conn->mappedPinNid);
	conn->audioPipeEnabled = false;
	return false;
#endif

	if (!conn->framebuffer) return false;
	if (conn->audioPipeEnabled) return true;

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

	/* Update engine name using a generic label; avoid GPU-family specific names. */
	if (conn->audioPipeEnabled && mDevice && conn->mappedPinNid >= 0) {
		for (int i = 0; i < mDevice->mNumHDMIEngines; i++) {
			VoodooHDADevice::HDMIEngineSlot *slot = &mDevice->mHDMIEngines[i];
			if (slot->engine && slot->pinNid == conn->mappedPinNid) {
				const char *desc = slot->engine->mPortName ?
				    slot->engine->mPortName : "VoodooHDA HDMI/DP Audio";
				slot->engine->setProperty("IOAudioEngineDescription", desc);
				FBLOG("enableAudioPipe: updated generic engine name for pin=%d", conn->mappedPinNid);
			}
		}
	}

	return conn->audioPipeEnabled;
}

void VoodooHDAFramebufferNotifier::retryEnableAudioPipeAll()
{
	/* AMD HDMI safety: do not retry framebuffer audio-pipe changes globally. */
}

void VoodooHDAFramebufferNotifier::disableAudioPipe(FBConnectionState *conn)
{
	if (!conn->audioPipeEnabled || !conn->framebuffer)
		return;
	/* Do not call setAttributeForConnectionExt(..., 0) on AMD HDMI; it can
	 * blank displays during termination/sleep transitions. */
	FBLOG("disableAudioPipe: pin=%d skipped framebuffer attribute write",
	      conn->mappedPinNid);
	conn->audioPipeEnabled = false;
}

/* Called from updateHDMIEnginePresence() when a pin loses presence (cable removed).
 * Tells the GPU it can stop the audio pipe for that output → allows GPU power gating. */
void VoodooHDAFramebufferNotifier::disableAudioPipeForPin(int cad, nid_t pinNid)
{
	if (!mLock)
		return;

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

/* ---------- ELD injection into VoodooHDA widget ---------- */

void VoodooHDAFramebufferNotifier::injectELDIntoWidget(FBConnectionState *conn)
{
	if (!mDevice || conn->mappedPinNid < 0 || !conn->eld || conn->eldLen == 0)
		return;

	nid_t targetPin = conn->mappedPinNid;
	nid_t codecInfoPin = -1;
	if (conn->framebuffer && vhdaReadAMDAudioCodecInfoPin(conn->framebuffer, &codecInfoPin))
		targetPin = codecInfoPin;

	Codec *codec = vhdaGetCodecIfValid(mDevice, conn->mappedCodecCad);
	if (!codec) return;

	for (int fg = 0; fg < codec->numFuncGroups; fg++) {
		FunctionGroup *funcGroup = &codec->funcGroups[fg];
		if (funcGroup->nodeType != HDA_PARAM_FCT_GRP_TYPE_NODE_TYPE_AUDIO)
			continue;
		Widget *w = mDevice->widgetGet(funcGroup, targetPin);
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
			FBLOG("injectELD: nid=%d eld_len=%d spkalloc=0x%02x",
			      targetPin, w->eld_len,
			      (w->eld_len > 7) ? w->eld[7] : 0);
		}
		return;
	}
}

void VoodooHDAFramebufferNotifier::injectELDIntoAllPinsWithPresence(FBConnectionState *conn)
{
	if (!mDevice || !conn->eld || conn->eldLen == 0)
		return;

	Codec *codec = vhdaGetCodecIfValid(mDevice, mATIPinCad);
	if (!codec) return;

	for (int fg = 0; fg < codec->numFuncGroups; fg++) {
		FunctionGroup *funcGroup = &codec->funcGroups[fg];
		if (funcGroup->nodeType != HDA_PARAM_FCT_GRP_TYPE_NODE_TYPE_AUDIO)
			continue;

		for (int i = 0; i < mATIPinCount; i++) {
			nid_t nid = mATIPinNids[i];
			if (nid == conn->mappedPinNid) continue; /* already injected by injectELDIntoWidget */

#if VOODOO_HDA_DEBUG_BUILD
			UInt32 pinSense = mDevice->sendCommand(
				HDA_CMD_GET_PIN_SENSE(mATIPinCad, nid), mATIPinCad);
#endif

			/* RX4xx/RX5xx compatibility: on several Polaris boards the HDA
			 * pin-sense bits are stale, inverted, or only become valid after
			 * kConnectionEnableAudio is set on the framebuffer connector.  ELD
			 * is harmless metadata, so broadcast the EDID-derived ELD to every
			 * ATI HDMI pin.  Connected-only publication still happens later in
			 * VoodooHDADevice::updateHDMIEnginePresence(), so this does not make
			 * extra HDMI outputs visible. */

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
#if VOODOO_HDA_DEBUG_BUILD
				FBLOG("injectELD(all-ati): nid=%d pinSense=0x%08x eld_len=%d",
				      nid, pinSense, w->eld_len);
#endif
			}
		}
		return;
	}
}

void VoodooHDAFramebufferNotifier::injectELDIntoPinIfReady(int cad, nid_t pinNid)
{
	if (!mLock)
		return;

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
		Codec *codec = vhdaGetCodecIfValid(mDevice, cad);
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
					FBLOG("injectELDIntoPinIfReady: nid=%d eld_len=%d spkalloc=0x%02x",
					      pinNid, w->eld_len, (w->eld_len > 7) ? w->eld[7] : 0);
				}
				break;
			}
		}
	}

	IOLockUnlock(mLock);
}

void VoodooHDAFramebufferNotifier::clearWidgetELD(FBConnectionState *conn)
{
	if (!mDevice || conn->mappedPinNid < 0) return;

	Codec *codec = vhdaGetCodecIfValid(mDevice, conn->mappedCodecCad);
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
	 * Do not toggle the pipe off on stop; that can blank displays during normal
	 * CoreAudio open/close churn. On start, make sure the single selected HDMI
	 * framebuffer connection has kConnectionEnableAudio enabled.
	 */
	if (streaming)
		ensureAudioPipeEnabled(cad, pinNid);
}

/* ---------- public query interface ---------- */

bool VoodooHDAFramebufferNotifier::detectedAMDGPUFamily(VoodooHDAAMDGPUFamily *outFamily, UInt16 *outDeviceId)
{
	if (!mLock)
		return false;
	IOLockLock(mLock);
	VoodooHDAAMDGPUFamily family = mAMDGPUFamily;
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

bool VoodooHDAFramebufferNotifier::prefersLegacyPolarisHDMIPinFallback()
{
	VoodooHDAAMDGPUFamily family = kVoodooHDAAMDGPUUnknown;
	return detectedAMDGPUFamily(&family, NULL) && family == kVoodooHDAAMDGPUClassicPolaris;
}

bool VoodooHDAFramebufferNotifier::shouldVoodooHDAEnableFramebufferAudioPipe()
{
	VoodooHDAAMDGPUFamily family = kVoodooHDAAMDGPUUnknown;
	UInt16 gpuDeviceId = 0;

	if (detectedAMDGPUFamily(&family, &gpuDeviceId)) {
		/* Native AMD framebuffer families already own their display pipe.
		 * Let AppleGFXHDA/AMDFramebuffer keep that state; VoodooHDA should only
		 * provide HDA verbs/ELD and avoid setAttributeForConnectionExt() here. */
		if (family == kVoodooHDAAMDGPUModernNavi ||
		    family == kVoodooHDAAMDGPUVega ||
		    family == kVoodooHDAAMDGPUVega20RadeonVII ||
		    family == kVoodooHDAAMDGPUGenericAMD)
			return false;

		return family == kVoodooHDAAMDGPUClassicPolaris;
	}

	/* Do not trust the separate AMD HDA audio-function ID alone here.  Some GPU
	 * generations reuse or spoof HDA IDs, so framebuffer pipe writes are allowed
	 * only after the matched GPU framebuffer has identified a Polaris path. */
	return false;
}

const char *VoodooHDAFramebufferNotifier::detectedAMDGPUFamilyName()
{
	VoodooHDAAMDGPUFamily family = kVoodooHDAAMDGPUUnknown;
	if (!detectedAMDGPUFamily(&family, NULL))
		return "Unknown AMD HDMI";
	return amdHdmiGpuFamilyNameForFamily(family);
}

bool VoodooHDAFramebufferNotifier::getAnyFramebufferELD(uint8_t **outELD, int *outLen)
{
	if (!mLock || !mDevice || !outELD || !outLen)
		return false;
	*outELD = NULL;
	*outLen = 0;
	IOLockLock(mLock);
	for (int i = 0; i < mNumConnections; i++) {
		FBConnectionState *conn = &mConnections[i];
		if (conn->eld && conn->eldLen > 0) {
			uint8_t *eldCopy = (uint8_t *)mDevice->allocMem(conn->eldLen);
			if (!eldCopy) {
				IOLockUnlock(mLock);
				return false;
			}
			memcpy(eldCopy, conn->eld, conn->eldLen);
			*outELD = eldCopy;
			*outLen = conn->eldLen;
			IOLockUnlock(mLock);
			return true;
		}
	}
	IOLockUnlock(mLock);
	return false;
}

bool VoodooHDAFramebufferNotifier::getPreferredConnectedPin(
	int cad, const nid_t *pinNids, int pinCount, nid_t *outPin)
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

bool VoodooHDAFramebufferNotifier::getFramebufferELD(
	int cad, nid_t pinNid, uint8_t **outELD, int *outLen)
{
	if (!mLock || !mDevice || !outELD || !outLen)
		return false;
	*outELD = NULL;
	*outLen = 0;
	IOLockLock(mLock);
	for (int i = 0; i < mNumConnections; i++) {
		FBConnectionState *conn = &mConnections[i];
		if (conn->mappedCodecCad != cad || !conn->eld || conn->eldLen <= 0)
			continue;
		nid_t effectivePin = conn->mappedPinNid;
		nid_t codecInfoPin = -1;
		if (conn->framebuffer && vhdaReadAMDAudioCodecInfoPin(conn->framebuffer, &codecInfoPin))
			effectivePin = codecInfoPin;
		if (effectivePin == pinNid) {
			uint8_t *eldCopy = (uint8_t *)mDevice->allocMem(conn->eldLen);
			if (!eldCopy) {
				IOLockUnlock(mLock);
				return false;
			}
			memcpy(eldCopy, conn->eld, conn->eldLen);
			*outELD = eldCopy;
			*outLen = conn->eldLen;
			IOLockUnlock(mLock);
			return true;
		}
	}
	IOLockUnlock(mLock);
	return false;
}

void VoodooHDAFramebufferNotifier::ensureAudioPipeEnabled(int cad, nid_t pinNid)
{
	if (!mLock)
		return;
	UInt16 diagFlags = mDevice ? mDevice->diagnosticFlagsForPin(cad, pinNid) : 0;
	bool skipAudioPipe = ((diagFlags & kVoodooHDADiagSkipAudioPipe) != 0) ||
	                     !shouldVoodooHDAEnableFramebufferAudioPipe();

	IOLockLock(mLock);
	FBConnectionState *target = NULL;

	/* First try the exact HDA pin -> framebuffer mapping. */
	for (int i = 0; i < mNumConnections; i++) {
		FBConnectionState *conn = &mConnections[i];
		if (conn->mappedCodecCad == cad && conn->mappedPinNid == pinNid && conn->framebuffer) {
			target = conn;
			break;
		}
	}

	/* If CoreAudio selected a pin based on audio-codec-info, locate the
	 * framebuffer that owns that codec pin even when the older fallback mapping
	 * stored a different mappedPinNid. */
	if (!target) {
		for (int i = 0; i < mNumConnections; i++) {
			FBConnectionState *conn = &mConnections[i];
			if (conn->mappedCodecCad != cad || !conn->framebuffer)
				continue;
			nid_t codecPin = -1;
			if (vhdaReadAMDAudioCodecInfoPin(conn->framebuffer, &codecPin) && codecPin == pinNid) {
				target = conn;
				FBLOG("ensureAudioPipeEnabled: pin=%d matched framebuffer by audio-codec-info", pinNid);
				break;
			}
		}
	}

	/* AMD fallback: if the HDA pin selected by CoreAudio does not match the
	 * framebuffer connector index, enable the first online EDID-backed
	 * framebuffer for the same HDMI codec.  This keeps Polaris RX460/470/480/
	 * 550/560/570/580 and Navi RX5500/5600/5700/RX6xxx variants working without
	 * enabling every stale HDA pin. */
	if (!target) {
		for (int i = 0; i < mNumConnections; i++) {
			FBConnectionState *conn = &mConnections[i];
			if (conn->mappedCodecCad == cad && conn->framebuffer && (conn->edidValid || conn->displayOnline)) {
				target = conn;
				FBLOG("ensureAudioPipeEnabled: pin=%d using framebuffer fallback pin=%d",
				      pinNid, conn->mappedPinNid);
				break;
			}
		}
	}

	if (target && !target->edidValid) {
		/* No EDID yet — GPU may be awake now, try again. Keep this read-only. */
		if (readEDID(target) && parseEDIDAudio(target)) {
			buildELDFromEDID(target);
			target->edidValid = true;
			injectELDIntoWidget(target);
			injectELDIntoAllPinsWithPresence(target);
		}
	}

	/* Copy only stable objects/values, retain the framebuffer, then drop
	 * mLock before calling IOFramebuffer::setAttributeForConnectionExt().
	 * Never pass an unlocked FBConnectionState pointer into the framebuffer call:
	 * the connection can terminate while CoreAudio is tearing down the engine. */
	bool shouldEnable = (target && !skipAudioPipe && !target->audioPipeEnabled &&
	                     target->framebuffer && (target->edidValid || target->displayOnline));
	IOService *enableFB = NULL;
	nid_t enablePin = -1;
	int enableCad = -1;
	if (shouldEnable) {
		enableFB = target->framebuffer;
		enableFB->retain();
		enablePin = target->mappedPinNid;
		enableCad = target->mappedCodecCad;
	}
	if (skipAudioPipe) {
		FBLOG("ensureAudioPipeEnabled: pin=%d framebuffer audio pipe write disabled by policy",
		      pinNid);
	}
	IOLockUnlock(mLock);

	if (enableFB) {
		IOFramebuffer *fb = reinterpret_cast<IOFramebuffer *>(enableFB);
		IOReturn ret = fb->setAttributeForConnectionExt(0, kConnectionEnableAudio, 1);
		FBLOG("ensureAudioPipeEnabled: pin=%d setAttributeForConnectionExt(kConnectionEnableAudio)=0x%x",
		      enablePin, ret);

		IOLockLock(mLock);
		for (int i = 0; i < mNumConnections; i++) {
			FBConnectionState *conn = &mConnections[i];
			if (conn->framebuffer == enableFB && conn->mappedCodecCad == enableCad &&
			    conn->mappedPinNid == enablePin) {
				conn->audioPipeEnabled = (ret == kIOReturnSuccess);
				break;
			}
		}
		IOLockUnlock(mLock);

		if (ret == kIOReturnSuccess && mDevice && enablePin >= 0) {
			for (int i = 0; i < mDevice->mNumHDMIEngines; i++) {
				VoodooHDADevice::HDMIEngineSlot *slot = &mDevice->mHDMIEngines[i];
				if (slot->engine && slot->pinNid == enablePin) {
					const char *desc = slot->engine->mPortName ?
					    slot->engine->mPortName : "VoodooHDA HDMI/DP Audio";
					slot->engine->setProperty("IOAudioEngineDescription", desc);
					break;
				}
			}
		}
		enableFB->release();
	}
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
#define AZ_REG_PIN_CONTROL_CHANNEL_SPEAKER          0x25
#define AZ_REG_PIN_CONTROL_AUDIO_DESCRIPTOR(n)      (0x28 + (n))
#define AZ_REG_PIN_CONTROL_MULTICHANNEL_ENABLE       0x36
#define AZ_REG_PIN_CONTROL_RESPONSE_HBR              0x38
#define AZ_REG_PIN_CONTROL_HOT_PLUG_CONTROL          0x54
#define AZ_REG_PIN_CONTROL_MULTICHANNEL_MODE          0x58

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

static const char *amdHdmiGpuFamilyName(UInt16 gpuDeviceId)
{
	return amdHdmiGpuFamilyNameForFamily(classifyAMDGPUDevice(gpuDeviceId));
}

static const char *azRegTableName(const AZRegOffsets *regs)
{
	if (regs == &kPolarisRegs)
		return "Polaris DCE11";
	if (regs == &kVega10Regs)
		return "Vega DCE12";
	return "Unknown";
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
	while ((child = OSDynamicCast(IOService, iter->getNextObject()))) {
		IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, child);
		if (!pci) continue;
		UInt16 vendor = pci->configRead16(kIOPCIConfigVendorID);
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

	/* Select register offset table based on the detected framebuffer GPU.
	 * This is diagnostic/read-only in the default safe mode.  HDMI policy must
	 * be based on the GPU function, not on the separate HDA audio function. */
	if (family == kVoodooHDAAMDGPUClassicPolaris)
		mRegs = &kPolarisRegs;
	else
		mRegs = &kVega10Regs;

	FBLOG("mapGPUMMIO: using %s register offsets for GPU device=%04x family=%s",
	      azRegTableName(reinterpret_cast<const AZRegOffsets *>(mRegs)), gpuDeviceId,
	      amdHdmiGpuFamilyNameForFamily(family));

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
#if !VOODOO_HDA_DEBUG_BUILD
	return;
#else
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
#endif
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

/* ---------- Auto-init: try all endpoints/DIGs ---------- */

void VoodooHDAFramebufferNotifier::initGPUAudioIfNeeded()
{
#if VHDA_AMD_FB_SAFE_MODE
	/*
	 * RX5xx/Polaris safety: do not map GPU MMIO or poke AZ/AFMT/DP_SEC
	 * registers from VoodooHDA. This avoids display-engine side effects that
	 * can cause black screen.
	 */
	mGPUAudioInitDone = true;
	FBLOG("initGPUAudio: skipped by RX5xx safe mode");
	return;
#else
	if (mGPUAudioInitDone) return;

	if (!mapGPUMMIO()) {
		FBLOG("initGPUAudio: cannot map GPU MMIO");
		return;
	}

	mGPUAudioInitDone = true;

	/* Read-only diagnostic dump of GPU AZ state.
	 * Audio is enabled by correct HDA verbs (ATI paired multichannel),
	 * not by GPU MMIO writes.  This dump is for debugging only. */
	FBLOG("initGPUAudio: === GPU AZ STATE (diagnostic) ===");
	dumpAZState();
#endif
}

void VoodooHDAFramebufferNotifier::diagnosticDumpGPUState(const char *reason, int cad, nid_t pinNid)
{
#if VHDA_AMD_FB_SAFE_MODE
	/* Diagnostic MMIO mapping is also disabled by default on RX5xx. */
	FBLOG("diagnosticDumpGPUState: skipped by RX5xx safe mode reason=%s cad=%d pin=%d",
	      reason ? reason : "unknown", cad, pinNid);
	return;
#else
	if (!mapGPUMMIO()) {
		FBLOG("diagnosticDumpGPUState: reason=%s cad=%d pin=%d map failed",
		      reason ? reason : "unknown", cad, pinNid);
		return;
	}

	FBLOG("diagnosticDumpGPUState: reason=%s cad=%d pin=%d regTable=%s",
	      reason ? reason : "unknown", cad, pinNid,
	      azRegTableName(reinterpret_cast<const AZRegOffsets *>(mRegs)));
	dumpAZState();
#endif
}
