#include "License.h"

#include "VoodooHDADevice.h"
#include "VoodooHDAEngine.h"
#include "VoodooGFXHDA.h"
#include "VoodooHDAFramebufferNotifier.h"
#include "Common.h"
#include "Verbs.h"

static UInt32 sDigitalBlockCounter[16] = {0};

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
    if (!mActive || !mEngine) return;
    // Реагируем ТОЛЬКО на завершение DMA-блока
    if (!(status & HDAC_SDSTS_BCIS)) return;
//    mEngine->takeTimeStamp(true, timeStamp);
  
  
  // 🔧 Polaris HDMI: используем системное время вместо timeStamp прерывания.
  // Это убирает PCIe-джиттер, который под нагрузкой вызывает случайный хрип.
  mEngine->takeTimeStamp(false);
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
	coeff = appleGfxHdaAmdMemoryDescCoeffForCodec(channel->funcGroup->codec->hdaPciDeviceId);
	if (coeff != 0) {
		/* AppleGFXHDA allocates graphics-audio stream memory as
		 * streamId * coeff * 4, then slices it into 4 KB BDL pages. */
		pcmDevice->chanSize = channel->streamId * coeff * 4;
		pcmDevice->chanNumBlocks = pcmDevice->chanSize / HDA_BUFSZ_MIN;
		mDevice->logMsg("HDMI DMA: codec=%04x:%04x hdaPci=%04x family=%s streamId=%d coeff=0x%x chanSize=%u chanNumBlocks=%u blockSize=%u\n",
				channel->funcGroup->codec->vendorId,
				channel->funcGroup->codec->deviceId,
				channel->funcGroup->codec->hdaPciDeviceId,
				appleGfxHdaAmdCodecFamilyName(channel->funcGroup->codec->hdaPciDeviceId),
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
    // 🔧 КРИТИЧНО: полный сброс статусных флагов ДО настройки BDL.
    // Убирает "хвосты" BCIS/FIFOE от прошлого потока, вызывающие дублирование на аналоге.
    //  mDevice->writeData8(channel->off + HDAC_SDSTS, 0xFF);

    // Сброс программного счётчика DMA-блоков при каждом старте потока
    UInt32 idx = (channel->streamId - 1) & 0xF;
    sDigitalBlockCounter[idx] = 0;

	stopStreamRegisters(channel);
	resetStreamRegisters(channel);

    // 🔧 Polaris HDMI: принудительно очищаем статусные флаги.
    // Без этого FIFOE/BCIS от прошлого трека вызывает рассинхрон на 1-м блоке DMA.
    mDevice->writeData8(channel->off + HDAC_SDSTS,
                            HDAC_SDSTS_DESE | HDAC_SDSTS_FIFOE | HDAC_SDSTS_BCIS);

//    {
//        UInt32 posAfterReset = mDevice->readData32(channel->off + HDAC_SDLPIB);
//        if (posAfterReset != 0)
//            mDevice->errorMsg("SDLPIB=0x%x after streamReset (off=0x%x), expected 0",
//                              posAfterReset, channel->off);
//    }

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

void VoodooGFXHDAController::advanceDigitalPosition(Channel *channel)
{
    UInt32 idx = (channel->streamId - 1) & 0xF;
    sDigitalBlockCounter[idx]++;
}

UInt32 VoodooGFXHDAController::getLinkPosition(Channel *channel, bool *valid)
{
    UInt32 position = 0;
    UInt32 bufferBytes;
    if (valid) *valid = false;
    if (!mDevice || !channel) return 0;

    bufferBytes = channel->blockSize * channel->numBlocks - channel->slack;
    if (bufferBytes == 0) return 0;
    
    // Для цифровых потоков (HDMI/DP) используем программный счётчик блоков.
    // SDLPIB на Polaris нестабилен и вызывает хрип из-за скачков/зависаний.
/*    if (channel->pcmDevice && channel->pcmDevice->digital >= 2) {
        UInt32 idx = (channel->streamId - 1) & 0xF;
        UInt32 pos = (sDigitalBlockCounter[idx] * channel->blockSize) % bufferBytes;
        if (valid) *valid = true;
        return pos;
    }*/

    mDevice->lock(__FUNCTION__);
 //   position = mDevice->readData32(channel->off + HDAC_SDLPIB);
  if (channel->dmaPos && !(channel->pcmDevice && channel->pcmDevice->digital >= 2))
    position = *(channel->dmaPos);
  else
    position = mDevice->readData32(channel->off + HDAC_SDLPIB);

    mDevice->unlock(__FUNCTION__);

    // Polaris HDMI: SDLPIB может превысить размер буфера из-за prefetch.
    // Простой modulo безопаснее любых фильтров.
    if (position >= bufferBytes)
        position %= bufferBytes;
  // есть вариант return 0;  //SICK!
    
    // 🔧 Цифровые потоки используют программный счётчик, аналоговые — ТОЛЬКО SDLPIB.
    // Перекрёстное использование ломает трекинг и вызывает дублирование.
//    if (channel->pcmDevice && channel->pcmDevice->digital >= 2) {
//        UInt32 idx = (channel->streamId - 1) & 0xF;
//        sDigitalBlockCounter[idx] = (position / channel->blockSize); // Синхронизация
//    }


    if (valid) *valid = true;
    return position;
}

UInt32 VoodooGFXHDAController::getClippedPosition(Channel *channel, bool *active)
{
	VoodooGFXHDAStream *stream = lookupStream(channel);

	if (active)
		*active = stream ? stream->isActive() : false;

	return stream ? stream->getClippedPosition() : 0;
}

void VoodooGFXHDAController::setupStream(Channel* channel, nid_t dac, AudioAssoc* assoc, int totalchn, int totalext)
{
  FunctionGroup* funcGroup = channel->funcGroup;
  nid_t cad = funcGroup->codec->cad;
  nid_t nid_pin;
  Widget* widget_pin;
  bool atiCodec = isAtiHdmiCodec(funcGroup->codec);
  
  const static UInt8 hdmica[2][8] =
  { { 0x02, 0x00, 0x04, 0x08, 0x0a, 0x0e, 0x12, 0x12 },
    { 0x01, 0x03, 0x01, 0x03, 0x09, 0x0b, 0x0f, 0x13 } };
  const static UInt32 hdmich[2][8] =
  { { 0xFFFF0F00, 0xFFFFFF10, 0xFFF2FF10, 0xFF32FF10,
    0xFF324F10, 0xF5324F10, 0x54326F10, 0x54326F10 },
    { 0xFFFFF000, 0xFFFF0100, 0xFFFFF210, 0xFFFF2310,
      0xFF32F410, 0xFF324510, 0xF6324510, 0x76325410 } };
  
  for (int j = 0; j < 16; j++) {
    if (assoc->dacs[j] != dac) continue;
    nid_pin = assoc->pins[j];
    widget_pin = mDevice->widgetGet(funcGroup, nid_pin);
    if (!widget_pin) continue;
    if (!HDA_PARAM_PIN_CAP_DP(widget_pin->pin.cap) &&
        !HDA_PARAM_PIN_CAP_HDMI(widget_pin->pin.cap)) continue;
    
    if (atiCodec && mDevice->mFBNotifier)
      mDevice->mFBNotifier->ensureAudioPipeEnabled(cad, nid_pin);
    
    mDevice->hdaa_eld_handler(widget_pin);
    
    UInt32 dipSizeTest = mDevice->sendCommand(HDA_CMD_GET_HDMI_DIP_SIZE(cad, nid_pin, 0x00), cad);
    bool useStandardPath = (dipSizeTest != HDA_INVALID) && ((dipSizeTest & 0xff) > 0);
    
    // ==================== ATI VENDOR PATH ====================
    if (atiCodec && !useStandardPath) {
      int ca = hdmica[totalext == 0 ? 0 : 1][totalchn - 1];
      
      // Multichannel slot verbs
      static const UInt16 ati_paired_verbs[4] = {
        ATI_VERB_SET_MULTICHANNEL_01, ATI_VERB_SET_MULTICHANNEL_23,
        ATI_VERB_SET_MULTICHANNEL_45, ATI_VERB_SET_MULTICHANNEL_67
      };
      for (int k = 0; k < 4; k++) {
        int base_slot = k * 2;
        int enable = (base_slot < totalchn) ? 1 : 0;
        UInt32 val = (base_slot << 4) | enable;
        mDevice->sendCommand(ATI_CMD_12BIT(cad, nid_pin, ati_paired_verbs[k], val), cad);
      }
      
      mDevice->sendCommand(ATI_CMD_12BIT(cad, nid_pin, ATI_VERB_SET_CHANNEL_ALLOCATION, ca), cad);
      
      if (HDA_PARAM_PIN_CAP_HDMI(widget_pin->pin.cap) &&
          HDA_PARAM_PIN_CAP_HBR(widget_pin->pin.cap)) {
        UInt32 hbr = ((channel->format & AFMT_AC3) && (totalchn == 8)) ? ATI_HBR_ENABLE : 0;
        mDevice->sendCommand(ATI_CMD_12BIT(cad, nid_pin, ATI_VERB_SET_HBR_CONTROL, hbr), cad);
      }
      
      widget_pin->pin.ctrl |= 0x40;
      mDevice->sendCommand(HDA_CMD_SET_PIN_WIDGET_CTRL(cad, nid_pin, widget_pin->pin.ctrl), cad);
      
      for (int k = 0; k < 8; k++) {
        UInt16 slotVerb = (k < totalchn) ?
        (((hdmich[totalext == 0 ? 0 : 1][totalchn - 1] >> (k * 4)) & 0xf) << 4) | k :
        (0xf0 | k);
        mDevice->sendCommand(HDA_CMD_SET_HDMI_CHAN_SLOT(cad, nid_pin, slotVerb), cad);
      }
      
      // ===== InfoFrame (единый расчёт для ATI) =====
      UInt8 sf = 0;
      switch (channel->speed) {
        case 32000: sf = 1; break;
        case 44100: sf = 2; break;
        case 48000: sf = 3; break;
        case 88200: sf = 4; break;
        case 96000: sf = 5; break;
        case 176400: sf = 6; break;
        case 192000: sf = 7; break;
        default: sf = 3; break;
      }
      UInt8 ss = 0;
      if (channel->format & AFMT_S16_LE) ss = 1;
      else if (channel->format & AFMT_S32_LE) {
        switch (channel->bit32) {
          case 2: ss = 2; break;
          case 3:
          case 4:
          default: ss = 3; break;
        }
      }
      UInt8 ct = (channel->format & AFMT_AC3) ? 1 : 0;
      UInt8 cc = (totalchn > 0) ? (totalchn - 1) : 0;
      UInt8 pb0 = ((sf & 0x3) << 6) | ((ss & 0x3) << 4) | ((ct & 0x1) << 3) | (cc & 0x7);
      UInt8 caAti = (totalchn == 2) ? 0x00 : hdmica[totalext == 0 ? 0 : 1][totalchn - 1];
      UInt8 csum = -(0x84 + 0x01 + 0x0a + pb0 + caAti);
      
      mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_INDEX(cad, nid_pin, 0x00), cad);
      mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_XMIT(cad, nid_pin, 0x00), cad);
      mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_INDEX(cad, nid_pin, 0x00), cad);
      mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x84), cad);
      mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x01), cad);
      mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x0a), cad);
      mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, csum), cad);
      mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, pb0), cad);  // ← ИМЕННО pb0
      mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x00), cad);
      mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x00), cad);
      mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, caAti), cad);
      mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_INDEX(cad, nid_pin, 0x00), cad);
      mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_XMIT(cad, nid_pin, 0xc0), cad);
      continue;
    }
    
    // ==================== STANDARD HDA PATH ====================
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
    
    UInt16 AudioInfopacketBufferSize = static_cast<UInt16>(
                                                           mDevice->sendCommand(HDA_CMD_GET_HDMI_DIP_SIZE(cad, nid_pin, 0x00), cad)) + 1U;
    if (AudioInfopacketBufferSize < 10U) continue;
    
    // Очистка буфера InfoFrame
    mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_INDEX(cad, nid_pin, 0x00), cad);
    mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_XMIT(cad, nid_pin, 0x00), cad);
    mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_INDEX(cad, nid_pin, 0x00), cad);
    for (int k = 0; k < static_cast<int>(AudioInfopacketBufferSize); k++)
      mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x00), cad);
    
    // Определяем тип подключения из ELD
    bool isDP_conn = widget_pin->eld && widget_pin->eld_len >= 6 &&
    ((widget_pin->eld[5] >> 2) & 0x3) == 1;
    
    // ===== Единый расчёт PB0 и Checksum =====
    UInt8 sf = 0;
    switch (channel->speed) {
      case 32000: sf = 1; break;
      case 44100: sf = 2; break;
      case 48000: sf = 3; break;
      case 88200: sf = 4; break;
      case 96000: sf = 5; break;
      case 176400: sf = 6; break;
      case 192000: sf = 7; break;
      default: sf = 3; break;
    }
    UInt8 ss = 0;
    if (channel->format & AFMT_S16_LE) ss = 1;
    else if (channel->format & AFMT_S32_LE) {
      switch (channel->bit32) {
        case 2: ss = 2; break;
        case 3:
        case 4:
        default: ss = 3; break;
      }
    }
    UInt8 ct = (channel->format & AFMT_AC3) ? 1 : 0;
    UInt8 cc = (totalchn > 0) ? (totalchn - 1) : 0;
    UInt8 pb0 = ((sf & 0x3) << 6) | ((ss & 0x3) << 4) | ((ct & 0x1) << 3) | (cc & 0x7);
    
    // Заголовочные байты: DP vs HDMI
    UInt8 byte1 = 0x84;
    UInt8 byte2 = isDP_conn ? 0x1b : 0x01;
    UInt8 byte3 = isDP_conn ? 0x44 : 0x0a;
    UInt8 ca = hdmica[totalext == 0 ? 0 : 1][totalchn - 1];
    
    // Checksum = -(сумма всех байтов пакета) & 0xFF
    UInt8 csum = -(byte1 + byte2 + byte3 + pb0 + ca);
    
    // Отправка InfoFrame (строго по порядку: заголовок → csum → payload)
    mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_INDEX(cad, nid_pin, 0x00), cad);
    mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_XMIT(cad, nid_pin, 0x00), cad);
    mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_INDEX(cad, nid_pin, 0x00), cad);
    mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, byte1), cad);
    mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, byte2), cad);
    mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, byte3), cad);
    mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, csum), cad);  // ← csum после заголовка
    mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, pb0), cad);   // ← pb0, НЕ totalchn-1
    mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x00), cad);
    mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, 0x00), cad);
    mDevice->sendCommand(HDA_CMD_SET_HDMI_DIP_DATA(cad, nid_pin, ca), cad);
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
    
    // 🔧 КРИТИЧНО: обнуляем программный счётчик при каждом stop.
    // Иначе IOAudioEngine продолжает "играть" phantom-фреймы после конца трека.
    UInt32 idx = (channel->streamId - 1) & 0xF;
    sDigitalBlockCounter[idx] = 0;
}

void VoodooGFXHDAController::startStreamRegisters(Channel *channel)
{

	channel->flags |= HDAC_CHN_RUNNING;

  UInt32 ctl = mDevice->readData32(HDAC_INTCTL);
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
  ctl &= ~(HDAC_SDCTL_IOCE | HDAC_SDCTL_FEIE | HDAC_SDCTL_DEIE | HDAC_SDCTL_RUN); // сначала очистка
	ctl |= HDAC_SDCTL_IOCE | HDAC_SDCTL_DEIE | HDAC_SDCTL_RUN;
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
