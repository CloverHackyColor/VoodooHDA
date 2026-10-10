#include "VoodooHDADevice.h"

#define HAVE_AAPL_DISPLAYTYPE 1U
#define HAVE_CONNECTOR_TYPE 2U
#define HAVE_AV_SIGNAL_TYPE 4U
#define HAVE_AUDIO_CODEC_INFO 8U
#define HAVE_PORT_NUMBER 16U
#define HAVE_ALL 31U

struct FramebufferTracker
{
	IOFramebuffer* fb;
	IOService* ad;
	IOLock* lock;
	IONotifier* notifiers[2];
	struct FramebufferTracker* next;
	int index;
	uint32_t have_flags;
	union {
		uint32_t props[5];
		struct {
			uint32_t aapl_displaypipe;
			uint32_t connector_type;
			uint32_t av_signal_type;
			uint32_t audio_codec_info;
			uint32_t port_number;
		} p;
	} u;
#ifdef LINK_IOGRAPHICSFAMILY
	bool isConnected;
#endif
	bool basicAudio;
	char monitorName[14];
	uint8_t speakerAllocation;
	uint8_t numSADs;
	uint8_t sads[30];
	uint8_t logging;
};

struct CentralTracker
{
	struct FramebufferTracker* first;
	IOLock* lock;
	IONotifier* notifiers[2];
	int fbCounter;
	uint16_t vendorId;
	uint8_t busNumber;
	uint8_t logging;
};

static uintptr_t chop_ptr(void const* p)
{
	uintptr_t v = reinterpret_cast<uintptr_t>(p);
	return v & 0xFFFFFFFFFFULL;
}

static char const * const propNames[] = {
	"AAPL,DisplayPipe",
	"connector-type",
	"av-signal-type",
	"audio-codec-info",
	"port-number"
};

static void scanFramebufferProps(struct FramebufferTracker* fbt)
{
	if (!fbt || !fbt->fb || (fbt->have_flags & HAVE_ALL) == HAVE_ALL)
		return;
	for (int i = 0; i < 4; ++i) {
		if (fbt->have_flags & (1U << i))
			continue;
		OSData* osd = OSDynamicCast(OSData, fbt->fb->getProperty(propNames[i]));
		if (osd) {
			uint32_t d = 0U;
			unsigned l = osd->getLength();
			if (l >= 4U)
				bcopy(osd->getBytesNoCopy(), &d, 4U);
			else
				bcopy(osd->getBytesNoCopy(), &d, l);
			fbt->u.props[i] = d;
			fbt->have_flags |= (1U << i);
		}
	}
	if (fbt->have_flags & (1U << 4))
		return;
	OSNumber* number = OSDynamicCast(OSNumber, fbt->fb->getProperty(propNames[4]));
	if (number) {
		fbt->u.props[4] = number->unsigned32BitValue();
		fbt->have_flags |= (1U << 4);
	}
}

static void printFramebufferProps(struct FramebufferTracker* fbt)
{
	if (!fbt || !fbt->logging)
		return;
	for (int i = 0; i < 4; ++i) {
		if (fbt->have_flags & (1U << i))
			IOLog("VoodooHDA DBG:   Prop %s value 0x%x\n", propNames[i], fbt->u.props[i]);
	}
	if (fbt->have_flags & (1U << 4))
		IOLog("VoodooHDA DBG:   Prop %s value %u\n", propNames[4], fbt->u.props[4]);
}

static bool matchCadNid(struct FramebufferTracker* fbt, int cad, int pinNid)
{
	if (!fbt)
		return false;
	if (fbt->have_flags & HAVE_AUDIO_CODEC_INFO) {
		uint32_t acf = fbt->u.p.audio_codec_info;
		uint8_t x_cad = acf & 255U;
		uint8_t x_nid = (acf >> 16) & 255U;
		return cad == x_cad && pinNid == x_nid;
	}
	if (fbt->have_flags & HAVE_PORT_NUMBER)
		return pinNid == static_cast<int>(fbt->u.p.port_number);
	return false;
}

#ifdef LINK_IOGRAPHICSFAMILY
static void updateFramebufferConnected(struct FramebufferTracker* fbt)
{
	uintptr_t attr;

	if (!fbt || !fbt->fb)
		return;
	fbt->isConnected = (fbt->fb->getAttributeForConnectionExt(0, kConnectionEnable, &attr) == kIOReturnSuccess) && (attr);
}
#endif

#ifdef LINK_IOGRAPHICSFAMILY
static IOReturn framebufferNotificationHandler(OSObject* obj, void* ref, IOFramebuffer* framebuffer, IOIndex event, void* info)
{
	struct FramebufferTracker* fbt;
	const char* name = static_cast<const char*>(NULL);
	const char* tail = static_cast<const char*>(NULL);
	IONotifier* n = static_cast<IONotifier*>(NULL);
	uint32_t prev_have_flags;
	char strbuf[16];

	fbt = static_cast<struct FramebufferTracker*>(ref);
	if (ref && !(fbt->fb))
		return kIOReturnSuccess;
	switch (event) {
			case kIOFBNotifyWillNotify:
			case kIOFBNotifyDidNotify:
			case kIOFBNotifyWSAAWillEnterDefer:
			case kIOFBNotifyWSAAWillExitDefer:
			case kIOFBNotifyWSAADidEnterDefer:
			case kIOFBNotifyWSAADidExitDefer:
				return kIOReturnSuccess;
	}
	if (!ref || fbt->fb != framebuffer || fbt->logging)
		switch (event) {
			case kIOFBNotifyDisplayModeWillChange:
				name = "DisplayModeWillChange";
				break;
			case kIOFBNotifyDisplayModeDidChange:
				name = "DisplayModeDidChange";
				break;
			case kIOFBNotifyOnlineChange:
				name = "OnlineChange";
				break;
			default:
				snprintf(&strbuf[0], sizeof strbuf, "%d", event);
				name = &strbuf[0];
				break;
		}
	if (!ref || fbt->fb != framebuffer) {
		IOLog("VoodooHDA DBG: framebufferNotificationHandler obj 0x%lx, ref NULL, framebuffer 0x%lx, event %s, info %lu\n",
		      chop_ptr(obj),
		      chop_ptr(framebuffer),
		      name,
		      reinterpret_cast<uintptr_t>(info));
		return kIOReturnSuccess;
	}
	IOLockLock(fbt->lock);
	if (!fbt->fb) { // must check this again while locked for termination signalling
		IOLockUnlock(fbt->lock);
		return kIOReturnSuccess;
	}
	if (fbt->logging) {
		updateFramebufferConnected(fbt);
		tail = (fbt->isConnected ? " (Enabled)" : " (Disabled)");
		IOLog("VoodooHDA DBG: framebufferNotificationHandler obj 0x%lx, framebuffer %d, event %s, info %lu%s\n",
			  chop_ptr(obj),
			  fbt->index,
			  name,
			  reinterpret_cast<uintptr_t>(info),
			  tail);
	}
	prev_have_flags = fbt->have_flags;
	scanFramebufferProps(fbt);
	if (prev_have_flags != fbt->have_flags)
		printFramebufferProps(fbt);
	if ((fbt->have_flags & HAVE_ALL) == HAVE_ALL) {
		n = fbt->notifiers[0];
		fbt->notifiers[0] = static_cast<IONotifier*>(NULL);
	}
	IOLockUnlock(fbt->lock);
	if (n)
		n->remove();
	return kIOReturnSuccess;
}
#endif

static IOReturn framebufferInterestHandler(void* target, void* refCon, UInt32 messageType, IOService* provider, void* messageArgument, vm_size_t argSize)
{
	struct FramebufferTracker* fbt;
	const char* name = static_cast<const char*>(NULL);
	const char* tail = static_cast<const char*>(NULL);
	IONotifier* n = static_cast<IONotifier*>(NULL);
	uint32_t prev_have_flags;
	char strbuf[16];

	fbt = static_cast<struct FramebufferTracker*>(target);
	if (target && !(fbt->fb))
		return kIOReturnSuccess;

	if (!target || fbt->fb != provider || fbt->logging)
		switch (messageType) {
			case kIOMessageDeviceHasPoweredOn:
				name = "DeviceHasPoweredOn";
				break;
			default:
				snprintf(&strbuf[0], sizeof strbuf, "0x%x", messageType);
				name = &strbuf[0];
				break;
		}
	if (!target || fbt->fb != provider) {
		IOLog("VoodooHDA DBG: framebufferInterestHandler target 0x%lx, framebuffer %lu, messageType %s, provider 0x%lx, messageArgument 0x%lx, %lu\n",
			  chop_ptr(target),
			  reinterpret_cast<uintptr_t>(refCon),
			  name,
			  chop_ptr(provider),
			  chop_ptr(messageArgument),
			  argSize);
		return kIOReturnSuccess;
	}
	IOLockLock(fbt->lock);
	if (!fbt->fb) { // must check this again while locked for termination signalling
		IOLockUnlock(fbt->lock);
		return kIOReturnSuccess;
	}
	if (fbt->logging) {
#ifdef LINK_IOGRAPHICSFAMILY
		updateFramebufferConnected(fbt);
		tail = (fbt->isConnected ? " (Enabled)" : " (Disabled)");
#else
		tail = "";
#endif
		IOLog("VoodooHDA DBG: framebufferInterestHandler target 0x%lx, framebuffer %lu, messageType %s, provider 0x%lx, messageArgument 0x%lx, %lu%s\n",
			  chop_ptr(target),
			  reinterpret_cast<uintptr_t>(refCon),
			  name,
			  chop_ptr(provider),
			  chop_ptr(messageArgument),
			  argSize,
			  tail);
	}
	prev_have_flags = fbt->have_flags;
	scanFramebufferProps(fbt);
	if (prev_have_flags != fbt->have_flags)
		printFramebufferProps(fbt);
	if ((fbt->have_flags & HAVE_ALL) == HAVE_ALL) {
		n = fbt->notifiers[1];
		fbt->notifiers[1] = static_cast<IONotifier*>(NULL);
	}
	IOLockUnlock(fbt->lock);
	if (n)
		n->remove();
	return kIOReturnSuccess;
}

static void initialScanFramebuffer(struct FramebufferTracker* fbt)
{
	if (!fbt | !fbt->fb)
		return;
#ifdef LINK_IOGRAPHICSFAMILY
	if (fbt->logging) {
		updateFramebufferConnected(fbt);
		IOLog("VoodooHDA DBG:   Framebuffer is %s\n", fbt->isConnected ? "Enabled" : "Disabled");
	}
#endif
	scanFramebufferProps(fbt);
	printFramebufferProps(fbt);
	if ((fbt->have_flags & HAVE_AAPL_DISPLAYTYPE) && (fbt->u.p.aapl_displaypipe & 0xFFFFU) == 0xFFFFU)
		return;
#ifdef LINK_IOGRAPHICSFAMILY
	if (fbt->logging)
		IOLog("VoodooHDA DBG: Installing Framebuffer Notification Begin\n");
	fbt->notifiers[0] = fbt->fb->addFramebufferNotification(framebufferNotificationHandler, static_cast<OSObject*>(NULL), fbt);
	if (fbt->logging)
		IOLog("VoodooHDA DBG: Installing Framebuffer Notification End\n");
#endif
	if (fbt->logging)
		IOLog("VoodooHDA DBG: Installing Framebuffer Interest Handler Begin\n");
	fbt->notifiers[1] = fbt->fb->registerInterest(gIOGeneralInterest, framebufferInterestHandler, fbt, reinterpret_cast<void*>(static_cast<uintptr_t>(fbt->index)));
	if (fbt->logging)
		IOLog("VoodooHDA DBG: Installing Framebuffer Interest Handler End\n");
}

static IOFramebuffer* searchForFramebuffer(struct CentralTracker* ct, IOService* newService)
{
	IOFramebuffer* nullRet = static_cast<IOFramebuffer*>(NULL);
	IOFramebuffer* fb = nullRet;
	if (!ct || !newService)
		return nullRet;
	IOPCIDevice* pcid = static_cast<IOPCIDevice*>(NULL);
	IORegistryEntry* ancestor = newService;
	while ((ancestor = ancestor->getParentEntry(gIOServicePlane))) {
		if (!fb) {
#ifdef LINK_IOGRAPHICSFAMILY
			fb = OSDynamicCast(IOFramebuffer, ancestor);
#else
			fb = static_cast<IOFramebuffer*>(ancestor->metaCast("IOFramebuffer"));
#endif
			if (fb && pcid)
				break;
			if (fb)
				continue;
		}
		if (!pcid) {
			pcid = OSDynamicCast(IOPCIDevice, ancestor);
			if (pcid && fb)
				break;
		}
	}
	if ((!fb) || (!pcid))
		return nullRet;
	if (ct->vendorId != pcid->extendedConfigRead16(kIOPCIConfigVendorID))
		return nullRet;
	if (ct->busNumber != pcid->getBusNumber())
		return nullRet;
	return fb;
}

static void parseEdid(struct FramebufferTracker* fbt, OSData* osd)
{
	unsigned edidLen, bound;
	uint8_t const* edidPtr;
	uint8_t const* cta;
	int numExtensions, dtdOffset, pos;

	if (!fbt || !osd)
		return;
	edidLen = osd->getLength();
	edidPtr = static_cast<uint8_t const*>(osd->getBytesNoCopy());
	if (!edidPtr)
		return;
	bound = (edidLen < 126U ? edidLen : 126U);
	for (unsigned i = 54U; i + 18U <= bound; i += 18U)
		if (edidPtr[i] == 0U && edidPtr[i + 1U] == 0U && edidPtr[i + 2U] == 0U && edidPtr[i + 3U] == 0xFCU && edidPtr[i + 4U] == 0U) {
			for (unsigned j = 0U; j < 13U; ++j) {
				char ch = static_cast<char>(edidPtr[i + 5U + j]);
				if (ch == '\n')
					break;
				fbt->monitorName[j] = ch;
			}
			break;
		}
	if (edidLen >= 127U)
		numExtensions = static_cast<int>(edidPtr[126]);
	else
		numExtensions = 0;
	if (edidLen >= 130U && !(edidPtr[128] == 2U && edidPtr[129] == 3U))  // code is for CTA extension revision 3
		numExtensions = 0;
	if (!numExtensions || edidLen < 256U) {
		fbt->speakerAllocation = 1U; // FL/FR
		fbt->sads[0] = 9U; // LPCM stereo
		fbt->sads[1] = 7U; // 48, 44.1 and 32 KHz
		fbt->sads[2] = 5U; // 24, 16 bits
		fbt->numSADs = 1U;
		return;
	}
	cta = edidPtr + 128;
	dtdOffset = static_cast<int>(cta[2]);
	if (dtdOffset > 127)
		dtdOffset = 127;
	fbt->basicAudio = ((cta[3] & 0x40U) != 0U);
	pos = 4;
	while (pos < dtdOffset) {
		uint8_t b = cta[pos];
		uint8_t tag = (b >> 5) & 7U;
		int blockLen = static_cast<int>(b & 31U);
		++pos;
		if (pos + blockLen > dtdOffset)
			break;
		if (tag == 1U) { // Audio Data Block
			int nSADs = blockLen / 3;
			if (nSADs > 10)
				nSADs = 10;
			bcopy(&cta[pos], &fbt->sads[0], static_cast<size_t>(nSADs * 3));
			fbt->numSADs = static_cast<uint8_t>(nSADs);
		} else if (tag == 4U) // Speaker Allocation
			fbt->speakerAllocation = (cta[pos] & 127U);
		pos += blockLen;
	}
}

static bool displayMatchedNotificationHandler(void* target, void* refCon, IOService* newService, IONotifier* notifier)
{
	struct CentralTracker* ct;

	ct = static_cast<struct CentralTracker*>(target);
	if (!target || ct->logging)
		IOLog("VoodooHDA DBG: displayMatchedNotificationHandler target 0x%lx, refCon 0x%lx, newService 0x%lx, notifier 0x%lx\n",
			  chop_ptr(target),
			  chop_ptr(refCon),
			  chop_ptr(newService),
			  chop_ptr(notifier));
	if (!target)
		return false;
	if (!strncmp(newService->getMetaClass()->getClassName(), "AppleDisplay", sizeof "AppleDisplay")) {
		OSData* pEdid = OSDynamicCast(OSData, newService->getProperty(kIODisplayEDIDKey));
		if (ct->logging) {
			if (pEdid)
				IOLog("VoodooHDA DBG:   AppleDisplay with EDID of length %u\n", pEdid->getLength());
			else
				IOLog("VoodooHDA DBG:   AppleDisplay, no EDID\n");
		}
		IOLockLock(ct->lock);
		struct FramebufferTracker* fbtlast = static_cast<struct FramebufferTracker*>(NULL);
		for (struct FramebufferTracker* fbt = ct->first; fbt; fbt = fbt->next) {
			if (fbt->ad == newService) {
				IOLockUnlock(ct->lock);
				return false;
			}
			fbtlast = fbt;
		}
		IOFramebuffer* fb = searchForFramebuffer(ct, newService);
		if (fb) {
			if (ct->logging)
				IOLog("VoodooHDA DBG:   Framebuffer ancestor 0x%lx\n", chop_ptr(fb));
			struct FramebufferTracker* new_fbt = static_cast<struct FramebufferTracker*>(IOMalloc(sizeof *new_fbt));
			if (!new_fbt) {
				IOLockUnlock(ct->lock);
				return false;
			}
			bzero(new_fbt, sizeof *new_fbt);
			new_fbt->fb = fb;
			new_fbt->ad = newService;
			new_fbt->lock = ct->lock;
			new_fbt->logging = ct->logging;
			new_fbt->index = ++ct->fbCounter;
			if (fbtlast)
				fbtlast->next = new_fbt;
			else
				ct->first = new_fbt;
			initialScanFramebuffer(new_fbt);
			parseEdid(new_fbt, pEdid);
		}
		IOLockUnlock(ct->lock);
	}
	return false;
}

static bool displayTerminatedNotificationHandler(void* target, void* __unused refCon, IOService* newService, __unused IONotifier* notifier)
{
	struct CentralTracker* ct;
	struct FramebufferTracker* fbt;
	struct FramebufferTracker* fbtlast;
	struct FramebufferTracker* fbtnext;
	bool unlinked = false;

	if (!target)
		return false;
	ct = (struct CentralTracker*) target;
	IOLockLock(ct->lock);
	fbtlast = static_cast<struct FramebufferTracker*>(NULL);
	for (fbt = ct->first; fbt; fbt = fbtnext) {
		fbtnext = fbt->next;
		if (fbt->ad == newService) {
			fbt->fb = static_cast<IOFramebuffer*>(NULL); // invalidate for handlers
			if (fbtlast)
				fbtlast->next = fbtnext;
			else
				ct->first = fbtnext;
			unlinked = true;
			break;
		}
		fbtlast = fbt;
	}
	IOLockUnlock(ct->lock);
	if (unlinked) {
		for (int i = 0; i < 2; ++i)
			if (fbt->notifiers[i])
				fbt->notifiers[i]->remove();
		bzero(fbt, sizeof *fbt);
		IOFree(fbt, sizeof *fbt);
	}
	return false;
}

__attribute__((noinline, visibility("hidden")))
void* VoodooHDADevice::installVoodooHDAMatchedNotificationHandlers(void)
{
	OSDictionary* md;
	struct CentralTracker* ct;

	if (!mPciNub)
		return NULL;
	ct = static_cast<struct CentralTracker*>(IOMalloc(sizeof *ct));
	if (!ct)
		return NULL;
	bzero(ct, sizeof *ct);
	ct->vendorId = static_cast<uint16_t>(mDeviceId & 0xFFFFU);
	ct->logging = static_cast<uint8_t>(mVerbose & 255U);
	ct->lock = IOLockAlloc();
	if (!ct->lock) {
		bzero(ct, sizeof *ct);
		IOFree(ct, sizeof *ct);
		return NULL;
	}
	ct->busNumber = mPciNub->getBusNumber();
	if (ct->logging)
		IOLog("VoodooHDA DBG: installVoodooHDAMatchedNotificationHandlers Begin\n");
	md = IOService::serviceMatching("AppleDisplay");
	if (md) {
		ct->notifiers[0] = IOService::addMatchingNotification(gIOMatchedNotification, md, &displayMatchedNotificationHandler, ct, NULL, 0);
		ct->notifiers[1] = IOService::addMatchingNotification(gIOTerminatedNotification, md, &displayTerminatedNotificationHandler, ct, NULL, 0);
		md->release();
	}
	if (ct->logging)
		IOLog("VoodooHDA DBG: installVoodooHDAMatchedNotificationHandlers End\n");
	return ct;
}

__attribute__((noinline, visibility("hidden")))
void VoodooHDADevice::uninstallVoodooHDAMatchedNotificationHandlers(void* obj)
{
	struct CentralTracker* ct;
	struct FramebufferTracker* fbt;
	struct FramebufferTracker* fbtnext;

	if (!obj)
		return;
	ct = static_cast<struct CentralTracker*>(obj);
	for (int i = 0; i < 2; ++i)
		if (ct->notifiers[i]) {
			ct->notifiers[i]->remove();
			ct->notifiers[i] = static_cast<IONotifier*>(NULL);
		}
	IOLockLock(ct->lock);
	for (fbt = ct->first; fbt; fbt = fbt->next)
		fbt->fb = static_cast<IOFramebuffer*>(NULL); // invalidate for handlers
	fbt = ct->first;
	ct->first = static_cast<struct FramebufferTracker*>(NULL); // unlink whole list
	IOLockUnlock(ct->lock);
	for (; fbt; fbt = fbtnext) {
		fbtnext = fbt->next;
		for (int i = 0; i < 2; ++i)
			if (fbt->notifiers[i])
				fbt->notifiers[i]->remove();
		bzero(fbt, sizeof *fbt);
		IOFree(fbt, sizeof *fbt);
	}
	IOLockFree(ct->lock);
	bzero(ct, sizeof *ct);
	IOFree(ct, sizeof *ct);
}

__attribute__((noinline, visibility("hidden")))
UInt32 VoodooHDADevice::getMonitorNameAndConnectorType(void* obj, int cad, int pinNid, const char** pMonitorName, UInt32* pConnectorType)
{
	struct CentralTracker* ct;
	struct FramebufferTracker* fbt;
	UInt32 ret = 0U;

	if (!obj)
		return false;
	ct = static_cast<struct CentralTracker*>(obj);
	IOLockLock(ct->lock);
	for (fbt = ct->first; fbt; fbt = fbt->next) {
		scanFramebufferProps(fbt);
		if (!matchCadNid(fbt, cad, pinNid))
			continue;
		if (pMonitorName && fbt->monitorName[0] != '\0') {
			*pMonitorName = &fbt->monitorName[0];
			ret |= 1U;
		}
		if (pConnectorType && (fbt->have_flags & HAVE_CONNECTOR_TYPE)) {
			*pConnectorType = fbt->u.p.connector_type;
			ret |= 2U;
		}
		break;
	}
	IOLockUnlock(ct->lock);
	return ret;
}
