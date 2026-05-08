#include "License.h"

#include "VoodooHDADevice.h"
#include "VoodooHDAEngine.h"
#include "VoodooGFXHDA.h"
#include "VoodooHDAFramebufferNotifier.h"
#include "Common.h"
#include "Verbs.h"

static UInt32 appleGfxHdaAmdMemoryDescCoeff(UInt32 controllerId)
{
	switch (controllerId & 0xffff) {
		case 0xaa00:
		case 0xaa08:
		case 0xaa10:
		case 0xaa18:
		case 0xaa20:
		case 0xaa28:
		case 0xaa30:
		case 0xaa38:
		case 0xaa40:
		case 0xaa48:
		case 0xaaf0:
			return 0x3000;
		default:
			return 0;
	}
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

VoodooGFXHDAStream *VoodooGFXHDAController::lookupStream(Channel *channel)
{
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

	if (!ownsChannel(channel) || !mDevice)
		return false;

	pcmDevice = channel->pcmDevice;
	coeff = appleGfxHdaAmdMemoryDescCoeff(mDevice->mDeviceId);
	if (coeff != 0) {
		/* AppleGFXHDA allocates graphics-audio stream memory as
		 * streamId * coeff * 4, then slices it into 4 KB BDL pages. */
		pcmDevice->chanSize = channel->streamId * coeff * 4;
		pcmDevice->chanNumBlocks = pcmDevice->chanSize / HDA_BUFSZ_MIN;
		IOLog("VoodooHDA DBG: channelInit HDMI AppleGFXHDA-like dma streamId=%d coeff=0x%x chanSize=%u chanNumBlocks=%u blockSize=%u\n",
		      channel->streamId, (unsigned)coeff, (unsigned)pcmDevice->chanSize,
		      (unsigned)pcmDevice->chanNumBlocks,
		      (unsigned)(pcmDevice->chanSize / pcmDevice->chanNumBlocks));
	}

	channel->blockSize = pcmDevice->chanSize / pcmDevice->chanNumBlocks;
	channel->numBlocks = pcmDevice->chanNumBlocks;

	if (allocateBdlMemory(channel) != 0) {
		channel->numBlocks = 0;
		return false;
	}

	channel->buffer = mDevice->allocateDmaMemory(pcmDevice->chanSize, "buffer");
	if (!channel->buffer) {
		mDevice->errorMsg("can't allocate HDMI/DP sound buffer!\n");
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

	if (stream)
		stream->resetPositionState();

	stopStreamRegisters(channel);
	resetStreamRegisters(channel);

	{
		UInt32 posAfterReset = mDevice->readData32(channel->off + HDAC_SDLPIB);
		if (posAfterReset != 0)
			IOLog("VoodooHDA WARN: SDLPIB=0x%x after streamReset (stream off=0x%x), expected 0\n",
			      posAfterReset, channel->off);
	}

	if (channel->buffer)
		bzero(reinterpret_cast<void *>(channel->buffer->virtAddr), channel->buffer->size);

	setupBdl(channel);
	setStreamId(channel);
}

void VoodooGFXHDAController::startStream(Channel *channel)
{
	if (ownsChannel(channel))
		startStreamRegisters(channel);
}

void VoodooGFXHDAController::stopStream(Channel *channel)
{
	if (!ownsChannel(channel))
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

void VoodooGFXHDAController::setupStream(Channel *channel, nid_t dac, AudioAssoc *assoc, int totalchn, int totalext)
{
	FunctionGroup *funcGroup = channel->funcGroup;
	UInt8 csum;
	UInt16 AudioInfopacketBufferSize = 0xFFFFU;
	nid_t cad = funcGroup->codec->cad;
	nid_t nid_pin;
	Widget *widget_pin;
	bool atiCodec = isAtiHdmiCodec(funcGroup->codec);
	IOLog("VoodooHDA HDMI: streamSetup dac=%d ati=%d totalchn=%d totalext=%d codec=0x%04x:0x%04x\n",
	      dac, atiCodec, totalchn, totalext, funcGroup->codec->vendorId, funcGroup->codec->deviceId);

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

		if (atiCodec && mDevice->mFBNotifier)
			mDevice->mFBNotifier->ensureAudioPipeEnabled(cad, nid_pin);

		IOLog("VoodooHDA HDMI: streamSetup nid_pin=%d dac=%d eld_len=%d (before re-read) pinCap=0x%08x\n",
		      nid_pin, dac, widget_pin->eld_len, (unsigned)widget_pin->pin.cap);
		mDevice->hdaa_eld_handler(widget_pin);
		IOLog("VoodooHDA HDMI: streamSetup nid_pin=%d eld_len=%d (after re-read)\n",
		      nid_pin, widget_pin->eld_len);

		UInt32 dipSizeTest = mDevice->sendCommand(HDA_CMD_GET_HDMI_DIP_SIZE(cad, nid_pin, 0x00), cad);
		bool useStandardPath = (dipSizeTest != HDA_INVALID) && ((dipSizeTest & 0xff) > 0);

		IOLog("VoodooHDA HDMI: streamSetup nid_pin=%d DIP_SIZE(0x00)=0x%08x -> useStandard=%d ati=%d\n",
		      nid_pin, (unsigned)dipSizeTest, useStandardPath, atiCodec);

		if (atiCodec && !useStandardPath) {
			int ca = hdmica[totalext == 0 ? 0 : 1][totalchn - 1];
			IOLog("VoodooHDA HDMI: ATI verb path nid_pin=%d ca=0x%02x totalchn=%d\n",
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
				IOLog("VoodooHDA HDMI: ATI MC%d%d=0x%02x\n", base_slot, base_slot + 1, val);
			}

			mDevice->sendCommand(ATI_CMD_12BIT(cad, nid_pin, ATI_VERB_SET_CHANNEL_ALLOCATION, ca), cad);
			IOLog("VoodooHDA HDMI: ATI CA=0x%02x pinCtrl=0x%02x\n", ca, widget_pin->pin.ctrl);

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
				else
					slotVerb = 0xf0 | k;
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
			IOLog("VoodooHDA HDMI: ATI path + CHAN_SLOT + InfoFrame + DIP_XMIT=0xc0 nid=%d ca=0x%02x chn=%d\n",
			      nid_pin, hdmica[totalext == 0 ? 0 : 1][totalchn - 1], totalchn);
			continue;
		}

		IOLog("VoodooHDA HDMI: standard HDA path nid_pin=%d\n", nid_pin);

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
			AudioInfopacketBufferSize = static_cast<UInt16>(mDevice->sendCommand(HDA_CMD_GET_HDMI_DIP_SIZE(cad, nid_pin, 0x00), cad)) + 1U;
			IOLog("VoodooHDA HDMI: nid_pin=%d AudioInfopacketBufferSize=%u\n", nid_pin, AudioInfopacketBufferSize);
		}

		if (AudioInfopacketBufferSize < 10U) {
			IOLog("VoodooHDA HDMI: nid_pin=%d infoframe buffer too small (%u), skipping\n",
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
		IOLog("VoodooHDA HDMI: nid_pin=%d infoframe: eld_len=%d conn_type=%s ca=0x%02x totalchn=%d\n",
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
	}
}

VoodooHDADevice *VoodooGFXHDAController::getDevice() const
{
	return mDevice;
}

int VoodooGFXHDAController::allocateBdlMemory(Channel *channel)
{
	PcmDevice *pcmDevice = channel->pcmDevice;

	ASSERT(pcmDevice);
	ASSERT(pcmDevice->chanNumBlocks);

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

	addr = (UInt64)channel->buffer->physAddr;
	bdlEntry = (BdlEntry *)channel->bdlMem->virtAddr;
	blockSize = channel->blockSize;
	numBlocks = channel->numBlocks;

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
	if (channel->dmaPos && !(mDevice->readData32(HDAC_DPIBLBASE) & 0x00000001)) {
		addr = mDevice->mDmaPosMem->physAddr;
		mDevice->writeData32(HDAC_DPIBLBASE, ((UInt32)addr & HDAC_DPLBASE_DPLBASE_MASK) | 0x00000001);
		mDevice->writeData32(HDAC_DPIBUBASE, (UInt32)(addr >> 32));
	}
}

void VoodooGFXHDAController::stopStreamRegisters(Channel *channel)
{
	UInt32 ctl;

	ctl = mDevice->readData8(channel->off + HDAC_SDCTL0);
	ctl &= ~(HDAC_SDCTL_IOCE | HDAC_SDCTL_FEIE | HDAC_SDCTL_DEIE | HDAC_SDCTL_RUN);
	mDevice->writeData8(channel->off + HDAC_SDCTL0, ctl);

	channel->flags &= ~HDAC_CHN_RUNNING;

	ctl = mDevice->readData32(HDAC_INTCTL);
	ctl &= ~(1 << (channel->off >> 5));
	mDevice->writeData32(HDAC_INTCTL, ctl);
}

void VoodooGFXHDAController::startStreamRegisters(Channel *channel)
{
	UInt32 ctl;

	channel->flags |= HDAC_CHN_RUNNING;

	ctl = mDevice->readData32(HDAC_INTCTL);
	ctl |= 1 << (channel->off >> 5);
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

	ctl = mDevice->readData8(channel->off + HDAC_SDCTL2);
	ctl &= ~(HDAC_SDCTL2_STRM_MASK | HDAC_SDCTL2_STRIPE_MASK);
	ctl |= channel->streamId << HDAC_SDCTL2_STRM_SHIFT;
	mDevice->writeData8(channel->off + HDAC_SDCTL2, ctl);
}
