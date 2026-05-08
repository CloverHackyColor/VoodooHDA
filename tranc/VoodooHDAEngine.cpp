#include "License.h"

#include "VoodooHDAEngine.h"
#include "VoodooHDADevice.h"
#include "VoodooGFXHDA.h"
#include "Common.h"
#include "Verbs.h"
#include "OssCompat.h"
#include "Tables.h"

#include <libkern/version.h>
#include <IOKit/audio/IOAudioDefines.h>
#include <IOKit/audio/IOAudioPort.h>
#include <IOKit/audio/IOAudioSelectorControl.h>
#include <IOKit/audio/IOAudioLevelControl.h>
#include <IOKit/audio/IOAudioToggleControl.h>
#include <IOKit/pci/IOPCIDevice.h>

#ifdef TIGER
#include "TigerAdditionals.h"
#endif

#define super IOAudioEngine
OSDefineMetaClassAndStructors(VoodooHDAEngine, IOAudioEngine)

#define SAMPLE_CHANNELS		2	// forced stereo quirk is always enabled

/* Sample offset / latency: mirroring Apple's AppleGFXHDAEngine::recalculateEnginesSampleOffset()
 * which calls getOutputSafetyOffset(sampleRate) on every format change.
 *
 * Formula (from AppleGFXHDADriver decompile):
 *   offset = roundup(sampleRate * safetyCoeff_μs / 1_000_000) + base_frames
 *
 * setSampleOffset() (deprecated) does NOT call setOutputSampleOffset() internally —
 * confirmed from IOAudioFamily decompile: setSampleOffset writes field+0x104 /
 * setProperty while setOutputSampleOffset calls vtable+0xb78 which updates the
 * output IOAudioStream object.  We call them explicitly and recalculate on each
 * performFormatChange() as Apple does.
 *
 * Analog coefficients produce the legacy values at 48 kHz (≈64 / 32 frames). */
#define ANALOG_SAFETY_US	1333	// → 64 frames at 48 kHz
#define ANALOG_LATENCY_US	 667	// → 32 frames at 48 kHz
#define HDMI_SAFETY_US		5000	// → 240 + 64 = 304 frames at 48 kHz (~5 ms + FIFO base)
#define HDMI_LATENCY_US		2000	// → 96 frames at 48 kHz (~2 ms)
#define HDMI_FIFO_BASE		  64	// base frames: covers controller FIFO minimum

//extern const char * const gDeviceTypes[], * const gConnTypes[];

#define kVoodooHDAPortSubTypeBase		'voo\x40'
#define VOODOO_OSS_TO_SUBTYPE(type)		(kVoodooHDAPortSubTypeBase + 1 + type)
#define VOODOO_SUBTYPE_TO_OSS(type)		(type - 1 - kVoodooHDAPortSubTypeBase)

/******************************************************************************************/
/******************************************************************************************/

#define logMsg(fmt, args...)	if(mVerbose>0)\
		messageHandler(kVoodooHDAMessageTypeGeneral, fmt, ##args)
#define errorMsg(fmt, args...)	messageHandler(kVoodooHDAMessageTypeError, fmt, ##args)
#define dumpMsg(fmt, args...)	messageHandler(kVoodooHDAMessageTypeDump, fmt, ##args)

__attribute__((visibility("hidden")))
void VoodooHDAEngine::messageHandler(UInt32 type, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	if (mDevice)
		mDevice->messageHandler(type, format, args);
	else if (mVerbose >= 1)
		vprintf(format, args);
	va_end(args);
}

bool    VoodooHDAEngine::driverDesiresHiResSampleIntervals(void) { return false;}

bool VoodooHDAEngine::diagnosticsEnabled() const
{
#if !VOODOO_HDA_DEBUG_BUILD
	return false;
#else
	return mChannel &&
	       mChannel->direction == PCMDIR_PLAY &&
	       ((mChannel->diagnosticFlags & kVoodooHDADiagEnable) != 0);
#endif
}

UInt16 VoodooHDAEngine::diagnosticFlags() const
{
#if !VOODOO_HDA_DEBUG_BUILD
	return 0;
#else
	return mChannel ? mChannel->diagnosticFlags : 0;
#endif
}

bool VoodooHDAEngine::diagnosticUsesMixTone() const
{
	UInt16 flags = diagnosticFlags();
	return diagnosticsEnabled() &&
	       ((flags & kVoodooHDADiagInjectMixTone) != 0) &&
	       ((flags & kVoodooHDADiagInjectDirectTone) == 0);
}

bool VoodooHDAEngine::diagnosticUsesDirectTone() const
{
	return diagnosticsEnabled() &&
	       ((diagnosticFlags() & kVoodooHDADiagInjectDirectTone) != 0);
}

bool VoodooHDAEngine::diagnosticUsesChord() const
{
	return diagnosticsEnabled() &&
	       ((diagnosticFlags() & kVoodooHDADiagInjectChord) != 0);
}

/* Diag-mode chord generator. Sum of 5 pure sines at 1/2/4/8/16 kHz, identical
 * on every channel. Designed for spectral analysis of the captured DMA bytes:
 * a clean spectrum (5 sharp peaks, low noise floor) means the entire CPU+DMA
 * path is intact; smearing/extra peaks/elevated noise means a defect in the
 * driver, format conversion, link, or codec.
 *
 * Uses a 1024-entry sine LUT. Phase is per-frequency, advanced once per audio
 * frame, kept on the Channel struct so it stays continuous across clipOutput
 * calls (any phase reset would itself create a click and look like a glitch).
 */
static const UInt32 kChordFreqs[5] = { 1000U, 2000U, 4000U, 8000U, 16000U };
static const UInt32 kChordLUTBits = 10;
static const UInt32 kChordLUTSize = 1U << kChordLUTBits;
static const UInt32 kChordLUTMask = kChordLUTSize - 1U;
static float gChordSineLUT[kChordLUTSize];
static volatile UInt32 gChordSineLUTReady = 0;

static void chordEnsureLUT()
{
	if (gChordSineLUTReady)
		return;
	/* Bhaskara I sine approximation: sin(pi*t) ≈ 16t(1-t) / (5 - 4t(1-t)),
	 * t in [0,1]. Better than 0.2% — clean enough that error stays well
	 * below the noise floor of any plausible 16/24-bit capture. */
	for (UInt32 i = 0; i < kChordLUTSize; i++) {
		float t = static_cast<float>(i) / static_cast<float>(kChordLUTSize);
		float s;
		if (t < 0.5f) {
			float u = t * 2.0f;        /* [0,1] over first half */
			s = 16.0f * u * (1.0f - u) / (5.0f - 4.0f * u * (1.0f - u));
		} else {
			float u = (t - 0.5f) * 2.0f; /* [0,1] over second half */
			s = -16.0f * u * (1.0f - u) / (5.0f - 4.0f * u * (1.0f - u));
		}
		gChordSineLUT[i] = s;
	}
	OSSynchronizeIO();
	gChordSineLUTReady = 1;
}

static inline float chordSineFromPhase(UInt32 phase)
{
	UInt32 idx = (phase >> (32U - kChordLUTBits)) & kChordLUTMask;
	UInt32 next = (idx + 1U) & kChordLUTMask;
	UInt32 fracBits = 32U - kChordLUTBits;
	UInt32 frac = phase & ((1U << fracBits) - 1U);
	float a = gChordSineLUT[idx];
	float b = gChordSineLUT[next];
	float f = static_cast<float>(frac) / static_cast<float>(1U << fracBits);
	return a + (b - a) * f;
}

static inline float chordNextFrame(UInt32 *phases, UInt32 sampleRate)
{
	float sum = 0.0f;
	for (int k = 0; k < 5; k++) {
		UInt64 step = (static_cast<UInt64>(kChordFreqs[k]) << 32) / sampleRate;
		phases[k] += static_cast<UInt32>(step);
		sum += chordSineFromPhase(phases[k]);
	}
	/* Each sine is in [-1,1]; 5 of them can constructively peak at 5.0.
	 * Scale by 0.18 → worst-case peak 0.9, headroom for quantization. */
	return sum * 0.18f;
}

void VoodooHDAEngine::fillDiagnosticChordBuffer(void *sampleBuf, UInt32 firstSampleFrame,
                                                UInt32 numSampleFrames,
                                                const IOAudioStreamFormat *streamFormat)
{
	if (!sampleBuf || !streamFormat || !mChannel)
		return;

	chordEnsureLUT();

	UInt32 channels = streamFormat->fNumChannels;
	if (!channels)
		return;
	UInt32 bitWidth = streamFormat->fBitWidth;
	UInt32 sampleRate = (getSampleRate() && getSampleRate()->whole)
	                    ? getSampleRate()->whole : 48000U;
	UInt32 firstSample = firstSampleFrame * channels;

	UInt32 *phases = mChannel->diagnosticPhase;

	if (streamFormat->fSampleFormat == kIOAudioStreamSampleFormatLinearPCM &&
	    streamFormat->fNumericRepresentation == kIOAudioStreamNumericRepresentationSignedInt) {
		switch (bitWidth) {
		case 16: {
			SInt16 *out = reinterpret_cast<SInt16 *>(sampleBuf) + firstSample;
			for (UInt32 f = 0; f < numSampleFrames; f++) {
				float s = chordNextFrame(phases, sampleRate);
				SInt16 v = static_cast<SInt16>(s * 32767.0f);
				for (UInt32 c = 0; c < channels; c++)
					out[f * channels + c] = v;
			}
			return;
		}
		case 20:
		case 24: {
			UInt8 *out = reinterpret_cast<UInt8 *>(sampleBuf) + firstSample * 3;
			for (UInt32 f = 0; f < numSampleFrames; f++) {
				float s = chordNextFrame(phases, sampleRate);
				SInt32 v = static_cast<SInt32>(s * 8388607.0f); /* 2^23 - 1 */
				for (UInt32 c = 0; c < channels; c++) {
					UInt8 *p = out + (f * channels + c) * 3;
					p[0] = static_cast<UInt8>(v & 0xFFU);
					p[1] = static_cast<UInt8>((v >> 8) & 0xFFU);
					p[2] = static_cast<UInt8>((v >> 16) & 0xFFU);
				}
			}
			return;
		}
		case 32: {
			SInt32 *out = reinterpret_cast<SInt32 *>(sampleBuf) + firstSample;
			for (UInt32 f = 0; f < numSampleFrames; f++) {
				float s = chordNextFrame(phases, sampleRate);
				SInt32 v = static_cast<SInt32>(s * 2147483647.0f);
				for (UInt32 c = 0; c < channels; c++)
					out[f * channels + c] = v;
			}
			return;
		}
		default:
			break;
		}
	} else if (streamFormat->fSampleFormat == kIOAudioStreamSampleFormatLinearPCM &&
	           streamFormat->fNumericRepresentation == kIOAudioStreamNumericRepresentationIEEE754Float &&
	           bitWidth == 32) {
		float *out = reinterpret_cast<float *>(sampleBuf) + firstSample;
		for (UInt32 f = 0; f < numSampleFrames; f++) {
			float s = chordNextFrame(phases, sampleRate);
			for (UInt32 c = 0; c < channels; c++)
				out[f * channels + c] = s;
		}
		return;
	}
}

bool VoodooHDAEngine::diagnosticSkipsErase() const
{
    if (mDigitalStream) return false;
    
	UInt16 flags = diagnosticFlags();
	return diagnosticsEnabled() &&
	       (((flags & kVoodooHDADiagSkipErase) != 0) ||
	        ((flags & kVoodooHDADiagFreezeBuffer) != 0));
}

bool VoodooHDAEngine::diagnosticBypassesProcessing() const
{
	return diagnosticsEnabled() &&
	       ((diagnosticFlags() & kVoodooHDADiagBypassProcessing) != 0);
}

bool VoodooHDAEngine::diagnosticFreezesBuffer() const
{
	return diagnosticsEnabled() &&
	       ((diagnosticFlags() & kVoodooHDADiagFreezeBuffer) != 0);
}

bool VoodooHDAEngine::diagnosticPrimesBufferOnStart() const
{
	return diagnosticsEnabled() &&
	       ((diagnosticFlags() & kVoodooHDADiagPrimeBufferOnStart) != 0);
}

void VoodooHDAEngine::resetDiagnosticState()
{
	if (!mChannel)
		return;

	mChannel->diagnosticPhase[0] = 0;
	mChannel->diagnosticPhase[1] = 0;
	mChannel->diagnosticBufferPrimed = false;
	mChannel->diagnosticClipCalls = 0;
	mChannel->diagnosticMixToneFills = 0;
	mChannel->diagnosticDirectToneFills = 0;
	mChannel->diagnosticEraseCalls = 0;
	mChannel->diagnosticEraseSkips = 0;
	mChannel->diagnosticLastFirstFrame = 0;
	mChannel->diagnosticLastNumFrames = 0;
}

float VoodooHDAEngine::nextDiagnosticSample(UInt32 channelIndex)
{
	static const UInt32 freqs[2] = { 440U, 660U };
	UInt32 sampleRate;
	UInt32 phaseIndex;
	UInt64 step;
	UInt32 phase;
	UInt32 ramp;
	SInt32 triangle;

	if (!mChannel)
		return 0.0f;

	sampleRate = (getSampleRate() && getSampleRate()->whole) ? getSampleRate()->whole : 48000U;
	phaseIndex = channelIndex & 1U;
	step = ((static_cast<UInt64>(freqs[phaseIndex]) << 32) / sampleRate);
	if (step == 0)
		step = 1;

	mChannel->diagnosticPhase[phaseIndex] += static_cast<UInt32>(step);
	phase = mChannel->diagnosticPhase[phaseIndex];
	ramp = phase >> 16;
	triangle = (ramp < 32768U) ? static_cast<SInt32>(ramp)
	                           : static_cast<SInt32>(65535U - ramp);

	return (((static_cast<float>(triangle) / 16383.5f) - 1.0f) * 0.35f);
}

void VoodooHDAEngine::fillDiagnosticMixBuffer(float *floatMixBuf, UInt32 numSamples, UInt32 numChannels)
{
	if (!floatMixBuf || !numChannels)
		return;

	for (UInt32 i = 0; i < numSamples; i += numChannels) {
		for (UInt32 ch = 0; ch < numChannels; ch++)
			floatMixBuf[i + ch] = nextDiagnosticSample(ch);
	}
}

IOReturn VoodooHDAEngine::fillDiagnosticSampleBuffer(void *sampleBuf, UInt32 firstSampleFrame,
		UInt32 numSampleFrames, const IOAudioStreamFormat *streamFormat)
{
	UInt32 channels;
	UInt32 bitWidth;
	UInt32 bitDepth;
	UInt32 firstSample;
	UInt32 numSamples;
	UInt32 padBits;

	if (!sampleBuf || !streamFormat)
		return kIOReturnBadArgument;

	channels = streamFormat->fNumChannels;
	if (!channels)
		return kIOReturnBadArgument;

	bitWidth = streamFormat->fBitWidth;
	bitDepth = streamFormat->fBitDepth ? streamFormat->fBitDepth : bitWidth;
	firstSample = firstSampleFrame * channels;
	numSamples = numSampleFrames * channels;
	padBits = (bitWidth > bitDepth) ? (bitWidth - bitDepth) : 0;

	if (streamFormat->fSampleFormat == kIOAudioStreamSampleFormatLinearPCM &&
	    streamFormat->fNumericRepresentation == kIOAudioStreamNumericRepresentationSignedInt) {
		switch (bitWidth) {
			case 8: {
				SInt8 *outBuf = reinterpret_cast<SInt8 *>(sampleBuf) + firstSample;
				for (UInt32 i = 0; i < numSamples; i++)
					outBuf[i] = static_cast<SInt8>(nextDiagnosticSample(i) * 127.0f);
				return kIOReturnSuccess;
			}
			case 16: {
				SInt16 *outBuf = reinterpret_cast<SInt16 *>(sampleBuf) + firstSample;
				for (UInt32 i = 0; i < numSamples; i++)
					outBuf[i] = static_cast<SInt16>(nextDiagnosticSample(i) * 32767.0f);
				return kIOReturnSuccess;
			}
			case 32: {
				SInt32 *outBuf = reinterpret_cast<SInt32 *>(sampleBuf) + firstSample;
				const UInt32 maxValue = (bitDepth >= 31) ? 0x7fffffffU : ((1U << (bitDepth - 1)) - 1U);
				for (UInt32 i = 0; i < numSamples; i++) {
					SInt32 sample = static_cast<SInt32>(nextDiagnosticSample(i) * static_cast<float>(maxValue));
					if (padBits)
						sample *= static_cast<SInt32>(1U << padBits);
					outBuf[i] = sample;
				}
				return kIOReturnSuccess;
			}
			default:
				break;
		}
	} else if (streamFormat->fSampleFormat == kIOAudioStreamSampleFormatLinearPCM &&
	           streamFormat->fNumericRepresentation == kIOAudioStreamNumericRepresentationIEEE754Float &&
	           bitWidth == 32 && bitDepth == 32) {
		float *outBuf = reinterpret_cast<float *>(sampleBuf) + firstSample;
		for (UInt32 i = 0; i < numSamples; i++)
			outBuf[i] = nextDiagnosticSample(i);
		return kIOReturnSuccess;
	}

	return kIOReturnUnsupported;
}

void VoodooHDAEngine::primeDiagnosticBuffer()
{
	const IOAudioStreamFormat *streamFormat;

	if (!diagnosticsEnabled() || !mStream || !mChannel || !mChannel->buffer || !mNumSampleFrames)
		return;

	streamFormat = mStream->getFormat();
	if (!streamFormat)
		return;

	if (fillDiagnosticSampleBuffer(reinterpret_cast<void *>(mChannel->buffer->virtAddr), 0,
	                               mNumSampleFrames, streamFormat) == kIOReturnSuccess) {
		mChannel->diagnosticBufferPrimed = true;
		mChannel->diagnosticDirectToneFills++;
		mChannel->diagnosticLastFirstFrame = 0;
		mChannel->diagnosticLastNumFrames = mNumSampleFrames;
		if (mDigitalStream)
			mDigitalStream->noteClippedPosition(mNumSampleFrames);
	}
}

/******************************************************************************************/
/******************************************************************************************/

bool VoodooHDAEngine::initWithChannel(Channel *channel)
{
	bool result = false;

//	logMsg("VoodooHDAEngine[%p]::init\n", this);

	if (!channel || !super::init(NULL))
		goto done;

	mChannel = channel;
	mDigitalStream = NULL;

	result = true;
done:
	return result;
}

void VoodooHDAEngine::free()
{
//	logMsg("VoodooHDAEngine[%p]::free\n", this);

	RELEASE(mStream);

	RELEASE(mSelControl);
	if (mDigitalStream) {
		if (mDevice && mDevice->mGFXController)
			mDevice->mGFXController->unregisterStream(mChannel, mDigitalStream);
		delete mDigitalStream;
		mDigitalStream = NULL;
	}

	mDevice = NULL;

	super::free();
}

static
UInt32 pinConfigToSelection(UInt32 pinConfig)
{
	switch (pinConfig & HDA_CONFIG_DEFAULTCONF_DEVICE_MASK) {
		case HDA_CONFIG_DEFAULTCONF_DEVICE_LINE_OUT:
		case HDA_CONFIG_DEFAULTCONF_DEVICE_LINE_IN:
			return kIOAudioSelectorControlSelectionValueLine;
		case HDA_CONFIG_DEFAULTCONF_DEVICE_SPEAKER:
			return kIOAudioSelectorControlSelectionValueExternalSpeaker;
		case HDA_CONFIG_DEFAULTCONF_DEVICE_HP_OUT:
			return kIOAudioSelectorControlSelectionValueHeadphones;
		case HDA_CONFIG_DEFAULTCONF_DEVICE_CD:
			return kIOAudioSelectorControlSelectionValueCD;
		case HDA_CONFIG_DEFAULTCONF_DEVICE_SPDIF_OUT:
		case HDA_CONFIG_DEFAULTCONF_DEVICE_SPDIF_IN:
			return kIOAudioSelectorControlSelectionValueSPDIF;
		case HDA_CONFIG_DEFAULTCONF_DEVICE_DIGITAL_OTHER_OUT:
		case HDA_CONFIG_DEFAULTCONF_DEVICE_DIGITAL_OTHER_IN:
			return kIOAudioDeviceTransportTypeHdmi;
		case HDA_CONFIG_DEFAULTCONF_DEVICE_MIC_IN:
			return kIOAudioSelectorControlSelectionValueExternalMicrophone;
		default:
			return kIOAudioSelectorControlSelectionValueNone;
	}
}

static
UInt32 selectionAndDirectionToTerminalType(UInt32 selection, IOAudioStreamDirection direction)
{
	switch (selection) {
		case kIOAudioSelectorControlSelectionValueLine:
			return EXTERNAL_LINE_CONNECTOR;
		case kIOAudioSelectorControlSelectionValueExternalSpeaker:
			return OUTPUT_DESKTOP_SPEAKER;
		case kIOAudioSelectorControlSelectionValueHeadphones:
			return OUTPUT_HEADPHONES;
		case kIOAudioSelectorControlSelectionValueCD:
			return EMBEDDED_CD_PLAYER;
		case kIOAudioSelectorControlSelectionValueSPDIF:
			return EXTERNAL_SPDIF_INTERFACE;
		case kIOAudioSelectorControlSelectionValueExternalMicrophone:
			return INPUT_DESKTOP_MICROPHONE;
		case kIOAudioDeviceTransportTypeHdmi:
			return EXTERNAL_DIGITAL_AUDIO_INTERFACE;
	}
	return direction == kIOAudioStreamDirectionInput ? INPUT_NULL : OUTPUT_NULL;
}

__attribute__((visibility("hidden")))
const char *VoodooHDAEngine::getPortName()
{
	UInt32 numDacs;
	nid_t dacNid, outputNid;
	Widget *widget;
	AudioAssoc *assoc;

	if (mPortName)
		return mPortName;

	mDevice->lock(__FUNCTION__);

	for (numDacs = 0; (numDacs < 16) && (mChannel->io[numDacs] != -1); numDacs++){
		//Slice - to trace
/*		if (mVerbose > 2) {
			logMsg(" io[%d] in assoc %d = %d\n", (int)numDacs, (int)mChannel->assocNum, (int)mChannel->io[numDacs]);
		}*/
	}
	if (numDacs == 0)
		goto done;
	if (numDacs > 1 && mChannel->caps.channels > 2) {
		switch (mChannel->caps.channels) {
			case 4:
				mPortName = "4CH (Green+Black Rear)";
				mPortType = kIOAudioSelectorControlSelectionValueLine;
				break;
			case 6:
				mPortName = "5.1CH (Green+Orange+Black Rear)";
				mPortType = kIOAudioSelectorControlSelectionValueLine;
				break;
			case 8:
				mPortName = "7.1CH (Green+Orange+Black+Grey Rear)";
				mPortType = kIOAudioSelectorControlSelectionValueLine;
				break;
			default:
			    mPortName = "Complex output";
				break;
		}
		goto done;
	}
	
	dacNid = mChannel->io[0];

	assoc = &mChannel->funcGroup->audio.assocs[mChannel->assocNum];
	outputNid = -1;
	for (int n = 0; (n < 16) && assoc->dacs[n]; n++)
		if (assoc->dacs[n] == dacNid)
			outputNid = assoc->pins[n];
	if (outputNid == -1)
		goto done;

	widget = mDevice->widgetGet(mChannel->funcGroup, outputNid);
	if (!widget)
		goto done;

	//Slice - advanced PinName
	{
		const char *pinName = &widget->name[5];
		/* Use codec name (e.g. "Realtek ALC897", "Intel Raptor Lake HDMI")
		 * instead of controller name — the controller name is the same for
		 * all codecs on the same HDA bus, which is misleading when Realtek
		 * analog outputs show as "Intel: Headphones". */
		const char *codecName = NULL;
		if (mChannel->funcGroup && mChannel->funcGroup->codec)
			codecName = VoodooHDADevice::findCodecName(mChannel->funcGroup->codec);
		if (!codecName)
			codecName = mDevice->mControllerName;
		if (codecName) {
			snprintf(mPortNameBuf, sizeof(mPortNameBuf), "%s: %s", codecName, pinName);
			mPortName = mPortNameBuf;
		} else {
			mPortName = pinName;
		}
	}
	mPortType = pinConfigToSelection(widget->pin.config);
done:
	mDevice->unlock(__FUNCTION__);

	if (!mPortName)
		mPortName = "Not connected";
	if (!mPortType)
		mPortType = kIOAudioSelectorControlSelectionValueNone;
	
	return mPortName;
}
/*
const char *VoodooHDAEngine::getDescription(char* callerBuffer, unsigned length)
{
	if (!callerBuffer)
		return 0;
	PcmDevice *pcmDevice = mChannel->pcmDevice;
	snprintf(callerBuffer, length, "%s PCM #%d", (pcmDevice->digital ? "Digital" : "Analog"),
			 pcmDevice->index);
	return callerBuffer;
}

void VoodooHDAEngine::identifyPaths()
{
	IOAudioStreamDirection direction = getEngineDirection();
	FunctionGroup *funcGroup = mChannel->funcGroup;

	for (int i = funcGroup->startNode; i < funcGroup->endNode; i++) {
		Widget *widget;
		UInt32 config;
		const char *devType, *connType;

		widget = mDevice->widgetGet(funcGroup, i);
		if (!widget || widget->enable == 0)
			continue;
		if (((direction == kIOAudioStreamDirectionOutput) &&
				(widget->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_PIN_COMPLEX)) ||
				((direction == kIOAudioStreamDirectionInput) &&
				(widget->type != HDA_PARAM_AUDIO_WIDGET_CAP_TYPE_AUDIO_INPUT)))
			continue;
		if (widget->bindAssoc != mChannel->assocNum)
			continue;
		config = widget->pin.config;
		devType = gDeviceTypes[HDA_CONFIG_DEFAULTCONF_DEVICE(config)];
		connType = gConnTypes[HDA_CONFIG_DEFAULTCONF_CONNECTIVITY(config)];
    if (mVerbose > 3) {
      logMsg("[nid %d] devType = %s, connType = %s\n", i, devType, connType);
    }
	}
}

UInt32 VoodooHDAEngine::getNumCtls(UInt32 dev)
{
	UInt32 numCtls = 0;
	AudioControl *control;

	for (int i = 0; (control = mDevice->audioCtlEach(mChannel->funcGroup, i)); i++) {
		if ((control->enable == 0) || !(control->ossmask & (1 << dev)))
			continue;
		if (!((control->widget->bindAssoc == mChannel->assocNum) || (control->widget->bindAssoc == -2)))
			continue;
		numCtls++;
	}

	return numCtls;
}
*/
__attribute__((visibility("hidden")))
UInt64 VoodooHDAEngine::getMinMaxDb(UInt32 mask)
{
	AudioControl *control;
	IOFixed minDb, maxDb;

	minDb = ~0L;
	maxDb = ~0L;

	// xxx: we currently use the values from the first found control (ie. amplifier settings)

	for (int i = 0; (control = mDevice->audioCtlEach(mChannel->funcGroup, i)); i++) {
		if ((control->enable == 0) || !(control->ossmask & mask))
			continue;
		if (!((control->widget->bindAssoc == mChannel->assocNum) || (control->widget->bindAssoc == -2)))
			continue;
		if (control->step <= 0)
			continue;
		minDb = ((0 - control->offset) * (control->size + 1) / 4) << 16;
		maxDb = ((control->step - control->offset) * (control->size + 1) / 4) << 16;
		break;
	}

	return ((UInt64) minDb << 32) | maxDb;
}

__attribute__((visibility("hidden")))
bool VoodooHDAEngine::haveDigitalMuteControl(UInt32 mask)
{
	AudioControl *control;

	for (int i = 0; (control = mDevice->audioCtlEach(mChannel->funcGroup, i)); i++) {
		if ((control->enable == 0) || !(control->ossmask & mask))
			continue;
		if (!((control->widget->bindAssoc == mChannel->assocNum) || (control->widget->bindAssoc == -2)))
			continue;
		if (control->mute)
			return true;
	}

	return false;
}


/*
bool VoodooHDAEngine::validateOssDev(int ossDev)
{
	return ((ossDev >= 0) && (ossDev < SOUND_MIXER_NRDEVICES));
}

const char *VoodooHDAEngine::getOssDevName(int ossDev)
{
	if (validateOssDev(ossDev))
		return gOssDeviceTypes[ossDev];
	else
		return "invalid";
}

void VoodooHDAEngine::setActiveOssDev(int ossDev)
{
	logMsg("setting active OSS device: %d (%s)\n", ossDev, getOssDevName(ossDev));
	ASSERT(validateOssDev(ossDev));
	mActiveOssDev = ossDev;
}

int VoodooHDAEngine::getActiveOssDev()
{
	int ossDev = mActiveOssDev;
	logMsg("active OSS device: %d (%s)\n", ossDev, getOssDevName(ossDev));
	ASSERT(validateOssDev(ossDev));
	return ossDev;
}
*/
bool VoodooHDAEngine::initHardware(IOService *provider)
{
	bool result = false;

    IOLog("VoodooHDAEngine[%p]::initHardware\n", this);

	if (!super::initHardware(provider)) {
		errorMsg("error: IOAudioEngine::initHardware failed\n");
		goto done;
	}
	mDevice = OSDynamicCast(VoodooHDADevice, provider);
	ASSERT(mDevice);

	mVerbose = mDevice->mVerbose;
	getPortName();
	if (mChannel->pcmDevice && mChannel->pcmDevice->digital >= 2 &&
	    getEngineDirection() == kIOAudioStreamDirectionOutput && mDevice->mGFXController) {
		mDigitalStream = new VoodooGFXHDAStream;
		if (!mDigitalStream || !mDigitalStream->init(mDevice->mGFXController, this, mChannel)) {
			errorMsg("error: couldn't initialize VoodooGFXHDAStream\n");
			goto done;
		}
		mDevice->mGFXController->registerStream(mChannel, mDigitalStream);
	}

    IOLog("setDesc portName = %s\n", mPortName);
	setDescription(mPortName);

	/* Initial offsets at 48 kHz default; recalculated on every performFormatChange(). */
	recalculateSampleOffsets(48000);
	if (version_major > 10)			/* newer than SnowLeopard */
 	  setClockIsStable(true);
	else
	  setProperty(kIOAudioEngineClockIsStableKey, 1ULL, 32U);

	if (!createAudioStream()) {
		errorMsg("error: createAudioStream failed\n");
		goto done;
	}
	emptyStream = true;
	if (!createAudioControls()) {
		errorMsg("error: createAudioControls failed\n");
		goto done;
	}
	mChannel->vectorize  = mDevice->vectorize;
	mChannel->noiseLevel = mDevice->noiseLevel;
	mChannel->useStereo  = mDevice->useStereo;
	mChannel->StereoBase = mDevice->StereoBase;
	resetDiagnosticState();
	
	result = true;
done:
	if (!result)
		stop(provider);

	return result;
}

__attribute__((visibility("hidden")))
bool VoodooHDAEngine::createAudioStream()
{
	bool result = false;
	bool isDigital;
	IOAudioStreamDirection direction;
	IOAudioSampleRate minSampleRate, maxSampleRate;
	UInt8 *sampleBuffer;
	UInt32 channels;

	ASSERT(!mStream);

//	logMsg("VoodooHDAEngine[%p]::createAudioStream\n", this);

//	logMsg("recDevMask: 0x%lx, devMask: 0x%lx\n", mChannel->pcmDevice->recDevMask,
//			mChannel->pcmDevice->devMask);

	direction = getEngineDirection();

//	logMsg("formats: ");
//	for (UInt32 n = 0; (n < 8) && mChannel->formats[n]; n++)
//		logMsg("0x%lx ", mChannel->formats[n]);
//	logMsg("\n");

	if (!HDA_PARAM_SUPP_STREAM_FORMATS_PCM(mChannel->supStreamFormats)) {
		errorMsg("error: channel doesn't support PCM stream format\n");
		goto done;
	}

//	logMsg("sample rates: ");
//	for (UInt32 n = 0; (n < 16) && mChannel->pcmRates[n]; n++)
//		logMsg("%ld ", mChannel->pcmRates[n]);
//	logMsg("(min: %ld, max: %ld)\n", mChannel->caps.minSpeed, mChannel->caps.maxSpeed);

//	ASSERT(mChannel->caps.minSpeed);
//	ASSERT(mChannel->caps.maxSpeed);
//	ASSERT(mChannel->caps.minSpeed <= mChannel->caps.maxSpeed);

	minSampleRate.whole = mChannel->caps.minSpeed;
	minSampleRate.fraction = 0;
	maxSampleRate.whole = mChannel->caps.maxSpeed;
	maxSampleRate.fraction = 0;
	channels = mChannel->caps.channels;

	/*
	 * HDMI/DP monitors typically support only 2-channel stereo.
	 * ATI codecs report 8 channels per association but sending 8ch
	 * to a 2ch sink produces noise.  Cap to 2 for digital outputs.
	 * (AV receivers with 5.1/7.1 can be supported later via ELD.)
	 */
	isDigital = (mChannel->funcGroup->audio.assocs[mChannel->assocNum].digital != 0);
	if (isDigital && channels > 2)
		channels = 2;

	logMsg("(min: %ld, max: %ld) channels=%d%s\n", (long int)mChannel->caps.minSpeed, (long int)mChannel->caps.maxSpeed, (int)channels, isDigital ? " (digital, capped to 2)" : "");
	sampleBuffer = (UInt8 *) mChannel->buffer->virtAddr;
	mBufferSize = mChannel->buffer ? static_cast<UInt32>(mChannel->buffer->size) : HDA_BUFSZ_DEFAULT;
	if (!createAudioStream(direction, sampleBuffer, mBufferSize, mChannel->pcmRates,
                           mChannel->supPcmSizeRates, mChannel->supStreamFormats, channels)) {
		errorMsg("error: createAudioStream failed channels=%d\n", (int)channels);
		goto done;
	}
	publishChannelLayout(direction, channels);
	result = true;
done:
	return result;
}

__attribute__((visibility("hidden")))
bool VoodooHDAEngine::createAudioStream(IOAudioStreamDirection direction, void *sampleBuffer,
		UInt32 sampleBufferSize, UInt32 *pcmRates,
		UInt32 supPcmSizeRates, UInt32 supStreamFormats, UInt32 channels)
{
	bool result = false;
	bool isDigital;
	UInt32 defaultSampleRate = 0U;

	IOAudioStreamFormat format = {
		channels,										// number of channels
		0,                                              // sample format (to be filled in)
		kIOAudioStreamNumericRepresentationSignedInt,	// numeric format
		0,												// bit depth (to be filled in)
		0,												// bit width (to be filled in)
		kIOAudioStreamAlignmentLowByte,					// low byte aligned
		kIOAudioStreamByteOrderLittleEndian,			// little endian
		true,											// format is mixable
		0												// driver-defined tag
	};

    IOAudioStreamFormatExtension formatEx = {
		kFormatExtensionCurrentVersion,					// version
		0,                                              // flags
		0,											    // frames per packet (to be filled in)
		0												// bytes per packet (to be filled in)
	};
    
    IOAudioSampleRate sampleRate = {
        0,
        0
    };
    
	ASSERT(!mStream);

//	logMsg("VoodooHDAEngine[%p]::createAudioStream(%d, %p, %ld)\n", this, direction, sampleBuffer,
//			sampleBufferSize);

	mStream = new IOAudioStream;
	if (!mStream->initWithAudioEngine(this, direction, 1)) {
		errorMsg("error: IOAudioStream::initWithAudioEngine failed\n");
		goto done;
	}

	mStream->setSampleBuffer(sampleBuffer, sampleBufferSize); // also creates mix buffer
	isDigital = (mChannel->funcGroup->audio.assocs[mChannel->assocNum].digital != 0);

    for(int i = 0; pcmRates[i]; i++) {
        sampleRate.whole = pcmRates[i];
		if (sampleRate.whole <= 48000U && defaultSampleRate < sampleRate.whole)
			defaultSampleRate = sampleRate.whole;
		/*
		 * Raw AC3 is packetized in 1536-sample-frame packets.
		 *   - packets are made of 16-bit words, big endian.
		 *   - allowed sample rate for target audio is 32 KHz, 44.1 KHz or 48 KHz.
		 *   - supports multichannel.
		 *   - packet size is bitrate and sample-rate dependent.  Varies from 128 bytes to 3840 bytes.
		 * AC3 is encapsulated in S/PDIF based on IEC 61937 as follows
		 *   - Uses 16-bit sample depth, little endian (byte order is reversed during encapsulation.)
		 *   - Has an 8-byte header, followed by the raw AC3 packet, padded with zeros to a fill a
		 *     pseudo linear-PCM span of 1536 sample-frames.
		 *   - Using 16-bit sample depth, 2-channel gives 4 bytes/sample-frame, which allows up to
		 *     6144 bytes - enough to fit in all sized AC3 packets.
		 *   - The standard allows using twice or 4-times the audio sample rate, but we use the same
		 *     sample rate, since it can fit all sized AC3 packets.
		 *   - Client must deliver AC3 encapsulated in S/PDIF (encapsulation is not done in clipOutputSamples).
		 */
        if(HDA_PARAM_SUPP_STREAM_FORMATS_AC3(supStreamFormats) && sampleRate.whole >= 32000U && sampleRate.whole <= 48000U) {
            format.fNumChannels = 2;
            format.fBitDepth = 16;
            format.fBitWidth = 16;
            format.fSampleFormat = kIOAudioStreamSampleFormat1937AC3;
            formatEx.fFramesPerPacket = 1536;
            formatEx.fBytesPerPacket = formatEx.fFramesPerPacket * format.fNumChannels * (format.fBitWidth / 8);
            format.fIsMixable = false;
            mStream->addAvailableFormat(&format, &formatEx, &sampleRate, &sampleRate);
            format.fNumChannels = channels;
        }
        format.fSampleFormat = kIOAudioStreamSampleFormatLinearPCM;
        formatEx.fFramesPerPacket = 1;
		format.fIsMixable = true;
	if (HDA_PARAM_SUPP_PCM_SIZE_RATE_16BIT(supPcmSizeRates)) {
		format.fBitDepth = 16;
		format.fBitWidth = 16;
            formatEx.fBytesPerPacket = format.fNumChannels * (format.fBitWidth / 8);
            mStream->addAvailableFormat(&format, &formatEx, &sampleRate, &sampleRate);
	}
	if (HDA_PARAM_SUPP_PCM_SIZE_RATE_24BIT(supPcmSizeRates)) {
		format.fBitDepth = 24;
		format.fBitWidth = 32;
            formatEx.fBytesPerPacket = format.fNumChannels * (format.fBitWidth / 8);
            mStream->addAvailableFormat(&format, &formatEx, &sampleRate, &sampleRate);
	} else if (isDigital) {
		/* HDMI/DP codecs are pass-through: the HDA PCM cap register may
		 * report only 16-bit, but the HDMI link carries 24-bit fine.
		 * AppleGFXHDA uses 24-bit on the same hardware. */
		IOAudioStreamFormat fmt24 = format;
		IOAudioStreamFormatExtension fmtEx24 = formatEx;
		fmt24.fBitDepth = 24;
		fmt24.fBitWidth = 32;
		fmtEx24.fBytesPerPacket = fmt24.fNumChannels * (fmt24.fBitWidth / 8);
		mStream->addAvailableFormat(&fmt24, &fmtEx24, &sampleRate, &sampleRate);
	} else if (HDA_PARAM_SUPP_PCM_SIZE_RATE_20BIT(supPcmSizeRates)) {
		format.fBitDepth = 20;
		format.fBitWidth = 32;
            formatEx.fBytesPerPacket = format.fNumChannels * (format.fBitWidth / 8);
            mStream->addAvailableFormat(&format, &formatEx, &sampleRate, &sampleRate);
	}
		/*
		 * S/PDIF supports 16 - 24 bit sample depths.  32-bit sample depth is used
		 *   for "Software-formatted S/PDIF" as explained in the HDA specification
		 *   section 7.3.3.9.  A 32-bit word in S/PDIF consists of
		 *   - 4 bit preamble
		 *   - 24 bit sample
		 *   - 4 bits of valid/channel-status/user-defined/parity as explained in IEC 60958-3.
		 * If a bit depth of 16 - 24 is chosen, the codec formats the S/PDIF overhead bits
		 *   by itself ("Codec-Formatted S/PDIF").  If a 32-bit depth is used, software
		 *   is expected to generate the overhead bits.  Since clipOutputSamples does not
		 *   do this, but treats the whole 32 bits as an audio sample, don't support
		 *   32-bit depth in digital channels.
		 */
	if (!isDigital && HDA_PARAM_SUPP_PCM_SIZE_RATE_32BIT(supPcmSizeRates)) {
		format.fBitDepth = 32;
		format.fBitWidth = 32;
            formatEx.fBytesPerPacket = format.fNumChannels * (format.fBitWidth / 8);
            mStream->addAvailableFormat(&format, &formatEx, &sampleRate, &sampleRate);
        }
	}
        
	if (!format.fBitDepth || !format.fBitWidth) {
		errorMsg("error: couldn't find supported bit depth (16, 24, or 32-bit)\n");
		goto done;
    }

	sampleRate.whole = defaultSampleRate;
	setSampleRate(&sampleRate);

	addAudioStream(mStream);

	mStream->setFormat(&format, &formatEx); // set widest format as default

	mStream->setTerminalType(selectionAndDirectionToTerminalType(mPortType, direction));

	result = true;
done:
	RELEASE(mStream);

	return result;
}

__attribute__((visibility("hidden")))
bool VoodooHDAEngine::publishChannelLayout(IOAudioStreamDirection direction, UInt32 channels)
{
	OSArray* layout;
	OSNumber* n = NULL;

	if (!channels || channels > 8U)
		return false;
	layout = OSArray::withCapacity(channels);
	if (!layout)
		return false;
	if (channels >= 1U) {
		n = OSNumber::withNumber(kIOAudioChannelLabel_Left, 32);
		if (!n || !layout->setObject(n))
			goto error;
		n->release();
	}
	if (channels >= 2U) {
		n = OSNumber::withNumber(kIOAudioChannelLabel_Right, 32);
		if (!n || !layout->setObject(n))
			goto error;
		n->release();
	}
	if (channels >= 5U) {
		n = OSNumber::withNumber(kIOAudioChannelLabel_Center, 32);
		if (!n || !layout->setObject(n))
			goto error;
		n->release();
		n = OSNumber::withNumber(kIOAudioChannelLabel_LFEScreen, 32);
		if (!n || !layout->setObject(n))
			goto error;
		n->release();
		n = OSNumber::withNumber(kIOAudioChannelLabel_LeftSurround, 32);
		if (!n || !layout->setObject(n))
			goto error;
		n->release();
		if (channels >= 6U) {
			n = OSNumber::withNumber(kIOAudioChannelLabel_RightSurround, 32);
			if (!n || !layout->setObject(n))
				goto error;
			n->release();
		}
		if (channels >= 7U) {
			n = OSNumber::withNumber(kIOAudioChannelLabel_RearSurroundLeft, 32);
			if (!n || !layout->setObject(n))
				goto error;
			n->release();
		}
		if (channels >= 8U) {
			n = OSNumber::withNumber(kIOAudioChannelLabel_RearSurroundRight, 32);
			if (!n || !layout->setObject(n))
				goto error;
			n->release();
		}
	} else {
		if (channels >= 3U) {
			n = OSNumber::withNumber(kIOAudioChannelLabel_LeftSurround, 32);
			if (!n || !layout->setObject(n))
				goto error;
			n->release();
		}
		if (channels >= 4U) {
			n = OSNumber::withNumber(kIOAudioChannelLabel_RightSurround, 32);
			if (!n || !layout->setObject(n))
				goto error;
			n->release();
		}
	}
	n = NULL;
	if (!setProperty((direction == kIOAudioStreamDirectionInput ?
					  kIOAudioEngineInputChannelLayoutKey :
					  kIOAudioEngineOutputChannelLayoutKey),
					 layout))
		goto error;
	layout->release();
	return true;

error:
	if (n)
		n->release();
	layout->release();
	return false;
}

__attribute__((visibility("hidden")))
IOAudioStreamDirection VoodooHDAEngine::getEngineDirection()
{
	IOAudioStreamDirection direction;

	if (mChannel->direction == PCMDIR_PLAY) {
		ASSERT(mChannel->pcmDevice->playChanId >= 0);
		direction = kIOAudioStreamDirectionOutput;
	} else if (mChannel->direction == PCMDIR_REC) {
		ASSERT(mChannel->pcmDevice->recChanId >= 0);
		direction = kIOAudioStreamDirectionInput;
	} else {
		BUG("invalid direction");
	}
	
	if (mStream)
		ASSERT(mStream->getDirection() == direction);

	return direction;
}

__attribute__((visibility("hidden")))
int VoodooHDAEngine::getEngineId()
{
	if (getEngineDirection() == kIOAudioStreamDirectionOutput)
		return mChannel->pcmDevice->playChanId;
	else
		return mChannel->pcmDevice->recChanId;
}

IOReturn VoodooHDAEngine::performAudioEngineStart()
{
 //   resetDiagnosticState();
    mDevice->channelStart(mChannel);
    if (mDigitalStream)
        mDigitalStream->resetClipPosition(0);
    else
      resetClipPosition(mStream, 0); // 🔧 Сброс для аналога при микшировании
  
//  // 🔧 Полное обнуление DMA-буфера перед стартом (убирает "икание" от старых данных)
//  if (mChannel && mChannel->buffer)
//    bzero(reinterpret_cast<void *>(mChannel->buffer->virtAddr), mChannel->buffer->size);


    // Polaris DCE 11.2: фиксированная задержка стабильнее опроса SDLPIB
//    IODelay(1500);
  // меняем опять на адаптивный
  // Polaris HDMI: ждём, пока SDLPIB выйдет из 0 (физический старт DMA)
  int timeout = 200;
  while (timeout-- > 0) {
    if (mDevice->readData32(mChannel->off + HDAC_SDLPIB) != 0) break;
    IODelay(10);
  }

    // Пересчёт смещений по текущей частоте (критично после сна/смены трека)
    const IOAudioSampleRate *rate = getSampleRate();
    if (rate) recalculateSampleOffsets(rate->whole);

//    if (diagnosticPrimesBufferOnStart())
//        primeDiagnosticBuffer();

    takeTimeStamp(false);
    return kIOReturnSuccess;
}

IOReturn VoodooHDAEngine::performAudioEngineStop()
{
//    resetDiagnosticState();
    mDevice->channelStop(mChannel);
    
    // Дренаж аппаратного FIFO перед выключением
    IODelay(500);
  
  // 🔧 Сброс позиции клипа для предотвращения эхо/дублирования
  resetClipPosition(mStream, 0);
    if (mDigitalStream)
        mDigitalStream->resetClipPosition(0);
  
  // Очистка буфера после стопа (гарантирует чистый старт следующего трека)
  if (mChannel && mChannel->buffer)
    bzero(reinterpret_cast<void *>(mChannel->buffer->virtAddr), mChannel->buffer->size);

	return kIOReturnSuccess;
}
	
/*
 * Look up an Apple-style integer property by walking up the IOService
 * plane from this engine.  Mirrors AppleGFXHDAEngine::recalculateEngines
 * SampleOffset (decompile:0x1dd00-0x1ddfa) which calls
 * `IORegistryEntry::getProperty(name, IOService plane,
 * kIORegistryIterateRecursively | kIORegistryIterateParents)`.
 * Returns true and stores the value if a parent in the IOService plane
 * publishes the key; false if no parent has it (caller falls back to
 * its hardcoded default).
 *
 * Apple publishes these from AGDC / AppleGraphicsDeviceControl on the
 * GPU's IORegistryEntry; on a default install they may be absent, in
 * which case we keep our VoodooHDA-tuned defaults.
 */
bool VoodooHDAEngine::lookupAncestorPropertyU32(const char *name, UInt32 *out) const
{
	if (!name || !out)
		return false;

	OSObject *prop = const_cast<VoodooHDAEngine *>(this)->copyProperty(
	    name, gIOServicePlane,
	    kIORegistryIterateRecursively | kIORegistryIterateParents);
	if (!prop)
		return false;

	bool ok = false;
	if (OSNumber *num = OSDynamicCast(OSNumber, prop)) {
		*out = num->unsigned32BitValue();
		ok = true;
	} else if (OSData *data = OSDynamicCast(OSData, prop)) {
		if (data->getLength() >= sizeof(UInt32)) {
			*out = *reinterpret_cast<const UInt32 *>(data->getBytesNoCopy());
			ok = true;
		}
	}
	prop->release();
	return ok;
}

/* Mirror of Apple's AppleGFXHDAEngine::recalculateEnginesSampleOffset/Latency().
 * Called from initHardware() with a default rate, then again on every
 * performFormatChange() so the offsets scale with the actual sample rate.
 *
 * Formula: offset = roundup(rate * safetyCoeff_μs / 1_000_000) + base
 * Input offset is always analog-style (no large FIFO on the capture side).
 *
 * Apple at decompile 0x16fe6 (AppleGFXHDADriver::getOutputSafetyOffset)
 * computes `iVar1 = (rate * driver+0x1b8 + 999999) / 1000000; return iVar1
 * + driver+0x1cc;` where driver+0x1b8/+0x1cc are loaded at engine init
 * (decompile:0x1dd00-0x1ddfa) by walking the IOService plane and reading
 * `AdditionalOutputSafetyOffset` / `SampleOffsetPad` /
 * `SystemSpecificSampleOffsetPad` / `OutputSampleLatency` properties from
 * a parent (AGDC on supported macs).  We mirror that lookup here: read
 * the same keys via copyProperty walk and *add* what we find on top of
 * our hardcoded baseline (HDMI_SAFETY_US/HDMI_FIFO_BASE/HDMI_LATENCY_US).
 * If no parent publishes them, fall back to the baseline. */

void VoodooHDAEngine::recalculateSampleOffsets(UInt32 sampleRate)
{
    bool isDigital = (mChannel->funcGroup->audio.assocs[mChannel->assocNum].digital != 0);
    UInt32 safetyUs  = isDigital ? HDMI_SAFETY_US  : ANALOG_SAFETY_US;
    UInt32 latencyUs = isDigital ? HDMI_LATENCY_US : ANALOG_LATENCY_US;
    UInt32 base      = isDigital ? HDMI_FIFO_BASE  : 0;

    // Polaris/Vega HDMI Audio Controllers требуют увеличенный запас из-за меньшего FIFO
    UInt32 devId = (mDevice->mDeviceId >> 16) & 0xFFFF;
    if (isDigital && mDevice) {        
        if (devId == 0xAAF0 || devId == 0xAAF8 ||
            devId == 0xAB20 || devId == 0xAB28 || devId == 0xAA60) {
            safetyUs = 9000;  // ~9 ms вместо 5 ms
            base     = 128;   // удвоенный базовый FIFO
        }
        latencyUs = HDMI_LATENCY_US;
    }

    UInt32 outOffset = (sampleRate * safetyUs  + 999999) / 1000000 + base;
    UInt32 latency   = (sampleRate * latencyUs + 999999) / 1000000;
    UInt32 inOffset  = (sampleRate * ANALOG_SAFETY_US + 999999) / 1000000;

    setSampleOffset(outOffset);
    setOutputSampleOffset(outOffset);
    setInputSampleOffset(inOffset);
    setSampleLatency(latency);
    setOutputSampleLatency(latency);

    IOLog("VoodooHDAEngine: recalculateSampleOffsets: rate=%u %s outOffset=%u inOffset=%u latency=%u (devId=0x%04x)\n",
           (unsigned)sampleRate, isDigital ? "HDMI/DP" : "Analog",
           (unsigned)outOffset, (unsigned)inOffset, (unsigned)latency, devId);
}

UInt32 VoodooHDAEngine::getCurrentSampleFrame()
{
	/* AppleGFXHDAEngine::getCurrentSampleFrame clamps to [0, numSampleFrames):
	 * if frame >= numSampleFrames it returns 0, guarding against SDLPIB glitches. */
	UInt32 position;

	if (mDigitalStream)
		return mDigitalStream->getCurrentSampleFrame();

	position = static_cast<UInt32>(mDevice->channelGetPosition(mChannel));

	UInt32 frame = position / mSampleSize;
	return (frame < mNumSampleFrames) ? frame : 0;
}

void VoodooHDAEngine::resetClipPosition(IOAudioStream *audioStream, UInt32 clipSampleFrame)
{
	/* AppleGFXHDAEngine overrides resetClipPosition() for its digital stream path.
	 * Mirror that hook for HDMI/DP so IOAudioFamily clip/erase state is reset together
	 * with the controller-owned link-position state. */
	if (audioStream == mStream && mDigitalStream)
		mDigitalStream->resetClipPosition(clipSampleFrame);

	super::resetClipPosition(audioStream, clipSampleFrame);
}

bool VoodooHDAEngine::usesAppleGfxClipPath() const
{
	return mDigitalStream != NULL &&
	       mChannel != NULL &&
	       mChannel->direction == PCMDIR_PLAY &&
	       mChannel->pcmDevice != NULL &&
	       mChannel->pcmDevice->digital >= 2;
}

IOReturn VoodooHDAEngine::eraseOutputSamples(const void *mixBuf, void *sampleBuf, UInt32 firstSampleFrame,
											 UInt32 numSampleFrames, const IOAudioStreamFormat *streamFormat,
											 __unused IOAudioStream *audioStream)
{
	/* Match AppleGFXHDAEngine::eraseOutputSamples(): zero the float mix region and
	 * the published sample-buffer region explicitly rather than inheriting the base hook implicitly. */
	if (mChannel) {
		mChannel->diagnosticEraseCalls++;
		mChannel->diagnosticLastFirstFrame = firstSampleFrame;
		mChannel->diagnosticLastNumFrames = numSampleFrames;
	}
	if (diagnosticSkipsErase()) {
		if (mChannel)
			mChannel->diagnosticEraseSkips++;
		return kIOReturnSuccess;
	}

	if (mixBuf && streamFormat) {
		UInt32 firstSample = firstSampleFrame * streamFormat->fNumChannels;
		UInt32 numSamples = numSampleFrames * streamFormat->fNumChannels;
		bzero(const_cast<float *>(reinterpret_cast<const float *>(mixBuf)) + firstSample,
		      numSamples * sizeof(float));
	}

	if (sampleBuf && streamFormat) {
		UInt32 offset = firstSampleFrame * (streamFormat->fBitWidth / 8) * streamFormat->fNumChannels;
		UInt32 size = numSampleFrames * (streamFormat->fBitWidth / 8) * streamFormat->fNumChannels;
		bzero(reinterpret_cast<UInt8 *>(sampleBuf) + offset, size);
	}

	return kIOReturnSuccess;
}

IOReturn VoodooHDAEngine::performFormatChange(IOAudioStream *audioStream,
											  const IOAudioStreamFormat *newFormat,
											  const IOAudioSampleRate *newSampleRate)
{
	IOReturn result = kIOReturnError;
	int setResult;
	UInt32 ossFormat;

	// ASSERT(audioStream == mStream);

	logMsg("VoodooHDAEngine[%p]::peformFormatChange(%p, %p, %p)\n", this, audioStream, newFormat,
			newSampleRate);

	if (!newSampleRate)
		newSampleRate = getSampleRate();
	if (!newFormat && !newSampleRate) {
		errorMsg("warning: performFormatChange(%p) called with no effect\n", audioStream);
		return kIOReturnSuccess;
	}

	if (newFormat) {
	int channels = newFormat->fNumChannels;

        if(!channels) {
            channels = 2;
        }

			ossFormat = AFMT_STEREO;

        if (newFormat->fSampleFormat == kIOAudioStreamSampleFormat1937AC3) {
            ossFormat = AFMT_AC3;
		} else if (channels == 4) {
			ossFormat = SND_FORMAT(0, 4, 0);
		} else if (channels == 6) {
			ossFormat = SND_FORMAT(0, 6, 1);
		} else if (channels == 8) {
			ossFormat = SND_FORMAT(0, 8, 1);
		}

		ASSERT(newFormat->fNumericRepresentation == kIOAudioStreamNumericRepresentationSignedInt);
		ASSERT(newFormat->fAlignment == kIOAudioStreamAlignmentLowByte);
		ASSERT(newFormat->fByteOrder == kIOAudioStreamByteOrderLittleEndian);

        if(ossFormat != AFMT_AC3) {
          switch (newFormat->fBitDepth) {
            case 16:
                ASSERT(newFormat->fBitWidth == 16);
                ossFormat |= AFMT_S16_LE;
                break;
            case 20:
                ASSERT(newFormat->fBitWidth == 32);
                ossFormat |= AFMT_S32_LE;
                mChannel->bit32 = 2;
                break;
            case 24:
                ASSERT(newFormat->fBitWidth == 32);
                ossFormat |= AFMT_S32_LE;
                mChannel->bit32 = 3;
                break;
            case 32:
                ASSERT(newFormat->fBitWidth == 32);
                ossFormat |= AFMT_S32_LE;
                mChannel->bit32 = 4;
                break;
            default:
                errorMsg("unsupported bit depth %d\n", newFormat->fBitDepth);

          }
        }
		//IOLog("ossFormat=%08x\n", (unsigned int)ossFormat);
        
        // 🔧 КРИТИЧНО для AMD HDMI: форсируем 32-битный DMA-контейнер для цифровых потоков.
        // Это заставляет IOAudioEngine писать 8 байт/фрейм, старшие биты заполняются нулями.
        // Без этого GPU читает мусор из выровненных ячеек → сплошной хрип.
//        if (mDigitalStream) {
//            ossFormat &= ~AFMT_S16_LE;
//            ossFormat |= AFMT_S32_LE;
//            mChannel->bit32 = 4;
//        }
		
		setResult = mDevice->channelSetFormat(mChannel, ossFormat);
		logMsg("channelSetFormat(0x%08lx) for channel %d returned %d\n", static_cast<long unsigned int>(ossFormat), getEngineId(),
				setResult);
		if (setResult != 0) {
			errorMsg("error: couldn't set format 0x%lx (%d-bit depth)\n", (long unsigned int)ossFormat, newFormat->fBitDepth);
			goto done;
		}

			ASSERT(mBufferSize);
		  mSampleSize = channels * (newFormat->fBitWidth / 8);
        // AMD HDMI/DP требует 32-битного DMA-контейнера даже для 16-битного звука.
        // Принудительно выравниваем семплы в 4 байта для цифровых потоков.
        //UInt32 dmaWidth = mDigitalStream ? 32 : newFormat->fBitWidth;
        //UInt32 dmaWidth = (ossFormat & AFMT_S32_LE) ? 32 : newFormat->fBitWidth;
        //mSampleSize = channels * (dmaWidth / 8);
    
//    // Защита от некорректного выравнивания (встречается на некоторых HDMI-кодеках)
//    if (mBufferSize % mSampleSize != 0) {
//      mNumSampleFrames = mBufferSize / mSampleSize;
//      mChannel->slack = static_cast<UInt16>(mBufferSize - mNumSampleFrames * mSampleSize);
//    } else {
//      mNumSampleFrames = mBufferSize / mSampleSize;
//      mChannel->slack = 0;
//    }

        mNumSampleFrames = mBufferSize / mSampleSize;
        
        mChannel->slack = static_cast<UInt16>(mBufferSize - mNumSampleFrames * mSampleSize);
        // 🔧 Polaris/AMD HDMI требует точного выравнивания блоков.
        // slack ломает расчёт длины последнего дескриптора → периодический хрип.
//        if (mDigitalStream) {
//            mChannel->slack = 0;
//        } else {
//            mChannel->slack = static_cast<UInt16>(mBufferSize - mNumSampleFrames * mSampleSize);
//        }
        
        
        setNumSampleFramesPerBuffer(mNumSampleFrames);
        if (mDigitalStream)
            mDigitalStream->resetPositionState();

		logMsg("VoodooHDA::buffer size: %ld, channels: %d, bit depth: %d, # samp. frames: %ld\n", (long int)mBufferSize,
				channels, newFormat->fBitDepth, (long int)mNumSampleFrames);
	}

	if (newSampleRate) {
		setResult = mDevice->channelSetSpeed(mChannel, newSampleRate->whole);
//		logMsg("channelSetSpeed(%ld) for channel %d returned %d\n", newSampleRate->whole, getEngineId(),
//				setResult);
			if ((UInt32) setResult != newSampleRate->whole) {
				errorMsg("error: couldn't set sample rate %ld\n", (long int)newSampleRate->whole);
				goto done;
			}
			if (mDigitalStream)
				mDigitalStream->resetPositionState();
//			resetDiagnosticState();
			/* Recalculate sample offsets for the new rate, as Apple does in
			 * recalculateEnginesSampleOffset() / recalculateEnginesSampleLatency(). */
			recalculateSampleOffsets(newSampleRate->whole);
	}

	result = kIOReturnSuccess;
done:
	return result;
}

static
IOReturn SelectorChanged(OSObject*, IOAudioControl*, SInt32, SInt32)
{
	return kIOReturnSuccess;
}

__attribute__((visibility("hidden")))
bool VoodooHDAEngine::createAudioControls()
{
	bool			result = false;
	IOAudioControl	*control;
	IOAudioStreamDirection direction;
	UInt32			usage;
	UInt64			minMaxDb;
	IOFixed			minDb,
					maxDb;
	int				initOssDev, initOssMask, idupper;
	direction = getEngineDirection();
	if (direction == kIOAudioStreamDirectionOutput) {
		usage = kIOAudioControlUsageOutput;
		initOssDev = SOUND_MIXER_VOLUME;
		initOssMask = SOUND_MASK_VOLUME;
	}	
	else if (direction == kIOAudioStreamDirectionInput) {
		usage = kIOAudioControlUsageInput;
		initOssDev = SOUND_MIXER_MIC;
		initOssMask = SOUND_MASK_MIC | SOUND_MASK_MONITOR;
	}
	else {
		errorMsg("uknown direction\n");
		goto Done;
	}

	idupper = mChannel->streamId << 16;

	if (mChannel->funcGroup->audio.assocs[mChannel->assocNum].digital) {
		/*
		 * Some digital pin complexes have mute control
		 */
		if (haveDigitalMuteControl(initOssMask))
			goto createMuteControl;
		else
			goto createSelectorControl;
	}

	minMaxDb = getMinMaxDb(initOssMask);
	minDb = (IOFixed) (minMaxDb >> 32);
	maxDb = (IOFixed) (minMaxDb & ~0UL);
//	logMsg("minDb: %d (%08lx), maxDb: %d (%08lx)\n", (SInt16) (minDb >> 16), minDb,
//		   (SInt16) (maxDb >> 16), maxDb);
	if ((minDb == ~0L) || (maxDb == ~0L)) {
		//logMsg("warning: found invalid min/max dB (using default -22.5 -> 0.0dB range)\n"); //-22.5 -> 0.0
		minDb = static_cast<int>(static_cast<unsigned>(-22) << 16) + (65536 / 2);
		maxDb = 0 << 16;
	}
	
	/* Create Volume controls */
	/* Left channel */
	control = IOAudioLevelControl::createVolumeControl(mDevice->mMixerDefaults[initOssDev],
													   0,	
													   100,	
													   minDb,
													   maxDb,
													   kIOAudioControlChannelIDDefaultLeft,
													   kIOAudioControlChannelNameLeft,
													   idupper | 0U,
													   usage);
    if (!control) {
        errorMsg("error: createVolumeControl failed\n");
        goto Done;
    }
    
    control->setValueChangeHandler((IOAudioControl::IntValueChangeHandler)volumeChangeHandler, this);
    this->addDefaultAudioControl(control);
    control->release();
    
	/* Right channel */
	control = IOAudioLevelControl::createVolumeControl(mDevice->mMixerDefaults[initOssDev],
													   0,	
													   100,	
													   minDb,
													   maxDb,
													   kIOAudioControlChannelIDDefaultRight,
													   kIOAudioControlChannelNameRight,
													   idupper | 1U,
													   usage);
    if (!control) {
        errorMsg("error: createVolumeControl failed\n");
        goto Done;
    }
    
    control->setValueChangeHandler((IOAudioControl::IntValueChangeHandler)volumeChangeHandler, this);
    this->addDefaultAudioControl(control);
    control->release();
    
	// Create mute control
createMuteControl:
    control = IOAudioToggleControl::createMuteControl(false,	// initial state - unmuted
													  kIOAudioControlChannelIDAll,	// Affects all channels
													  kIOAudioControlChannelNameAll,
													  idupper | 2U,
													  usage);
    if (!control) {
		errorMsg("error: createMuteControl failed\n");
        goto Done;
    }
	
    control->setValueChangeHandler((IOAudioControl::IntValueChangeHandler)muteChangeHandler, this);
    this->addDefaultAudioControl(control);
    control->release();

createSelectorControl:
	if(usage == kIOAudioControlUsageOutput) {
		mSelControl = IOAudioSelectorControl::createOutputSelector(mPortType,
																   kIOAudioControlChannelIDAll,
																   kIOAudioControlChannelNameAll,
																   idupper | 5U);
	}else{
		mSelControl = IOAudioSelectorControl::createInputSelector(mPortType,
																  kIOAudioControlChannelIDAll,
																  kIOAudioControlChannelNameAll,
																  idupper | 5U);
	}
	if(mSelControl != 0) {
		mSelControl->addAvailableSelection(mPortType, mPortName);
		mSelControl->setValueChangeHandler(SelectorChanged, this);
		this->addDefaultAudioControl(mSelControl);
	}
	
	result = true;

Done:
	return result;
}

__attribute__((visibility("hidden")))
void VoodooHDAEngine::setPinName(UInt32 pinConfig, const char* name)
{
	UInt32 previousPortType;
	if (!name)
		return;
	previousPortType = mPortType;
	mPortName = name;
	mPortType = pinConfigToSelection(pinConfig);
	beginConfigurationChange();
	setDescription(name);
	if(mSelControl == 0) {
		completeConfigurationChange();
		return;
	}
	
	mSelControl->removeAvailableSelection(previousPortType);
	mSelControl->addAvailableSelection(mPortType, name);
	mSelControl->setValue(mPortType);
	completeConfigurationChange();
}

__attribute__((visibility("hidden")))
IOReturn VoodooHDAEngine::volumeChangeHandler(IOService *target, IOAudioControl *volumeControl, SInt32 oldValue, SInt32 newValue)
{
	IOReturn result = kIOReturnBadArgument;
	VoodooHDAEngine *audioEngine = OSDynamicCast(VoodooHDAEngine, target);

	if (audioEngine) {
		result = audioEngine->volumeChanged(volumeControl, oldValue, newValue);
	}

	return result;
}

__attribute__((visibility("hidden")))
IOReturn VoodooHDAEngine::volumeChanged(IOAudioControl *volumeControl, SInt32 oldValue, SInt32 newValue)
{
	if(mVerbose >2)
		errorMsg("VoodooHDAEngine[%p]::volumeChanged(%p, %ld, %ld)\n", this, volumeControl, (long int)oldValue, (long int)newValue);

	if (volumeControl) {

		int ossDev = ( getEngineDirection() == kIOAudioStreamDirectionOutput) ? SOUND_MIXER_VOLUME:
		SOUND_MIXER_MIC;

		PcmDevice *pcmDevice = mChannel->pcmDevice;
		
		switch (ossDev) {
			case SOUND_MIXER_VOLUME:
				/* Left channel */
				if(volumeControl->getChannelID() == 1) {
					oldOutVolumeLeft = newValue;
					if (mEnableVolumeChangeFix) {
						mDevice->audioCtlOssMixerSet(pcmDevice, SOUND_MIXER_PCM, newValue, newValue);
					} else {
						mDevice->audioCtlOssMixerSet(pcmDevice, SOUND_MIXER_VOLUME, newValue, pcmDevice->right[0]);
					}
          
				}
				/* Right channel */
				else if(volumeControl->getChannelID() == 2) {
					oldOutVolumeRight = newValue;
					if (mEnableVolumeChangeFix) {
						mDevice->audioCtlOssMixerSet(pcmDevice, SOUND_MIXER_PCM, newValue, newValue);
					} else {
						mDevice->audioCtlOssMixerSet(pcmDevice, SOUND_MIXER_VOLUME, pcmDevice->left[0], newValue);
					}
				}
				
				break;
			case SOUND_MIXER_MIC:
				oldInputGain = newValue;
				mDevice->audioCtlOssMixerSet(pcmDevice, ossDev, newValue, newValue);
				break;
			default:
				break;
		}
		// cue8chalk: this seems to be needed when pin configs aren't set properly
		if (mEnableVolumeChangeFix) {
			for (int n = 0; n < SOUND_MIXER_NRDEVICES; n++){
				mDevice->audioCtlOssMixerSet(pcmDevice, n, newValue, newValue);
			}
		}
    
	}

	return kIOReturnSuccess;
}

__attribute__((visibility("hidden")))
IOReturn VoodooHDAEngine::muteChangeHandler(IOService *target, IOAudioControl *muteControl, SInt32 oldValue, SInt32 newValue)
{
	IOReturn result = kIOReturnBadArgument;
	VoodooHDAEngine *audioEngine = OSDynamicCast(VoodooHDAEngine, target);

	if (audioEngine) {
		result = audioEngine->muteChanged(muteControl, oldValue, newValue);
	}

	return result;
}

__attribute__((visibility("hidden")))
IOReturn VoodooHDAEngine::muteChanged(IOAudioControl *muteControl, SInt32 oldValue, SInt32 newValue)
{
	if(mVerbose >2)
		errorMsg("VoodooHDAEngine[%p]::outputMuteChanged(%p, %ld, %ld)\n", this, muteControl, (long int)oldValue, (long int)newValue);
    
	int ossDev = ( getEngineDirection() == kIOAudioStreamDirectionOutput) ? SOUND_MIXER_VOLUME:
																			SOUND_MIXER_MIC;
    
	PcmDevice *pcmDevice = mChannel->pcmDevice;
    
	if (newValue) {
        // VertexBZ: Mute fix
        if(mEnableMuteFix){
          mDevice->audioCtlOssMixerSet(pcmDevice, SOUND_MIXER_PCM, 0, 0);
        } else {
          mDevice->audioCtlOssMixerSet(pcmDevice, ossDev, 0, 0);
        }
	} else {
		if (mChannel->funcGroup->audio.assocs[mChannel->assocNum].digital) {
			oldOutVolumeLeft = oldOutVolumeRight = oldInputGain = 100;
		}

        // VertexBZ: Mute fix
        if(mEnableMuteFix){
            mDevice->audioCtlOssMixerSet(pcmDevice, SOUND_MIXER_PCM,
                                         (ossDev == SOUND_MIXER_VOLUME) ? oldOutVolumeLeft : oldInputGain,
                                         (ossDev == SOUND_MIXER_VOLUME) ? oldOutVolumeRight: oldInputGain);
        } else {
            
          mDevice->audioCtlOssMixerSet(pcmDevice, ossDev,
									   (ossDev == SOUND_MIXER_VOLUME) ? oldOutVolumeLeft : oldInputGain,
									   (ossDev == SOUND_MIXER_VOLUME) ? oldOutVolumeRight: oldInputGain);
		}
	}
    
    return kIOReturnSuccess;
}
	
OSString *VoodooHDAEngine::getLocalUniqueID()
{
	if (!mDevice || !mDevice->mPciNub)
			return super::getLocalUniqueID();
	
	OSString *ioName = OSDynamicCast(OSString, mDevice->mPciNub->getProperty("IOName"));
	if (!ioName)
			return super::getLocalUniqueID();
	
	char str[64] = "";
	snprintf(str, sizeof str, "%s:%lx", ioName->getCStringNoCopy(), (long unsigned int)index);
	return OSString::withCString(str);
}
