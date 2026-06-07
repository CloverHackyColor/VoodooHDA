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
	if (mATIPinCount > 0 && connIndex < mATIPinCount) {
		conn->mappedPinNid = mATIPinNids[connIndex];
		conn->mappedCodecCad = mATIPinCad;
		FBLOG("mapConnectionToPin: connIndex=%d -> pin nid=%d cad=%d",
		      connIndex, conn->mappedPinNid, conn->mappedCodecCad);
	}
}

/* ---------- EDID reading ---------- */
const uint8_t sampleEDID[256] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x4c, 0x2d, 0x59, 0x06, 0x00, 0x00, 0x00, 0x00, 0x2d, 0x13, 0x01, 0x03, 0x80, 0x10, 0x09, 0x78, 0x0a, 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54, 0xbd, 0xef, 0x80, 0x71, 0x4f, 0x81, 0x00, 0x81, 0x40, 0x81, 0x80, 0x95, 0x00, 0x95, 0x0f, 0xb3, 0x00, 0xa9, 0x40, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c, 0x45, 0x00, 0xa0, 0x5a, 0x00, 0x00, 0x00, 0x1e, 0x66, 0x21, 0x50, 0xb0, 0x51, 0x00, 0x1b, 0x30, 0x40, 0x70, 0x36, 0x00, 0xa0, 0x5a, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x18, 0x4b, 0x1a, 0x51, 0x17, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x53, 0x41, 0x4d, 0x53, 0x55, 0x4e, 0x47, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0x3a, 0x02, 0x03, 0x23, 0xf1, 0x4b, 0x90, 0x1f, 0x04, 0x13, 0x05, 0x14, 0x03, 0x12, 0x20, 0x21, 0x22, 0x23, 0x09, 0x07, 0x07, 0x83, 0x01, 0x00, 0x00, 0xe2, 0x00, 0x0f, 0x67, 0x03, 0x0c, 0x00, 0x20, 0x00, 0xb8, 0x2d, 0x01, 0x1d, 0x00, 0x72, 0x51, 0xd0, 0x1e, 0x20, 0x6e, 0x28, 0x55, 0x00, 0xa0, 0x5a, 0x00, 0x00, 0x00, 0x1e, 0x01, 0x1d, 0x00, 0xbc, 0x52, 0xd0, 0x1e, 0x20, 0xb8, 0x28, 0x55, 0x40, 0xa0, 0x5a, 0x00, 0x00, 0x00, 0x1e, 0x01, 0x1d, 0x80, 0x18, 0x71, 0x1c, 0x16, 0x20, 0x58, 0x2c, 0x25, 0x00, 0xa0, 0x5a, 0x00, 0x00, 0x00, 0x9e, 0x01, 0x1d, 0x80, 0xd0, 0x72, 0x1c, 0x16, 0x20, 0x10, 0x2c, 0x25, 0x80, 0xa0, 0x5a, 0x00, 0x00, 0x00, 0x9e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfd};

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
  
  FBLOG("initGPUAudio: === Starting GENTLE GPU AZ Audio Init ===");
  const AZRegOffsets *r = (const AZRegOffsets *)mRegs;
  
  /* Сканируем DIG0..5 в поисках того, который УЖЕ включен видеодрайвером как HDMI.
   * Мы НЕ трогаем DIG_MODE или DIG_BE_EN, чтобы избежать черного экрана.
   * Мы только добавляем аудио-пакеты в существующий рабочий поток. */
  for (int digIndex = 0; digIndex < 6; digIndex++) {
    uint32_t base = r->digFeCntl0 + digIndex * r->digStride;
    uint32_t beCntlOff = base + (r->digBeCntl0 - r->digFeCntl0);
    uint32_t beEnOff = base + (r->digBeEnCntl0 - r->digFeCntl0);
    
    uint32_t beCntl = gpuRead32(beCntlOff);
    uint32_t beEn = gpuRead32(beEnOff);
    
    bool isDigEnabled = (beEn & 0x1) != 0;
    int digMode = (beCntl >> 16) & 0x7; // 1 = HDMI, 2 = DVI
    
    if (isDigEnabled && digMode == 1) {
      FBLOG("initGPUAudio: Found ACTIVE HDMI DIG%d (BE_EN=1, mode=1). Applying audio fix...", digIndex);
      
      int endpoint = digIndex;
      // Вызываем исправленную функцию выше (где DP_SEC включен безусловно)
      enableGPUAudioEngine(endpoint, digIndex, false, 0x01, 2);
      
      // Прерываем цикл, чтобы не задеть другие порты
      break;
    }
  }
  
  FBLOG("initGPUAudio: === GPU AZ STATE (Final) ===");
  dumpAZState();
}

/* ---------- EDID CEA audio parsing ---------- */
#if 0
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
  
  // Если EDID ровно 128 байт или нет расширений CEA, используем безопасный стерео-профиль.
  // Это предотвращает выход за границы массива и тихие сбои в старых версиях.
  if (numExtensions == 0 || conn->edidLen < 256) {
    conn->speakerAllocation = 0x01;
    conn->sads[0] = 0x09; /* LPCM, 2ch */
    conn->sads[1] = 0x07; /* 32/44.1/48 kHz */
    conn->sads[2] = 0x05; /* 16/24-bit */
    conn->numSADs = 1;
    FBLOG("parseEDIDAudio: pin=%d using SAFE DEFAULT stereo LPCM (no CEA or short EDID)", conn->mappedPinNid);
    return true;
  }
  
  uint8_t *cea = &conn->edidData[128];
  if (cea[0] != 0x02) {
    FBLOG("parseEDIDAudio: pin=%d CEA tag=0x%02x (expected 0x02), falling back to default", conn->mappedPinNid, cea[0]);
    // Не возвращаем false! Мы используем дефолт, чтобы звук всё равно работал.
    conn->speakerAllocation = 0x01;
    conn->sads[0] = 0x09;
    conn->sads[1] = 0x07;
    conn->sads[2] = 0x01;
    conn->numSADs = 1;
    return true;
  }
  
  int dtdOffset = cea[2];
  bool basicAudio = (cea[3] & 0x40) != 0;
  FBLOG("parseEDIDAudio: pin=%d CEA rev=%d dtdOffset=%d basicAudio=%d", conn->mappedPinNid, cea[1], dtdOffset, basicAudio);
  
  if (!basicAudio) {
    FBLOG("parseEDIDAudio: pin=%d no basic audio support, falling back to default", conn->mappedPinNid);
    conn->speakerAllocation = 0x01;
    conn->sads[0] = 0x09;
    conn->sads[1] = 0x07;
    conn->sads[2] = 0x01;
    conn->numSADs = 1;
    return true;
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
              conn->mappedPinNid, conn->numSADs, fmt, nch, conn->sads[off + 1], conn->sads[off + 2]);
        conn->numSADs++;
      }
    } else if (tag == 3) {
      FBLOG("parseEDIDAudio: pin=%d Speaker Allocation: 0x%02x", conn->mappedPinNid, cea[pos]);
    }
    pos += blockLen;
  }
  
  if (conn->numSADs > 0 && conn->speakerAllocation == 0) {
    conn->speakerAllocation = 0x01;
  }
  
  FBLOG("parseEDIDAudio: pin=%d result: spkalloc=0x%02x nsads=%d", conn->mappedPinNid, conn->speakerAllocation, conn->numSADs);
  return (conn->numSADs > 0);
}
#else
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
  
  // Если не нашли speaker allocation, но есть SAD, используем значение по умолчанию
  if (conn->speakerAllocation == 0 && conn->numSADs > 0) {
    conn->speakerAllocation = 0x01;  // FL/FR only
    FBLOG("parseEDIDAudio: pin=%d using default spkalloc=0x01", conn->mappedPinNid);
  }
  
  FBLOG("parseEDIDAudio: pin=%d result: spkalloc=0x%02x nsads=%d",
        conn->mappedPinNid, conn->speakerAllocation, conn->numSADs);
  return (conn->numSADs > 0);
}
#endif

/* ---------- ELD construction ---------- */
#if 0
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
#else
void VoodooHDAFramebufferNotifier::buildELDFromEDID(FBConnectionState *conn)
{
  if (conn->eld) {
    IOFree(conn->eld, conn->eldLen);
    conn->eld = NULL;
    conn->eldLen = 0;
  }
  
  // mnl = monitor name length (0)
  int mnl = 0;
  int baselineLen = 4 + mnl + conn->numSADs * 3;
  int totalLen = 4 + baselineLen;
  
  conn->eld = (uint8_t *)IOMalloc(totalLen);
  if (!conn->eld) return;
  conn->eldLen = totalLen;
  bzero(conn->eld, totalLen);
  
  // ELD version 2
  conn->eld[0] = 0x02 << 3;
  // ELD size in DWORDs
  conn->eld[2] = baselineLen / 4;
  // Number of SADs << 4 | mnl
  conn->eld[4] = (conn->numSADs << 4) | mnl;
  // Connection type: 0x00 = HDMI, 0x04 = DP
  conn->eld[5] = 0x00;  // HDMI
                        // Audio sync delay (0)
  conn->eld[6] = 0;
  // Speaker allocation
  conn->eld[7] = conn->speakerAllocation;
  
  // Copy SADs
  for (int i = 0; i < conn->numSADs * 3; i++) {
    conn->eld[8 + i] = conn->sads[i];
  }
  
  FBLOG("buildELD: pin=%d eldLen=%d spkalloc=0x%02x nsads=%d",
        conn->mappedPinNid, totalLen, conn->speakerAllocation, conn->numSADs);
  
  // DEBUG: dump first few bytes
  FBLOG("buildELD: eld[0]=0x%02x eld[2]=0x%02x eld[4]=0x%02x eld[5]=0x%02x eld[7]=0x%02x",
        conn->eld[0], conn->eld[2], conn->eld[4], conn->eld[5], conn->eld[7]);
}
#endif


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
			FBLOG("injectELD: nid=%d eld_len=%d spkalloc=0x%02x",
			      conn->mappedPinNid, w->eld_len,
			      (w->eld_len > 7) ? w->eld[7] : 0);
      // КРИТИЧЕСКИ ВАЖНО: Если ELD был отложен, обновляем его сейчас
      if (w->needELDUpdate) {
        w->needELDUpdate = false;
        // Вызываем hdaa_eld_handler для обновления всех связанных настроек
        //mDevice->hdaa_eld_handler(w);
        FBLOG("injectELD: triggered hdaa_eld_handler for delayed nid=%d", conn->mappedPinNid);
      }
		}
		return;
	}
}
#if 0
void VoodooHDAFramebufferNotifier::injectELDIntoAllPinsWithPresence(FBConnectionState *conn)
{
	if (!mDevice || !conn->eld || conn->eldLen == 0 || mATIPinCad < 0)
		return;

	Codec *codec = mDevice->mCodecs[mATIPinCad];
	if (!codec) return;
  
  // Get the best speaker allocation from this connection
  uint8_t bestSpkalloc = (conn->eldLen > 7) ? conn->eld[7] : 0;
  
  // Also check if there are any connections with better speaker allocation
  IOLockLock(mLock);
  for (int i = 0; i < mNumConnections; i++) {
    FBConnectionState *c = &mConnections[i];
    if (c->edidValid && c->eld && c->eldLen > 0) {
      uint8_t spkalloc = (c->eldLen > 7) ? c->eld[7] : 0;
      if (spkalloc > bestSpkalloc) {
        bestSpkalloc = spkalloc;
        conn = c;  // Use the connection with better spkalloc
      }
    }
  }
  IOLockUnlock(mLock);
  
  FBLOG("injectELDIntoAllPinsWithPresence: using spkalloc=0x%02x eld_len=%d",
        bestSpkalloc, conn->eldLen);

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

      // Only update if new ELD has better speaker allocation
      uint8_t currentSpkalloc = (w->eld && w->eld_len > 7) ? w->eld[7] : 0;
      
      if (currentSpkalloc < bestSpkalloc) {
        if (w->eld) {
          VoodooHDADevice::freeMem(w->eld);
          w->eld = NULL;
          w->eld_len = 0;
        }
        w->eld = (uint8_t *)VoodooHDADevice::allocMem(conn->eldLen);
        if (w->eld) {
          memcpy(w->eld, conn->eld, conn->eldLen);
          w->eld_len = conn->eldLen;
          FBLOG("injectELD(presence): nid=%d spkalloc=0x%02x (was 0x%02x)",
                nid, bestSpkalloc, currentSpkalloc);
        }
      }
		}
		return;
	}
}
#else
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
        FBLOG("injectELD(presence): nid=%d spkalloc=0x%02x",
              nid, (w->eld_len > 7) ? w->eld[7] : 0);
      }
    }
    return;
  }
}
#endif

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
            cad, pinNid, conn->eldLen, conn->eldLen > 7 ? conn->eld[7] : 0);

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

/* ---------- Enable GPU audio engine for a DP/HDMI output ---------- */
bool VoodooHDAFramebufferNotifier::enableGPUAudioEngine(
                                                        int endpoint, int digIndex, bool isDP,
                                                        uint8_t speakerAlloc, int numChannels)
{
  if (!mGPUMMIO) return false;
  FBLOG("enableGPUAudio: ep=%d dig=%d isDP=%d spkalloc=0x%02x ch=%d",
        endpoint, digIndex, isDP, speakerAlloc, numChannels);
  
  const AZRegOffsets *r = (const AZRegOffsets *)mRegs;
  
  /* Phase 1: Configure Azalia endpoint */
  uint32_t hpc = azEndpointRead(endpoint, AZ_REG_PIN_CONTROL_HOT_PLUG_CONTROL);
  azEndpointWrite(endpoint, AZ_REG_PIN_CONTROL_HOT_PLUG_CONTROL,
                  hpc | AZ_HPC_CLOCK_GATING_DISABLE);
  
  uint32_t cs = azEndpointRead(endpoint, AZ_REG_PIN_CONTROL_CHANNEL_SPEAKER);
  cs &= ~(AZ_CS_SPEAKER_ALLOC_MASK | AZ_CS_HDMI_CONNECTION | AZ_CS_DP_CONNECTION);
  cs |= (speakerAlloc & AZ_CS_SPEAKER_ALLOC_MASK);
  if (isDP)
    cs |= AZ_CS_DP_CONNECTION;
  else
    cs |= AZ_CS_HDMI_CONNECTION;
  azEndpointWrite(endpoint, AZ_REG_PIN_CONTROL_CHANNEL_SPEAKER, cs);
  
  uint32_t desc0 = ((numChannels - 1) & 0x07) |
  (0x07 << 8) |    /* 32, 44.1, 48 kHz */
  (0x07 << 16) |   /* 16, 20, 24-bit */
  (0x07 << 24);    /* stereo frequencies */
  azEndpointWrite(endpoint, AZ_REG_PIN_CONTROL_AUDIO_DESCRIPTOR(0), desc0);
  
  for (int i = 1; i < 14; i++)
    azEndpointWrite(endpoint, AZ_REG_PIN_CONTROL_AUDIO_DESCRIPTOR(i), 0);
  
  hpc = azEndpointRead(endpoint, AZ_REG_PIN_CONTROL_HOT_PLUG_CONTROL);
  hpc &= ~AZ_HPC_CLOCK_GATING_DISABLE;
  azEndpointWrite(endpoint, AZ_REG_PIN_CONTROL_HOT_PLUG_CONTROL, hpc);
  
  /* Phase 2: Map DIG encoder to audio endpoint */
  gpuWrite32(r->afmtSrcCtl0 + digIndex * r->digStride, endpoint & 0x07);
  
  uint32_t pktCtl2 = gpuRead32(r->afmtPktCtl2_0 + digIndex * r->digStride);
  pktCtl2 &= ~0xFF00u;
  pktCtl2 |= (0x03 << 8);  /* enable 2 channels (stereo) */
  gpuWrite32(r->afmtPktCtl2_0 + digIndex * r->digStride, pktCtl2);
  
  /* Phase 3: Setup DTO clock (STRICTLY for DP to avoid HDMI jitter) */
  if (isDP) {
    uint32_t dtoSrc = gpuRead32(r->dccgDtoSource);
    dtoSrc &= ~0x30u;
    dtoSrc |= (1u << 4);
    gpuWrite32(r->dccgDtoSource, dtoSrc);
    gpuWrite32(r->dccgDto1Module, 1000000);
    gpuWrite32(r->dccgDto1Phase, 240000);
  }
  
  /* Phase 4: Enable AZ audio */
  hpc = azEndpointRead(endpoint, AZ_REG_PIN_CONTROL_HOT_PLUG_CONTROL);
  azEndpointWrite(endpoint, AZ_REG_PIN_CONTROL_HOT_PLUG_CONTROL,
                  hpc | AZ_HPC_CLOCK_GATING_DISABLE | AZ_HPC_AUDIO_ENABLED);
  azEndpointWrite(endpoint, AZ_REG_PIN_CONTROL_HOT_PLUG_CONTROL,
                  (hpc & ~AZ_HPC_CLOCK_GATING_DISABLE) | AZ_HPC_AUDIO_ENABLED);
  FBLOG("enableGPUAudio: AZ AUDIO_ENABLED set for ep=%d", endpoint);
  
  /* Phase 5: Enable DIG audio formatter and secondary packets */
  /* 5a. Enable AFMT clock */
  gpuWrite32(r->afmtCntl0 + digIndex * r->digStride,
             gpuRead32(r->afmtCntl0 + digIndex * r->digStride) | AFMT_AUDIO_CLOCK_EN);
  
  /* 5b. Setup secondary packets (REQUIRED FOR BOTH DP AND HDMI ON AMD) */
  /* УБРАН if (isDP) - это критически важно для устранения хрипов на HDMI! */
  gpuWrite32(r->dpSecAudN0 + digIndex * r->digStride, 0x8000);
  uint32_t timestamp = gpuRead32(r->dpSecTimestamp0 + digIndex * r->digStride);
  timestamp &= ~0x01u;
  gpuWrite32(r->dpSecTimestamp0 + digIndex * r->digStride, timestamp);
  
  uint32_t dpSec = gpuRead32(r->dpSecCntl0 + digIndex * r->digStride);
  dpSec |= DP_SEC_ASP_ENABLE | DP_SEC_ATP_ENABLE | DP_SEC_AIP_ENABLE;
  gpuWrite32(r->dpSecCntl0 + digIndex * r->digStride, dpSec);
  
  /* Master enable LAST */
  dpSec |= DP_SEC_STREAM_ENABLE;
  gpuWrite32(r->dpSecCntl0 + digIndex * r->digStride, dpSec);
  
  /* 5c. Unmute audio */
  uint32_t pktCtl = gpuRead32(r->afmtPktCtl0 + digIndex * r->digStride);
  pktCtl |= AFMT_AUDIO_SAMPLE_SEND;
  gpuWrite32(r->afmtPktCtl0 + digIndex * r->digStride, pktCtl);
  
  FBLOG("enableGPUAudio: SUCCESS! DIG%d DP_SEC + AFMT enabled", digIndex);
  return true;
}

/* ---------- Diagnostic dump of AZ state ---------- */
void VoodooHDAFramebufferNotifier::dumpAZState()
{
  if (!mGPUMMIO) return;
  for (int ep = 0; ep < 7; ep++) {
    uint32_t hpc = azEndpointRead(ep, AZ_REG_PIN_CONTROL_HOT_PLUG_CONTROL);
    uint32_t cs = azEndpointRead(ep, AZ_REG_PIN_CONTROL_CHANNEL_SPEAKER);
    uint32_t desc0 = azEndpointRead(ep, AZ_REG_PIN_CONTROL_AUDIO_DESCRIPTOR(0));
    uint32_t hbr = azEndpointRead(ep, AZ_REG_PIN_CONTROL_RESPONSE_HBR);
    FBLOG("AZ EP%d: HPC=0x%08x CS=0x%08x DESC0=0x%08x HBR=0x%08x audioEnabled=%d",
          ep, hpc, cs, desc0, hbr, (hpc & AZ_HPC_AUDIO_ENABLED) ? 1 : 0);
  }
  const AZRegOffsets *r = (const AZRegOffsets *)mRegs;
  static const uint32_t digExtraBase[] = { 0x5400, 0x5600, 0x5700 };
  int numDigs = 9;
  for (int d = 0; d < numDigs; d++) {
    uint32_t base;
    if (d < 6)
      base = r->digFeCntl0 + d * r->digStride;
    else
      base = DW2B(digExtraBase[d - 6]);
    
    uint32_t feCntl = gpuRead32(base);
    uint32_t beCntlOff = base + (r->digBeCntl0 - r->digFeCntl0);
    uint32_t beCntl = gpuRead32(beCntlOff);
    uint32_t dpLinkOff = base + (r->dpLinkCntl0 - r->digFeCntl0);
    uint32_t afmtSrcOff = base + (r->afmtSrcCtl0 - r->digFeCntl0);
    uint32_t afmtCntlOff = base + (r->afmtCntl0 - r->digFeCntl0);
    uint32_t dpSecOff = base + (r->dpSecCntl0 - r->digFeCntl0);
    uint32_t pktCtlOff = base + (r->afmtPktCtl0 - r->digFeCntl0);
    
    if (afmtCntlOff + 4 > mGPUMMIOSize) continue;
    
    uint32_t dpLink = gpuRead32(dpLinkOff);
    uint32_t afmtSrc = gpuRead32(afmtSrcOff);
    uint32_t afmtCntl = gpuRead32(afmtCntlOff);
    uint32_t dpSec = gpuRead32(dpSecOff);
    uint32_t pktCtl = gpuRead32(pktCtlOff);
    
    bool digEnabled = (beCntl & 0x1) != 0;
    int digMode = (beCntl >> 16) & 0x7;
    bool feStarted = (feCntl & (1 << 10)) != 0;
    bool dpTrained = (dpLink & 0x10) != 0;
    bool dpActive = (dpLink & 0x100) != 0;
    
    FBLOG("AZ DIG%d: FE=0x%08x BE=0x%08x BE_EN=%d mode=%d FE_START=%d dpTrained=%d dpActive=%d | AFMT_SRC=0x%x AFMT_CNTL=0x%x DP_SEC=0x%x PKT=0x%x",
          d, feCntl, beCntl, digEnabled, digMode, feStarted, dpTrained, dpActive,
          afmtSrc, afmtCntl, dpSec, pktCtl);
  }
  uint32_t dtoSrc = gpuRead32(r->dccgDtoSource);
  uint32_t dto1Phase = gpuRead32(r->dccgDto1Phase);
  uint32_t dto1Mod = gpuRead32(r->dccgDto1Module);
  FBLOG("AZ DCCG: DTO_SRC=0x%08x DTO1_PHASE=0x%08x DTO1_MOD=0x%08x",
        dtoSrc, dto1Phase, dto1Mod);
}
