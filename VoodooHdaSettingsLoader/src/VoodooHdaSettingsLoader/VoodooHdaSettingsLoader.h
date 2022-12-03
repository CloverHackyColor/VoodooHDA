//
//  VoodooHdaSettingsLoader.h
//  VoodooHdaSettingsLoader
//
//  Created by Ben on 10/11/11.
//  Copyright (c) 2011-2013 VoodooHDA. All rights reserved.
//

#import <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>

#define kVoodooHDAClassName	"VoodooHDADevice"
#define MAX_SLIDER_TAB_NAME_LENGTH 32
#define SOUND_MIXER_NRDEVICES 25

typedef union {
	struct {
		UInt8 action;
		UInt8 channel;
		UInt8 device;
		UInt8 val;
	} info;
	UInt32 value;
} actionInfo;

typedef struct _mixerDeviceInfo {
	UInt8 mixId;
	UInt8 value;
	char name[MAX_SLIDER_TAB_NAME_LENGTH];
	bool enabled;
	UInt8 non[5]; //align to 8 bytes
} mixerDeviceInfo;

typedef struct _ChannelInfo {
	char name[MAX_SLIDER_TAB_NAME_LENGTH];
	mixerDeviceInfo mixerValues[SOUND_MIXER_NRDEVICES];
	UInt8 numChannels;
	bool vectorize;
	bool useStereo;
    UInt8 noiseLevel;
	UInt8 StereoBase;
	UInt8 empty[3];
} ChannelInfo;

enum {
    kVoodooHDAActionMethod = 0,
    kVoodooHDANumMethods
};

enum {
	kVoodooHDAChannelNames = 0x3000
};

enum {
	kVoodooHDAActionSetMixer = 0x40,
	kVoodooHDAActionGetMixers = 0x50,
	kVoodooHDAActionSetMath = 0x60
};

@interface VoodooHdaSettingsLoader : NSObject {
}
- (void) load;
- (bool) loadSettings;
- (bool) tryImportOldSettings;

@end
