//
//  VoodooHDAPref.h
//  VoodooHDA
//
//  Created by fassl on 15.04.09.
//  Copyright (c) 2009-2013 Voodoo. All rights reserved.
//


#import <PreferencePanes/PreferencePanes.h>

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

@interface VoodooHDAPref : NSPreferencePane
{
    IBOutlet NSTextField *versionText;
    IBOutlet NSSlider *sliderAltPCM;
    IBOutlet NSSlider *sliderBass;
    IBOutlet NSSlider *sliderCD;
    IBOutlet NSSlider *sliderDigital1;
    IBOutlet NSSlider *sliderDigital2;
    IBOutlet NSSlider *sliderDigital3;
    IBOutlet NSSlider *sliderIGain;
    IBOutlet NSSlider *sliderIMix;
    IBOutlet NSSlider *sliderLine;
    IBOutlet NSSlider *sliderLine1;
    IBOutlet NSSlider *sliderLine2;
    IBOutlet NSSlider *sliderLine3;
    IBOutlet NSSlider *sliderMic;
    IBOutlet NSSlider *sliderMonitor;
    IBOutlet NSSlider *sliderOGain;
    IBOutlet NSSlider *sliderPCM;
    IBOutlet NSSlider *sliderPhoneIn;
    IBOutlet NSSlider *sliderPhoneOut;
    IBOutlet NSSlider *sliderRadio;
    IBOutlet NSSlider *sliderRecLev;
    IBOutlet NSSlider *sliderSpeaker;
    IBOutlet NSSlider *sliderSynth;
    IBOutlet NSSlider *sliderTreble;
    IBOutlet NSSlider *sliderVideo;

    IBOutlet NSSlider *sliderNoise;
    IBOutlet NSSlider *sliderVolume;
	IBOutlet NSSlider *sliderStereo;

    IBOutlet NSButton *soundVector;
	IBOutlet NSButton *stereoEnhance;

	IBOutlet NSPopUpButton *selector;
	IBOutlet NSPopUpButton *HDAselector;

	NSArray* services;
	NSInteger currentService;
	UInt8 currentChannel;
	ChannelInfo *chInfo;
}
//- (bool) updateChannelInfo;
- (bool) updateMath;
- (bool) updateSliders;
- (void) mainViewDidLoad;
- (void) didUnselect;
- (bool) saveSettings;
- (void) willSelect;
- (void) dealloc;
- (bool) populateHDASelector;
- (bool) populateSelector;
- (IBAction)sliderMoved:(NSSlider *)sender;
- (IBAction)selectorChanged:(NSPopUpButton *)sender;
//- (IBAction)enableAllSLiders:(NSButton *)sender;
- (IBAction)SSEChanged:(NSButton *)sender;
- (IBAction)useStereoEnhance:(NSButton *)sender;

//- (void) changeVersionText;

@end
