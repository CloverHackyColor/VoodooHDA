#include "License.h"

#ifndef _VOODOO_GFX_HDA_H
#define _VOODOO_GFX_HDA_H

#include <IOKit/IOLib.h>

#include "Private.h"

class VoodooHDADevice;
class VoodooHDAEngine;

class VoodooGFXHDAController;

class VoodooGFXHDAStream
{
public:
	VoodooGFXHDAStream();

	bool init(VoodooGFXHDAController *controller, VoodooHDAEngine *engine, Channel *channel);
	bool isActive() const;
	void activate();
	void deactivate();
	void resetPositionState();
	void resetClipPosition(UInt32 clipSampleFrame);
	void noteClippedPosition(UInt32 nextSampleFrame);
	void serviceInterrupt(UInt32 status, AbsoluteTime *timeStamp);
	UInt32 getCurrentSampleFrame();
	UInt32 getClippedPosition() const;

private:
	VoodooGFXHDAController *mController;
	VoodooHDAEngine *mEngine;
	Channel *mChannel;
	bool mActive;
	UInt32 mClippedPosition;
};

class VoodooGFXHDAController
{
public:
	VoodooGFXHDAController();

	bool init(VoodooHDADevice *device);
	bool ownsChannel(Channel *channel) const;
	void registerStream(Channel *channel, VoodooGFXHDAStream *stream);
	void unregisterStream(Channel *channel, VoodooGFXHDAStream *stream);
	VoodooGFXHDAStream *lookupStream(Channel *channel);
	bool initializeStreamDMA(Channel *channel);
	void prepareStreamDMA(Channel *channel);
	void startStream(Channel *channel);
	void stopStream(Channel *channel);
	void handleStreamInterrupt(Channel *channel, UInt32 status, AbsoluteTime *timeStamp);
	void updateTiming(Channel *channel, bool active, bool primeNow);
	UInt32 getLinkPosition(Channel *channel, bool *valid = NULL);
	void setupStream(Channel *channel, nid_t dac, AudioAssoc *assoc, int totalchn, int totalext);
	VoodooHDADevice *getDevice() const;

private:
	struct StreamSlot {
		Channel *channel;
		VoodooGFXHDAStream *stream;
	};

	VoodooHDADevice *mDevice;
	StreamSlot mStreams[16];
	int mNumStreams;

	int allocateBdlMemory(Channel *channel);
	void setupBdl(Channel *channel);
	void stopStreamRegisters(Channel *channel);
	void startStreamRegisters(Channel *channel);
	void resetStreamRegisters(Channel *channel);
	void setStreamId(Channel *channel);
};

#endif
