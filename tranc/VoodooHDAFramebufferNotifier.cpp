#include "License.h"

#include "VoodooHDAFramebufferNotifier.h"
#include "VoodooHDADevice.h"
#include "VoodooHDAEngine.h"
#include "Verbs.h"

#include <IOKit/pci/IOPCIDevice.h>

OSDefineMetaClassAndStructors(VoodooHDAFramebufferNotifier, OSObject)

#undef super
#define super OSObject

#if VOODOO_HDA_DEBUG_BUILD
#define FBLOG_OWNER(owner, fmt, ...) do { \
	if ((owner) && (owner)->mDevice && (owner)->mDevice->mVerbose >= 1) \
		IOLog("VoodooHDA FB: " fmt "\n", ##__VA_ARGS__); \
} while (0)
#define FBLOG(fmt, ...) FBLOG_OWNER(this, fmt, ##__VA_ARGS__)
#else
#define FBLOG_OWNER(owner, fmt, ...) do {} while (0)
#define FBLOG(fmt, ...) do {} while (0)
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
	void *target, void *refCon, IOService *newService, IONotifier *notifier)
{
	VoodooHDAFramebufferNotifier *self = (VoodooHDAFramebufferNotifier *)target;
	if (!newService) return true;

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
	if (service) self->handleFramebufferTerminated(service);
	return true;
}

bool VoodooHDAFramebufferNotifier::displayMatchedHandler(
	void *target, void *refCon, IOService *newService, IONotifier *notifier)
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
		FBLOG_OWNER(self, "displayMatched: IODisplay for pin=%d, reading EDID", conn->mappedPinNid);

		if (conn->edidData) {
			IOFree(conn->edidData, conn->edidLen);
			conn->edidData = NULL;
		}
		conn->edidLen = edidProp->getLength();
		conn->edidData = (uint8_t *)IOMalloc(conn->edidLen);
		if (conn->edidData) {
			memcpy(conn->edidData, edidProp->getBytesNoCopy(), conn->edidLen);
			FBLOG_OWNER(self, "displayMatched: pin=%d got %d bytes EDID", conn->mappedPinNid, conn->edidLen);

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

				FBLOG_OWNER(self, "displayMatched: pin=%d spkalloc=0x%02x nsads=%d pipe enabled",
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
		disableAudioPipe(conn);
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
	void *messageArgument, vm_size_t argSize)
{
	VoodooHDAFramebufferNotifier *self = (VoodooHDAFramebufferNotifier *)target;
	FBConnectionState *conn = (FBConnectionState *)refCon;
	if (!self || !conn) return kIOReturnSuccess;

	/*
	 * Mirror AppleGFXHDADriver::message dispatch (decompile:0x18662) and
	 * AppleGFXHDAEngineOutputDP::handleEdidStateTransition (0x39d84).
	 * Apple distinguishes:
	 *   kIOMessageServicePropertyChange  — re-read EDID/ELD
	 *   kIOMessageDeviceWillPowerOff      — sink/display about to sleep
	 *   kIOMessageDeviceHasPoweredOn      — sink/display awake
	 *   kIOMessageServiceIsSuspended      — display engine suspended
	 *   kIOMessageServiceIsResumed        — display engine resumed
	 *   kIOMessageServiceIsTerminated     — connector torn down
	 *   0xe03f8003 (EdidInvalid)          — explicit sink-disconnect
	 *   0xe03f8004 (EdidValid)            — explicit sink-connect
	 *   0xe03f800a / 0xe03f800b           — connector state changed,
	 *                                       re-evaluate routing
	 *   0xe03f8011                        — consumed by upstream, no-op
	 *   0xe03f8012                        — multistream connector
	 *                                       reroute, treated as
	 *                                       Edid-invalidate-then-revalidate
	 * The 0xe03f80xx codes are private kIOFB messages from
	 * IOGraphicsFamily; their numeric values are stable across
	 * macOS releases (verified against KDK 26.3 AppleGFXHDA disasm
	 * at addresses listed above). */
#define VHDA_FB_MSG_EDID_INVALID    0xe03f8003U
#define VHDA_FB_MSG_EDID_VALID      0xe03f8004U
#define VHDA_FB_MSG_CONN_CHANGE_A   0xe03f800aU
#define VHDA_FB_MSG_CONN_CHANGE_B   0xe03f800bU
#define VHDA_FB_MSG_CONSUMED        0xe03f8011U
#define VHDA_FB_MSG_MST_REROUTE     0xe03f8012U

	switch (messageType) {

	case kIOMessageServicePropertyChange:
		/* Apple does this on every Edid event; we converge on a
		 * single handler that reads EDID, parses, builds ELD, and
		 * injects.  Equivalent to handleEdidStateTransition_EdidValid
		 * if EDID is now valid; if EDID is gone, readEDID returns
		 * false and we leave ELD cleared by the next stream setup. */
		FBLOG_OWNER(self, "interestHandler: PropertyChange fb=%p pin=%d",
		            provider, conn->mappedPinNid);
		IOLockLock(self->mLock);
		if (self->readEDID(conn) && self->parseEDIDAudio(conn)) {
			self->buildELDFromEDID(conn);
			conn->edidValid = true;
			conn->displayOnline = true;
			self->enableAudioPipe(conn);
			self->injectELDIntoWidget(conn);
			FBLOG_OWNER(self, "interestHandler: EDID updated, spkalloc=0x%02x nsads=%d",
			            conn->speakerAllocation, conn->numSADs);
		}
		IOLockUnlock(self->mLock);
		break;

	case VHDA_FB_MSG_EDID_VALID: {
		/* Apple's handleEdidStateTransition_EdidValid (decompile:0x3a0ba) */
		FBLOG_OWNER(self, "interestHandler: EdidValid fb=%p pin=%d",
		            provider, conn->mappedPinNid);
		IOLockLock(self->mLock);
		if (self->readEDID(conn) && self->parseEDIDAudio(conn)) {
			self->buildELDFromEDID(conn);
			conn->edidValid = true;
			conn->displayOnline = true;
			self->enableAudioPipe(conn);
			self->injectELDIntoWidget(conn);
		}
		IOLockUnlock(self->mLock);
		break;
	}

	case VHDA_FB_MSG_EDID_INVALID:
	case VHDA_FB_MSG_MST_REROUTE: {
		/* Apple's handleEdidStateTransition_EdidInvalid (decompile:
		 * 0x39f48) clears ELD and disables the audio pipe.  MST
		 * reroute is treated as invalidate then revalidate. */
		FBLOG_OWNER(self, "interestHandler: EdidInvalid/MST fb=%p pin=%d msg=0x%x",
		            provider, conn->mappedPinNid, messageType);
		IOLockLock(self->mLock);
		conn->edidValid = false;
		conn->displayOnline = false;
		self->clearWidgetELD(conn);
		self->disableAudioPipe(conn);
		IOLockUnlock(self->mLock);
		break;
	}

	case VHDA_FB_MSG_CONN_CHANGE_A:
	case VHDA_FB_MSG_CONN_CHANGE_B:
		/* Apple's vtable+0x1a0 path — re-evaluate path-set without
		 * tearing down ELD.  We just kick a re-read in case EDID
		 * changed. */
		FBLOG_OWNER(self, "interestHandler: ConnChange fb=%p pin=%d msg=0x%x",
		            provider, conn->mappedPinNid, messageType);
		IOLockLock(self->mLock);
		if (self->readEDID(conn) && self->parseEDIDAudio(conn)) {
			self->buildELDFromEDID(conn);
			self->injectELDIntoWidget(conn);
		}
		IOLockUnlock(self->mLock);
		break;

	case VHDA_FB_MSG_CONSUMED:
		/* Apple explicitly drops this message in the message handler
		 * (decompile:0x187cc shows fall-through to LAB_0001896f w/
		 * arg2=0).  Mirror by returning success without action. */
		FBLOG_OWNER(self, "interestHandler: ConsumedUpstream fb=%p pin=%d",
		            provider, conn->mappedPinNid);
		break;

	case kIOMessageDeviceWillPowerOff:
		/* Display about to sleep.  Apple's path-set powerState goes
		 * to 0; we just disable the audio pipe so the GPU can power-
		 * gate.  Stream stays "running" from coreaudiod's view; on
		 * resume the audio pipe is re-enabled by the next stream
		 * setup or by kIOMessageDeviceHasPoweredOn below. */
		FBLOG_OWNER(self, "interestHandler: DeviceWillPowerOff fb=%p pin=%d",
		            provider, conn->mappedPinNid);
		IOLockLock(self->mLock);
		self->disableAudioPipe(conn);
		IOLockUnlock(self->mLock);
		break;

	case kIOMessageDeviceHasPoweredOn:
		FBLOG_OWNER(self, "interestHandler: DeviceHasPoweredOn fb=%p pin=%d",
		            provider, conn->mappedPinNid);
		IOLockLock(self->mLock);
		self->enableAudioPipe(conn);
		if (conn->edidValid)
			self->injectELDIntoWidget(conn);
		IOLockUnlock(self->mLock);
		break;

	case kIOMessageServiceIsSuspended:
		FBLOG_OWNER(self, "interestHandler: ServiceIsSuspended fb=%p pin=%d",
		            provider, conn->mappedPinNid);
		break;

	case kIOMessageServiceIsResumed:
		FBLOG_OWNER(self, "interestHandler: ServiceIsResumed fb=%p pin=%d",
		            provider, conn->mappedPinNid);
		IOLockLock(self->mLock);
		self->enableAudioPipe(conn);
		IOLockUnlock(self->mLock);
		break;

	case kIOMessageServiceIsTerminated:
		FBLOG_OWNER(self, "interestHandler: service terminated fb=%p pin=%d",
		            provider, conn->mappedPinNid);
		/* handleFramebufferTerminated will tear down the slot. */
		break;

	default:
		/* Unknown / family-private — log at verbose only */
		if (self->mDevice && self->mDevice->mVerbose >= 2)
			IOLog("VoodooHDA FB: interestHandler unhandled msg=0x%x fb=%p pin=%d\n",
			      (unsigned)messageType, provider, conn->mappedPinNid);
		break;
	}

	return kIOReturnSuccess;
}

/* ---------- pin mapping ---------- */

void VoodooHDAFramebufferNotifier::mapConnectionToPin(FBConnectionState *conn, int connIndex)
{
	if (mATIPinCount > 0 && connIndex < mATIPinCount) {
		conn->mappedPinNid = mATIPinNids[connIndex];
		conn->mappedCodecCad = mATIPinCad;
		FBLOG("mapConnectionToPin: connIndex=%d -> pin nid=%d cad=%d",
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

	if (edidProp && edidProp->getLength() >= 128) {
		conn->edidLen = edidProp->getLength();
		conn->edidData = (uint8_t *)IOMalloc(conn->edidLen);
		if (conn->edidData) {
			memcpy(conn->edidData, edidProp->getBytesNoCopy(), conn->edidLen);
			FBLOG("readEDID: pin=%d got %d bytes", conn->mappedPinNid, conn->edidLen);
			return true;
		}
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

	int pos = 4;
	while (pos < dtdOffset && pos < 127) {
		int tag = (cea[pos] >> 5) & 0x07;
		int blockLen = cea[pos] & 0x1f;
		pos++;
		if (pos + blockLen > dtdOffset) break;

		if (tag == 1) {
			int nSADs = blockLen / 3;
			for (int i = 0; i < nSADs && conn->numSADs < VHDA_FB_MAX_SADS; i++) {
				int off = conn->numSADs * 3;
				conn->sads[off + 0] = cea[pos + i * 3 + 0];
				conn->sads[off + 1] = cea[pos + i * 3 + 1];
				conn->sads[off + 2] = cea[pos + i * 3 + 2];
				int fmt = (conn->sads[off + 0] >> 3) & 0x0f;
				int nch = (conn->sads[off + 0] & 0x07) + 1;
				FBLOG("parseEDIDAudio: pin=%d SAD[%d]: fmt=%d ch=%d rates=0x%02x bits=0x%02x",
				      conn->mappedPinNid, conn->numSADs, fmt, nch,
				      conn->sads[off + 1], conn->sads[off + 2]);
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
	if (!conn->audioPipeEnabled || !conn->framebuffer)
		return;
	IOFramebuffer *fb = reinterpret_cast<IOFramebuffer *>(conn->framebuffer);
	IOReturn ret = fb->setAttributeForConnectionExt(0, kConnectionEnableAudio, 0);
	FBLOG("disableAudioPipe: pin=%d setAttributeForConnectionExt(kConnectionEnableAudio=0)=%x",
	      conn->mappedPinNid, ret);
	(void)ret;
	conn->audioPipeEnabled = false;
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
			FBLOG("injectELD: nid=%d eld_len=%d spkalloc=0x%02x",
			      conn->mappedPinNid, w->eld_len,
			      (w->eld_len > 7) ? w->eld[7] : 0);
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
			if (nid == conn->mappedPinNid) continue; /* already injected by injectELDIntoWidget */

			UInt32 pinSense = mDevice->sendCommand(
				HDA_CMD_GET_PIN_SENSE(mATIPinCad, nid), mATIPinCad);
			if (!(pinSense & (1U << 31))) continue; /* no presence */

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
				FBLOG("injectELD(presence): nid=%d pinSense=0x%08x eld_len=%d",
				      nid, pinSense, w->eld_len);
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

bool VoodooHDAFramebufferNotifier::getAnyFramebufferELD(uint8_t **outELD, int *outLen)
{
	IOLockLock(mLock);
	for (int i = 0; i < mNumConnections; i++) {
		FBConnectionState *conn = &mConnections[i];
		if (conn->eld && conn->eldLen > 0) {
			*outELD = conn->eld;
			*outLen = conn->eldLen;
			IOLockUnlock(mLock);
			return true;
		}
	}
	IOLockUnlock(mLock);
	return false;
}

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
			return true;
		}
	}
	IOLockUnlock(mLock);
	return false;
}

void VoodooHDAFramebufferNotifier::ensureAudioPipeEnabled(int cad, nid_t pinNid)
{
	UInt16 diagFlags = mDevice ? mDevice->diagnosticFlagsForPin(cad, pinNid) : 0;
	bool skipAudioPipe = (diagFlags & kVoodooHDADiagSkipAudioPipe) != 0;

	IOLockLock(mLock);
	for (int i = 0; i < mNumConnections; i++) {
		FBConnectionState *conn = &mConnections[i];
		if (conn->mappedCodecCad == cad && conn->mappedPinNid == pinNid) {
			if (!conn->framebuffer) break;
			if (!conn->edidValid) {
				/* No EDID yet — GPU may be awake now, try again */
				if (readEDID(conn) && parseEDIDAudio(conn)) {
					buildELDFromEDID(conn);
					conn->edidValid = true;
					if (!skipAudioPipe)
						enableAudioPipe(conn);
					else
						FBLOG("ensureAudioPipeEnabled: pin=%d skipping framebuffer audio-pipe enable due to diagnostic flag",
						      pinNid);
					injectELDIntoWidget(conn);
				}
			} else if (!conn->audioPipeEnabled && !skipAudioPipe) {
				/* EDID is valid but pipe was disabled (e.g., by power-gating logic
				 * that incorrectly reads ATI presence=0 as disconnected).
				 * Re-enable it now that audio is about to stream. */
				enableAudioPipe(conn);
			} else if (skipAudioPipe) {
				FBLOG("ensureAudioPipeEnabled: pin=%d leaving framebuffer audio pipe disabled due to diagnostic flag",
				      pinNid);
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

	/* Select register offset table based on GPU generation.
	 * Polaris 30 (RX 590, 6fxx) shares the same AZ/AFMT layout as the rest
	 * of Polaris in the local reference headers; do not let it fall through
	 * into the Vega table by accident. */
	if ((gpuDeviceId & 0xFF00) == 0x6700 || /* Polaris 10/11/12 */
	    (gpuDeviceId & 0xFF00) == 0x6600 || /* Polaris 12/Lexa */
	    (gpuDeviceId & 0xFF00) == 0x6F00 || /* Polaris 30 */
	    (gpuDeviceId & 0xFF00) == 0x6900)   /* Tonga/Fiji */
		mRegs = &kPolarisRegs;
	else
		mRegs = &kVega10Regs;  /* Vega and newer — best guess */

	FBLOG("mapGPUMMIO: using %s register offsets for GPU device=%04x",
	      azRegTableName(reinterpret_cast<const AZRegOffsets *>(mRegs)), gpuDeviceId);

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
}

void VoodooHDAFramebufferNotifier::diagnosticDumpGPUState(const char *reason, int cad, nid_t pinNid)
{
	if (!mapGPUMMIO()) {
		FBLOG("diagnosticDumpGPUState: reason=%s cad=%d pin=%d map failed",
		      reason ? reason : "unknown", cad, pinNid);
		return;
	}

	FBLOG("diagnosticDumpGPUState: reason=%s cad=%d pin=%d regTable=%s",
	      reason ? reason : "unknown", cad, pinNid,
	      azRegTableName(reinterpret_cast<const AZRegOffsets *>(mRegs)));
	dumpAZState();
}
