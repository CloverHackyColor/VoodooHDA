#include "License.h"

#include "VoodooHDADevice.h"
#include "VoodooHDAEngine.h"
#include "VoodooGFXHDA.h"
#include "VoodooHDAFramebufferNotifier.h"
#include "Common.h"
#include "Verbs.h"

static bool VoodooGFXHDAValidStreamRegisterState(VoodooHDADevice *device, Channel *channel, int *streamIndex)
{
	if (!device || !device->mRegBase || !channel || channel->off < 0)
		return false;

	int index = channel->off >> 5;
	if (index < 0 || index >= 32)
		return false;

	if (streamIndex)
		*streamIndex = index;
	return true;
}

VoodooGFXHDAStream::VoodooGFXHDAStream()
	: mController(NULL), mEngine(NULL), mChannel(NULL), mActive(false), mClippedPosition(0)
{
}

bool VoodooGFXHDAStream::init(VoodooGFXHDAController *controller, VoodooHDAEngine *engine, Channel *channel)
{
	if (!controller || !engine || !channel)
		return false;

	mController = controller;
	mEngine = engine;
	mChannel = channel;
	resetPositionState();
	return true;
}

bool VoodooGFXHDAStream::isActive() const
{
	return mActive;
}

void VoodooGFXHDAStream::activate()
{
	mActive = true;
	resetPositionState();
}

void VoodooGFXHDAStream::deactivate()
{
	mActive = false;
	resetPositionState();
}


void VoodooGFXHDAStream::unregisterFromController()
{
	/* Avoid VoodooHDAEngine::free() dereferencing VoodooHDADevice while
	 * coreaudiod/IOAudioFamily are tearing down live user clients.  The stream
	 * owns its controller/channel registration state, so unregister from here
	 * before detach/delete and then clear pointers. */
	if (mController && mChannel)
		mController->unregisterStream(mChannel, this);
}

void VoodooGFXHDAStream::detach()
{
	mActive = false;
	mController = NULL;
	mEngine = NULL;
	mChannel = NULL;
	mClippedPosition = 0;
}

void VoodooGFXHDAStream::resetPositionState()
{
	mClippedPosition = 0;
}

void VoodooGFXHDAStream::resetClipPosition(UInt32 clipSampleFrame)
{
	mClippedPosition = clipSampleFrame;
}

void VoodooGFXHDAStream::noteClippedPosition(UInt32 nextSampleFrame)
{
	mClippedPosition = nextSampleFrame;
}

void VoodooGFXHDAStream::serviceInterrupt(UInt32 status, AbsoluteTime *timeStamp)
{
	if (!mActive || !mEngine || !(status & HDAC_SDSTS_BCIS))
		return;

	mEngine->takeTimeStamp(true, timeStamp);
}

UInt32 VoodooGFXHDAStream::getCurrentSampleFrame()
{
	UInt32 position;
	UInt32 frame;
	bool valid = false;

	if (!mController || !mEngine || !mChannel || !mActive ||
	    !(mChannel->flags & HDAC_CHN_RUNNING))
		return 0;

	position = mController->getLinkPosition(mChannel, &valid);
	if (!valid)
		return 0;

	if (!mEngine->mSampleSize)
		return 0;

	frame = position / mEngine->mSampleSize;
	return (frame < mEngine->mNumSampleFrames) ? frame : 0;
}

UInt32 VoodooGFXHDAStream::getClippedPosition() const
{
	return mClippedPosition;
}

VoodooGFXHDAController::VoodooGFXHDAController()
	: mDevice(NULL), mNumStreams(0)
{
	bzero(mStreams, sizeof(mStreams));
}

bool VoodooGFXHDAController::init(VoodooHDADevice *device)
{
	if (!device)
		return false;

	mDevice = device;
	mNumStreams = 0;
	bzero(mStreams, sizeof(mStreams));
	return true;
}

bool VoodooGFXHDAController::ownsChannel(Channel *channel) const
{
	return channel && channel->pcmDevice &&
	       channel->direction == PCMDIR_PLAY &&
	       channel->pcmDevice->digital >= 2;
}

void VoodooGFXHDAController::registerStream(Channel *channel, VoodooGFXHDAStream *stream)
{
	if (!channel || !stream)
		return;

	if (mNumStreams < 0)
		mNumStreams = 0;
	if (mNumStreams > static_cast<int>(sizeof(mStreams) / sizeof(mStreams[0])))
		mNumStreams = static_cast<int>(sizeof(mStreams) / sizeof(mStreams[0]));

	for (int i = 0; i < mNumStreams; i++) {
		if (mStreams[i].channel != channel)
			continue;
		mStreams[i].stream = stream;
		return;
	}

	if (mNumStreams >= static_cast<int>(sizeof(mStreams) / sizeof(mStreams[0])))
		return;

	mStreams[mNumStreams].channel = channel;
	mStreams[mNumStreams].stream = stream;
	mNumStreams++;
}

void VoodooGFXHDAController::unregisterStream(Channel *channel, VoodooGFXHDAStream *stream)
{
	if (!channel || !stream || mNumStreams <= 0)
		return;
	if (mNumStreams > static_cast<int>(sizeof(mStreams) / sizeof(mStreams[0])))
		mNumStreams = static_cast<int>(sizeof(mStreams) / sizeof(mStreams[0]));

	for (int i = 0; i < mNumStreams; i++) {
		if (mStreams[i].channel != channel || mStreams[i].stream != stream)
			continue;
		for (int j = i + 1; j < mNumStreams; j++)
			mStreams[j - 1] = mStreams[j];
		mNumStreams--;
		mStreams[mNumStreams].channel = NULL;
		mStreams[mNumStreams].stream = NULL;
		return;
	}
}


void VoodooGFXHDAController::detachAllStreams()
{
	/*
	 * Live VoodooHDA removal can race IOAudioFamily/coreaudiod teardown.
	 * Do not delete stream helpers here: published IOAudioEngine instances may
	 * still hold mDigitalStream pointers until their own free() runs.  Detach the
	 * helpers from device/controller/channel state and leave the tiny helper
	 * objects intentionally inert.  This avoids UAF/KP during unsafe live unload.
	 */
	if (mNumStreams < 0)
		mNumStreams = 0;
	if (mNumStreams > static_cast<int>(sizeof(mStreams) / sizeof(mStreams[0])))
		mNumStreams = static_cast<int>(sizeof(mStreams) / sizeof(mStreams[0]));

	for (int i = 0; i < mNumStreams; i++) {
		if (mStreams[i].stream)
			mStreams[i].stream->detach();
		mStreams[i].channel = NULL;
		mStreams[i].stream = NULL;
	}
	mNumStreams = 0;
	mDevice = NULL;
}

VoodooGFXHDAStream *VoodooGFXHDAController::lookupStream(Channel *channel)
{
	if (!channel || mNumStreams <= 0)
		return NULL;
	if (mNumStreams > static_cast<int>(sizeof(mStreams) / sizeof(mStreams[0])))
		mNumStreams = static_cast<int>(sizeof(mStreams) / sizeof(mStreams[0]));

	for (int i = 0; i < mNumStreams; i++) {
		if (mStreams[i].channel == channel)
			return mStreams[i].stream;
	}
	return NULL;
}

bool VoodooGFXHDAController::initializeStreamDMA(Channel *channel)
{
	PcmDevice *pcmDevice;
	UInt32 coeff;
	UInt32 blockSize;

	if (!ownsChannel(channel) || !mDevice)
		return false;

	pcmDevice = channel->pcmDevice;
	if (!pcmDevice || !channel->funcGroup || !channel->funcGroup->codec)
		return false;
	coeff = appleGfxHdaAmdMemoryDescCoeffForCodec(channel->funcGroup->codec->deviceId);
	if (coeff != 0) {
		UInt64 chanSize = (UInt64)channel->streamId * coeff * 4U;
		if (chanSize > 0xffffffffULL) {
			mDevice->errorMsg("error: HDMI DMA size overflow streamId=%d coeff=0x%x\n",
					channel->streamId, (unsigned)coeff);
			return false;
		}
		/* AppleGFXHDA allocates graphics-audio stream memory as
		 * streamId * coeff * 4, then slices it into 4 KB BDL pages. */
		pcmDevice->chanSize = (UInt32)chanSize;
		pcmDevice->chanNumBlocks = pcmDevice->chanSize / HDA_BUFSZ_MIN;
	}

	if (!pcmDevice->chanSize || pcmDevice->chanNumBlocks < HDA_BDL_MIN ||
	    pcmDevice->chanNumBlocks > HDA_BDL_MAX) {
		mDevice->errorMsg("error: invalid HDMI DMA geometry size=%u blocks=%u codec=%04x\n",
				(unsigned)pcmDevice->chanSize, (unsigned)pcmDevice->chanNumBlocks,
				channel->funcGroup->codec->deviceId);
		return false;
	}
	blockSize = pcmDevice->chanSize / pcmDevice->chanNumBlocks;
	if (blockSize < HDA_BLK_MIN) {
		mDevice->errorMsg("error: invalid HDMI DMA block size=%u size=%u blocks=%u codec=%04x\n",
				(unsigned)blockSize, (unsigned)pcmDevice->chanSize,
				(unsigned)pcmDevice->chanNumBlocks, channel->funcGroup->codec->deviceId);
		return false;
	}

	mDevice->logMsg("HDMI DMA: codec=%04x family=%s streamId=%d coeff=0x%x chanSize=%u chanNumBlocks=%u blockSize=%u\n",
			channel->funcGroup->codec->deviceId,
			appleGfxHdaAmdCodecFamilyName(channel->funcGroup->codec->deviceId),
			channel->streamId, (unsigned)coeff, (unsigned)pcmDevice->chanSize,
			(unsigned)pcmDevice->chanNumBlocks, (unsigned)blockSize);

	channel->blockSize = blockSize;
	channel->numBlocks = pcmDevice->chanNumBlocks;

	if (allocateBdlMemory(channel) != 0) {
		channel->numBlocks = 0;
		return false;
	}

	channel->buffer = mDevice->allocateDmaMemory(pcmDevice->chanSize, "buffer");
	if (!channel->buffer) {
		mDevice->errorMsg("can't allocate HDMI/DP sound buffer!\n");
		if (channel->bdlMem) {
			mDevice->freeDmaMemory(channel->bdlMem);
			channel->bdlMem = NULL;
		}
		channel->numBlocks = 0;
		return false;
	}

	ASSERT(channel->buffer->size == pcmDevice->chanSize);
	ASSERT(channel->blockSize <= (pcmDevice->chanSize / HDA_BDL_MIN));
	ASSERT(channel->blockSize >= HDA_BLK_MIN);
	ASSERT(channel->numBlocks <= HDA_BDL_MAX);
	ASSERT(channel->numBlocks >= HDA_BDL_MIN);

	return true;
}

void VoodooGFXHDAController::prepareStreamDMA(Channel *channel)
{
	VoodooGFXHDAStream *stream = lookupStream(channel);

	if (!ownsChannel(channel) || !mDevice)
		return;
	if (!VoodooGFXHDAValidStreamRegisterState(mDevice, channel, NULL))
		return;

	if (stream)
		stream->resetPositionState();

	stopStreamRegisters(channel);
	resetStreamRegisters(channel);

	{
		UInt32 posAfterReset = mDevice->readData32(channel->off + HDAC_SDLPIB);
		if (posAfterReset != 0)
			mDevice->errorMsg("SDLPIB=0x%x after streamReset (stream off=0x%x), expected 0\n",
					  posAfterReset, channel->off);
	}

	if (channel->buffer)
		bzero(reinterpret_cast<void *>(channel->buffer->virtAddr), channel->buffer->size);

	setupBdl(channel);
	setStreamId(channel);
}

void VoodooGFXHDAController::startStream(Channel *channel)
{
	if (ownsChannel(channel) && VoodooGFXHDAValidStreamRegisterState(mDevice, channel, NULL))
		startStreamRegisters(channel);
}

void VoodooGFXHDAController::stopStream(Channel *channel)
{
	if (!ownsChannel(channel))
		return;
	if (!VoodooGFXHDAValidStreamRegisterState(mDevice, channel, NULL))
		return;

	stopStreamRegisters(channel);

	if (channel->buffer)
		bzero(reinterpret_cast<void *>(channel->buffer->virtAddr), channel->buffer->size);
}

void VoodooGFXHDAController::handleStreamInterrupt(Channel *channel, UInt32 status, AbsoluteTime *timeStamp)
{
	VoodooGFXHDAStream *stream = lookupStream(channel);

	if (!stream)
		return;

	stream->serviceInterrupt(status, timeStamp);
}

void VoodooGFXHDAController::updateTiming(Channel *channel, bool active, bool primeNow)
{
	VoodooGFXHDAStream *stream = lookupStream(channel);

	if (!stream)
		return;

	if (!active) {
		stream->deactivate();
		return;
	}

	stream->activate();
	if (primeNow)
		stream->resetPositionState();
}

UInt32 VoodooGFXHDAController::getLinkPosition(Channel *channel, bool *valid)
{
	UInt32 position = 0;
	UInt32 bufferBytes;

	if (valid)
		*valid = false;
	if (!mDevice || !channel)
		return 0;

	bufferBytes = channel->blockSize * channel->numBlocks - channel->slack;
	if (bufferBytes == 0)
		return 0;

	mDevice->lock(__FUNCTION__);

	if (channel->dmaPos && !(channel->pcmDevice && channel->pcmDevice->digital >= 2))
		position = *(channel->dmaPos);
	else
		position = mDevice->readData32(channel->off + HDAC_SDLPIB);

	mDevice->unlock(__FUNCTION__);

	if (position >= bufferBytes)
		return 0;
	if (valid)
		*valid = true;

	return position;
}

UInt32 VoodooGFXHDAController::getClippedPosition(Channel *channel, bool *active)
{
	VoodooGFXHDAStream *stream = lookupStream(channel);

	if (active)
		*active = stream ? stream->isActive() : false;

	return stream ? stream->getClippedPosition() : 0;
}

void VoodooGFXHDAController::setupStream(Channel *channel, nid_t dac, AudioAssoc *assoc, int totalchn, int totalext)
{
	FunctionGroup *funcGroup = channel->funcGroup;
	UInt8 csum;
	UInt16 AudioInfopacketBufferSize = 0xFFFFU;
	nid_t cad = funcGroup->codec->cad;
	nid_t nid_pin;
	Widget *widget_pin;
	bool atiCodec = isAtiHdmiCodec(funcGroup->codec);
	bool supportsDisableSlots = appleGfxHdaAmdSupportsDisableSlots(funcGroup->codec->deviceId);
	mDevice->logMsg("HDMI streamSetup dac=%d ati=%d totalchn=%d totalext=%d codec=0x%04x:0x%04x family=%s\n",
			dac, atiCodec, totalchn, totalext, funcGroup->codec->vendorId, funcGroup->codec->deviceId,
			appleGfxHdaAmdCodecFamilyName(funcGroup->codec->deviceId));

	const static UInt8 hdmica[2][8] =
	{{ 0x02, 0x00, 0x04, 0x08, 0x0a, 0x0e, 0x12, 0x12 },
	 { 0x01, 0x03, 0x01, 0x03, 0x09, 0x0b, 0x0f, 0x13 }};
	const static UInt32 hdmich[2][8] =
	{{ 0xFFFF0F00, 0xFFFFFF10, 0xFFF2FF10, 0xFF32FF10,
	   0xFF324F10, 0xF5324F10, 0x54326F10, 0x54326F10 },
	 { 0xFFFFF000, 0xFFFF0100, 0xFFFFF210, 0xFFFF2310,
	   0xFF32F410, 0xFF324510, 0xF6324510, 0x76325410 }};

	for (int j = 0; j < 16; j++) {
		if (assoc->dacs[j] != dac)
			continue;
		nid_pin = assoc->pins[j];
		widget_pin = mDevice->widgetGet(funcGroup, nid_pin);
		if (!widget_pin)
			continue;
		if (!HDA_PARAM_PIN_CAP_DP(widget_pin->pin.cap) &&
		    !HDA_PARAM_PIN_CAP_HDMI(widget_pin->pin.cap))
			continue;

		UInt16 diagFlags = channel->diagnosticFlags;
		bool dumpGPUState = (diagFlags & kVoodooHDADiagDumpGPUStateOnStream) != 0;
		bool forceStandardPath = (diagFlags & kVoodooHDADiagForceStandardHDMIPath) != 0;
		bool forceATIVendorPath = atiCodec && ((diagFlags & kVoodooHDADiagForceATIVendorPath) != 0);

		if (atiCodec && mDevice->mFBNotifier)
			mDevice->mFBNotifier->ensureAudioPipeEnabled(cad, nid_pin);
		if (dumpGPUState && mDevice->mFBNotifier)
			mDevice->mFBNotifier->diagnosticDumpGPUState("before-stream-setup", cad, nid_pin);

		mDevice->logMsg("HDMI streamSetup nid_pin=%d dac=%d eld_len=%d (before re-read) pinCap=0x%08x\n",
				nid_pin, dac, widget_pin->eld_len, (unsigned)widget_pin->pin.cap);
		mDevice->hdaa_eld_handler(widget_pin);
		mDevice->logMsg("HDMI streamSetup nid_pin=%d eld_len=%d (after re-read)\n",
				nid_pin, widget_pin->eld_len);

		/* HDMI quality/stability: keep GPU HDMI/DP on a clean stereo LPCM path
		 * unless AC3 passthrough explicitly requests more. Many Polaris/RX580
		 * sinks report broad capabilities but produce distorted audio when fed
		 * 6/8-channel channel-slot maps. */
		if (!(channel->format & AFMT_AC3) && totalchn > 2) {
			mDevice->logMsg("HDMI streamSetup: capping LPCM channels %d -> 2 for stable sink path\n", totalchn);
			totalchn = 2;
			totalext = 0;
		}

		UInt32 dipSizeTest = mDevice->sendCommand(HDA_CMD_GET_HDMI_DIP_SIZE(cad, nid_pin, 0x00), cad);
		bool useStandardPath = (dipSizeTest != HDA_INVALID) && ((dipSizeTest & 0xff) > 0);
		if (atiCodec)
			useStandardPath = false; /* ATI vendor verbs match AMD HDMI codecs better */
		if (forceATIVendorPath)
			useStandardPath = false;
		else if (forceStandardPath)
			useStandardPath = true;

		mDevice->logMsg("HDMI streamSetup nid_pin=%d DIP_SIZE(0x00)=0x%08x -> useStandard=%d ati=%d forceStd=%d forceAti=%d\n",
				nid_pin, (unsigned)dipSizeTest, useStandardPath, atiCodec,
				forceStandardPath ? 1 : 0, forceATIVendorPath ? 1 : 0);

		if (atiCodec && !useStandardPath) {
			int ca = hdmica[totalext == 0 ? 0 : 1][totalchn - 1];
			mDevice->logMsg("HDMI ATI verb path nid_pin=%d ca=0x%02x totalchn=%d\n",
					nid_pin, ca, totalchn);

			static const UInt16 ati_paired_verbs[4] = {
				ATI_VERB_SET_MULTICHANNEL_01,
				ATI_VERB_SET_MULTICHANNEL_23,
				ATI_VERB_SET_MULTICHANNEL_45,
				ATI_VERB_SET_MULTICHANNEL_67
			};

			for (int k = 0; k < 4; k++) {
				int base_slot = k * 2;
				int enable = (base_slot < totalchn) ? 1 : 0;
				UInt32 val = (base_slot << 4) | enable;
				mDevice->sendCommand(ATI_CMD_12BIT(cad, nid_pin, ati_paired_verbs[k], val), cad);
				mDevice->logMsg("HDMI ATI MC%d%d=0x%02x\n", base_slot, base_slot + 1, val);
			}

			mDevice->sendCommand(ATI_CMD_12BIT(cad, nid_pin, ATI_VERB_SET_CHANNEL_ALLOCATION, ca), cad);
			mDevice->logMsg("HDMI ATI CA=0x%02x pinCtrl=0x%02x\n", ca, widget_pin->pin.ctrl);

			if (HDA_PARAM_PIN_CAP_HDMI(widget_pin->pin.cap) &&
			    HDA_PARAM_PIN_CAP_HBR(widget_pin->pin.cap)) {
				UInt32 hbr = ((channel->format & AFMT_AC3) && (totalchn == 8)) ? ATI_HBR_ENABLE : 0;
				mDevice->sendCommand(ATI_CMD_12BIT(cad, nid_pin, ATI_VERB_SET_HBR_CONTROL, hbr), cad);
			}

			mDevice->logMsg("ATI HDMI verb path: nid=%d ca=0x%02x totalchn=%d\n", nid_pin, ca, totalchn);

			widget_pin->pin.ctrl |= 0x40;
			mDevice->sendCommand(HDA_CMD_SET_PIN_WIDGET_CTRL(cad, nid_pin, widget_pin->pin.ctrl), cad);

			for (int k = 0; k < 8; k++) {
				UInt16 slotVerb;
				if (k < totalchn)
					slotVerb = (((hdmich[totalext == 0 ? 0 : 1][totalchn - 1] >> (k * 4)) & 0xf) << 4) | k;
				else if (supportsDisableSlots)
					slotVerb = 0xf0 | k;
				else
					continue;
				mDevice->sendCommand(HDA_CMD_SET_HDMI_CHAN_SLOT(cad, nid_pin, slotVerb), cad);
			}

			{
				int caAti = hdmica[totalext == 0 ? 0 : 1][totalchn - 1];
				UInt8 csumAti = -(0x84 + 0x01 + 0x0a + (totalchn - 1) + caAti);

				mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_INDEX(cad, nid_pin, 0x00), cad);
				mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_XMIT(cad, nid_pin, 0x00), cad);
				mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_INDEX(cad, nid_pin, 0x00), cad);
				mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x84), cad);
				mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x01), cad);
				mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x0a), cad);
				mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, csumAti), cad);
				mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, totalchn - 1), cad);
				mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x00), cad);
				mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x00), cad);
				mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, caAti), cad);
				mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_INDEX(cad, nid_pin, 0x00), cad);
				mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_XMIT(cad, nid_pin, 0xc0), cad);
			}
			mDevice->logMsg("HDMI ATI path + CHAN_SLOT + InfoFrame + DIP_XMIT=0xc0 nid=%d ca=0x%02x chn=%d disableSlots=%d\n",
					nid_pin, hdmica[totalext == 0 ? 0 : 1][totalchn - 1], totalchn,
					supportsDisableSlots ? 1 : 0);
			if (dumpGPUState && mDevice->mFBNotifier)
				mDevice->mFBNotifier->diagnosticDumpGPUState("after-ati-stream-setup", cad, nid_pin);
			continue;
		}

		mDevice->logMsg("HDMI standard HDA path nid_pin=%d\n", nid_pin);

		for (int k = 0; k < 8; k++)
			mDevice->sendCommand(HDA_CMD_SET_HDMI_CHAN_SLOT(cad, nid_pin,
				(((hdmich[totalext == 0 ? 0 : 1][totalchn - 1] >> (k * 4)) & 0xf) << 4) | k), cad);

		if (HDA_PARAM_PIN_CAP_HDMI(widget_pin->pin.cap) &&
		    HDA_PARAM_PIN_CAP_HBR(widget_pin->pin.cap)) {
			widget_pin->pin.ctrl &= ~HDA_CMD_SET_PIN_WIDGET_CTRL_VREF_ENABLE_MASK;
			if ((channel->format & AFMT_AC3) && (totalchn == 8))
				widget_pin->pin.ctrl |= 0x03;
			mDevice->sendCommand(HDA_CMD_SET_PIN_WIDGET_CTRL(cad, nid_pin, widget_pin->pin.ctrl), cad);
		}

		if (AudioInfopacketBufferSize == 0xFFFFU) {
			UInt32 dipSize = mDevice->sendCommand(HDA_CMD_GET_HDMI_DIP_SIZE(cad, nid_pin, 0x00), cad);
			AudioInfopacketBufferSize = (dipSize != HDA_INVALID) ?
				static_cast<UInt16>((dipSize & 0xff) + 1U) : 0;
			mDevice->logMsg("HDMI nid_pin=%d AudioInfopacketBufferSize=%u\n", nid_pin, AudioInfopacketBufferSize);
		}

		if (AudioInfopacketBufferSize < 10U) {
			mDevice->logMsg("HDMI nid_pin=%d infoframe buffer too small (%u), skipping\n",
					nid_pin, AudioInfopacketBufferSize);
			continue;
		}

		mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_INDEX(cad, nid_pin, 0x00), cad);
		mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_XMIT(cad, nid_pin, 0x00), cad);
		mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_INDEX(cad, nid_pin, 0x00), cad);
		for (int k = 0; k < static_cast<int>(AudioInfopacketBufferSize); k++)
			mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x00), cad);

		mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_INDEX(cad, nid_pin, 0x00), cad);
		bool isDP_conn = widget_pin->eld != NULL && widget_pin->eld_len >= 6 && ((widget_pin->eld[5] >> 2) & 0x3) == 1;
		mDevice->logMsg("HDMI nid_pin=%d infoframe: eld_len=%d conn_type=%s ca=0x%02x totalchn=%d\n",
				nid_pin, widget_pin->eld_len, isDP_conn ? "DP" : "HDMI",
				hdmica[totalext == 0 ? 0 : 1][totalchn - 1], totalchn);
#if DP_AUDIO
		if (isDP_conn) {
			mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x84), cad);
			mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x1b), cad);
			mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x44), cad);
			mDevice->logMsg("DP Audio infoframe\n");
		} else {
#endif
			mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x84), cad);
			mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x01), cad);
			mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x0a), cad);
			mDevice->logMsg("HDMI Audio infoframe\n");

			csum = 0;
			csum -= 0x84 + 0x01 + 0x0a + (totalchn - 1) + hdmica[totalext == 0 ? 0 : 1][totalchn - 1];
			mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, csum), cad);
#if DP_AUDIO
		}
#endif
		mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, totalchn - 1), cad);
		mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x00), cad);
		mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x00), cad);
		mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, hdmica[totalext == 0 ? 0 : 1][totalchn - 1]), cad);
		mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_INDEX(cad, nid_pin, 0x00), cad);
		mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_XMIT(cad, nid_pin, 0xc0), cad);
		if (dumpGPUState && mDevice->mFBNotifier)
			mDevice->mFBNotifier->diagnosticDumpGPUState("after-standard-stream-setup", cad, nid_pin);
	}
}

VoodooHDADevice *VoodooGFXHDAController::getDevice() const
{
	return mDevice;
}

int VoodooGFXHDAController::allocateBdlMemory(Channel *channel)
{
	if (!channel || !mDevice || !channel->pcmDevice) {
		if (mDevice)
			mDevice->errorMsg("error: invalid HDMI/DP BDL allocation state\n");
		return -1;
	}

	PcmDevice *pcmDevice = channel->pcmDevice;
	if (pcmDevice->chanNumBlocks < HDA_BDL_MIN || pcmDevice->chanNumBlocks > HDA_BDL_MAX) {
		mDevice->errorMsg("error: invalid HDMI/DP BDL block count %u\n",
				(unsigned)pcmDevice->chanNumBlocks);
		return -1;
	}

	channel->bdlMem = mDevice->allocateDmaMemory(sizeof(BdlEntry) * pcmDevice->chanNumBlocks,
	                                             "bdlMem", kIOMapWriteThruCache);
	if (!channel->bdlMem) {
		mDevice->errorMsg("error: couldn't allocate HDMI/DP bdl\n");
		return -1;
	}

	return 0;
}

void VoodooGFXHDAController::setupBdl(Channel *channel)
{
	BdlEntry *bdlEntry;
	UInt64 addr;
	UInt32 blockSize, numBlocks;

	if (!VoodooGFXHDAValidStreamRegisterState(mDevice, channel, NULL) ||
	    !channel->buffer || !channel->bdlMem ||
	    !channel->buffer->physAddr || !channel->bdlMem->virtAddr)
		return;

	addr = (UInt64)channel->buffer->physAddr;
	bdlEntry = (BdlEntry *)channel->bdlMem->virtAddr;
	blockSize = channel->blockSize;
	numBlocks = channel->numBlocks;
	if (blockSize == 0 || numBlocks == 0 || blockSize * numBlocks <= channel->slack)
		return;

	for (UInt32 n = 1; n <= numBlocks; n++, bdlEntry++) {
		bdlEntry->addrl = (UInt32)addr;
		bdlEntry->addrh = (UInt32)(addr >> 32);
		bdlEntry->len = ((n == numBlocks) ? (blockSize - channel->slack) : blockSize);
		bdlEntry->ioc = (n == numBlocks);
		addr += bdlEntry->len;
	}

	mDevice->writeData32(channel->off + HDAC_SDCBL, blockSize * numBlocks - channel->slack);
	mDevice->writeData16(channel->off + HDAC_SDLVI, numBlocks - 1);
	addr = channel->bdlMem->physAddr;
	mDevice->writeData32(channel->off + HDAC_SDBDPL, (UInt32)addr);
	mDevice->writeData32(channel->off + HDAC_SDBDPU, (UInt32)(addr >> 32));
	if (channel->dmaPos && mDevice->mDmaPosMem &&
	    !(mDevice->readData32(HDAC_DPIBLBASE) & 0x00000001)) {
		addr = mDevice->mDmaPosMem->physAddr;
		mDevice->writeData32(HDAC_DPIBLBASE, ((UInt32)addr & HDAC_DPLBASE_DPLBASE_MASK) | 0x00000001);
		mDevice->writeData32(HDAC_DPIBUBASE, (UInt32)(addr >> 32));
	}
}

void VoodooGFXHDAController::stopStreamRegisters(Channel *channel)
{
	UInt32 ctl;
	int streamIndex;

	if (!VoodooGFXHDAValidStreamRegisterState(mDevice, channel, &streamIndex))
		return;

	ctl = mDevice->readData8(channel->off + HDAC_SDCTL0);
	ctl &= ~(HDAC_SDCTL_IOCE | HDAC_SDCTL_FEIE | HDAC_SDCTL_DEIE | HDAC_SDCTL_RUN);
	mDevice->writeData8(channel->off + HDAC_SDCTL0, ctl);

	channel->flags &= ~HDAC_CHN_RUNNING;

	ctl = mDevice->readData32(HDAC_INTCTL);
	ctl &= ~(1U << streamIndex);
	mDevice->writeData32(HDAC_INTCTL, ctl);
}

void VoodooGFXHDAController::startStreamRegisters(Channel *channel)
{
	UInt32 ctl;
	int streamIndex;

	if (!VoodooGFXHDAValidStreamRegisterState(mDevice, channel, &streamIndex))
		return;

	channel->flags |= HDAC_CHN_RUNNING;

	ctl = mDevice->readData32(HDAC_INTCTL);
	ctl |= 1U << streamIndex;
	mDevice->writeData32(HDAC_INTCTL, ctl);
	mDevice->writeData8(channel->off + HDAC_SDSTS, HDAC_SDSTS_DESE | HDAC_SDSTS_FIFOE | HDAC_SDSTS_BCIS);

	if (channel->stripectl) {
		ctl = mDevice->readData8(channel->off + HDAC_SDCTL2);
		ctl &= ~HDAC_SDCTL2_STRIPE_MASK;
		ctl |= channel->stripectl << HDAC_SDCTL2_STRIPE_SHIFT;
		mDevice->writeData8(channel->off + HDAC_SDCTL2, ctl);
	}

	ctl = mDevice->readData8(channel->off + HDAC_SDCTL0);
	ctl |= HDAC_SDCTL_IOCE | HDAC_SDCTL_FEIE | HDAC_SDCTL_DEIE | HDAC_SDCTL_RUN;
	mDevice->writeData8(channel->off + HDAC_SDCTL0, ctl);
}

void VoodooGFXHDAController::resetStreamRegisters(Channel *channel)
{
	int timeout = 1000;
	int to = timeout;
	UInt32 ctl;

	if (!VoodooGFXHDAValidStreamRegisterState(mDevice, channel, NULL))
		return;

	ctl = mDevice->readData8(channel->off + HDAC_SDCTL0);
	ctl |= HDAC_SDCTL_SRST;
	mDevice->writeData8(channel->off + HDAC_SDCTL0, ctl);
	do {
		ctl = mDevice->readData8(channel->off + HDAC_SDCTL0);
		if (ctl & HDAC_SDCTL_SRST)
			break;
		IODelay(10);
	} while (--to);
	if (!(ctl & HDAC_SDCTL_SRST))
		mDevice->errorMsg("timeout in HDMI/DP reset\n");
	ctl &= ~HDAC_SDCTL_SRST;
	mDevice->writeData8(channel->off + HDAC_SDCTL0, ctl);
	to = timeout;
	do {
		ctl = mDevice->readData8(channel->off + HDAC_SDCTL0);
		if (!(ctl & HDAC_SDCTL_SRST))
			break;
		IODelay(10);
	} while (--to);
	if (ctl & HDAC_SDCTL_SRST)
		mDevice->errorMsg("can't reset HDMI/DP stream!\n");
}

void VoodooGFXHDAController::setStreamId(Channel *channel)
{
	UInt32 ctl;

	if (!VoodooGFXHDAValidStreamRegisterState(mDevice, channel, NULL))
		return;

	ctl = mDevice->readData8(channel->off + HDAC_SDCTL2);
	ctl &= ~(HDAC_SDCTL2_STRM_MASK | HDAC_SDCTL2_STRIPE_MASK);
	ctl |= channel->streamId << HDAC_SDCTL2_STRM_SHIFT;
	mDevice->writeData8(channel->off + HDAC_SDCTL2, ctl);
}
