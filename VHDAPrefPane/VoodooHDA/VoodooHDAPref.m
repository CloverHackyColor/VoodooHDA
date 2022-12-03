//
//  VoodooHDAPref.m
//  VoodooHDA
//
//  Created by fassl on 15.04.09.
//  Copyright (c) 2009-2013 Voodoo Team. All rights reserved.
//
// Modded by Slice 2010
// Modded by Zenith432 2013 - Support systems with multiple HDA controllers

#import "VoodooHDAPref.h"

@implementation VoodooHDAPref

static
NSArray* getServices()
{
	io_service_t service;
	mach_port_t masterPort;
	io_iterator_t iter = 0;
	kern_return_t ret;
	io_string_t path;
	NSMutableArray* services;

	services = [NSMutableArray arrayWithCapacity:2U];

	ret = IOMasterPort(MACH_PORT_NULL, &masterPort);
	if (ret != KERN_SUCCESS) {
		NSRunCriticalAlertPanel(
								NSLocalizedString( @"Error", "MsgBox"),
								NSLocalizedString( @"Can't get masterport", "MsgBoxBody" ), nil, nil, nil );
		goto failure;
	}

	ret = IOServiceGetMatchingServices(masterPort, IOServiceMatching(kVoodooHDAClassName), &iter);
	if (ret != KERN_SUCCESS) {
		NSRunCriticalAlertPanel(
								NSLocalizedString( @"Error", "MsgBox"),
								NSLocalizedString( @"VoodooHDA is not running", "MsgBoxBody" ), nil, nil, nil );
		goto failure;
	}

	while ((service = IOIteratorNext(iter)) != 0) {
		ret = IORegistryEntryGetPath(service, kIOServicePlane, path);
		IOObjectRelease(service);
		if (ret != KERN_SUCCESS) {
			NSRunCriticalAlertPanel(
									NSLocalizedString( @"Error", "MsgBox"),
									NSLocalizedString( @"Can't get registry-entry path", "MsgBoxBody" ), nil, nil, nil );
			continue;
		}
		[services addObject:[NSString stringWithUTF8String:path]];
	}

failure:
	if (iter)
		IOObjectRelease(iter);
	return services;
}

static
kern_return_t connectToService(NSString* servicePath, io_connect_t* connect)
{
	mach_port_t masterPort;
	io_registry_entry_t service;
	io_string_t path;
	kern_return_t ret;

	if (!servicePath ||
		![servicePath getCString:path maxLength:sizeof path encoding:NSUTF8StringEncoding])
		return KERN_INVALID_ARGUMENT;
	ret = IOMasterPort(MACH_PORT_NULL, &masterPort);
	if (ret != KERN_SUCCESS)
		return ret;
	service = IORegistryEntryFromPath(masterPort, path);
	if (service == MACH_PORT_NULL)
		return KERN_INVALID_NAME;
	ret = IOServiceOpen((io_service_t) service, mach_task_self(), 0U, connect);
	IOObjectRelease(service);
	return ret;
}

//get channel info from driver
static
ChannelInfo *updateChannelInfo(NSString* servicePath)
{
	kern_return_t ret;
	io_connect_t connect = 0;
	ChannelInfo *info = 0;
#if __LP64__
	mach_vm_address_t address;
	mach_vm_size_t size;
#else
	vm_address_t address;
	vm_size_t size;
#endif

	ret = connectToService(servicePath, &connect);
	if (ret != KERN_SUCCESS) {
		NSRunCriticalAlertPanel(
								NSLocalizedString( @"Error", "MsgBox"),
								NSLocalizedString( @"Can't open IO Service", "MsgBoxBody" ), nil, nil, nil );
		goto failure;
	}

	ret = IOConnectMapMemory(connect, kVoodooHDAChannelNames, mach_task_self(), &address, &size,
							 kIOMapAnywhere | kIOMapDefaultCache | kIOMapReadOnly);
	if (ret != kIOReturnSuccess) {
		NSRunCriticalAlertPanel(
								NSLocalizedString( @"Error", "MsgBox"),
								NSLocalizedString( @"Can't map Memory", "MsgBoxBody" ), nil, nil, nil );
		goto failure;
	}

	info = malloc((size_t) size);
	if (!info) {
		NSRunCriticalAlertPanel(
								NSLocalizedString( @"Error", "MsgBox"),
								NSLocalizedString( @"Can't allocate memory", "MsgBoxBody" ), nil, nil, nil );
		goto failure;
	}
	memcpy(info, (void const*) address, (size_t) size);

failure:
	if (connect)
		IOServiceClose(connect);

	return info;
}

- (bool) updateSliders
{
	ChannelInfo* info;

	if (!services || currentService < 0)
		goto failure;

	if (chInfo) {
		free(chInfo);
		chInfo = 0;
	}
	if (!(chInfo = updateChannelInfo((NSString*) [services objectAtIndex:currentService])))  //from driver
		goto failure;

	info = chInfo;

	if(!(info[currentChannel].mixerValues[0].enabled))
		[sliderBass setEnabled:FALSE];
	else
		[sliderBass setEnabled:TRUE];
	[sliderBass setIntValue:info[currentChannel].mixerValues[0].value];

	if(!(info[currentChannel].mixerValues[1].enabled))
		[sliderTreble setEnabled:FALSE];
	else
		[sliderTreble setEnabled:TRUE];
	[sliderTreble setIntValue:info[currentChannel].mixerValues[1].value];

	if(!(info[currentChannel].mixerValues[2].enabled))
		[sliderSynth setEnabled:FALSE];
	else
		[sliderSynth setEnabled:TRUE];
	[sliderSynth setIntValue:info[currentChannel].mixerValues[2].value];

	if(!(info[currentChannel].mixerValues[3].enabled))
		[sliderPCM setEnabled:FALSE];
	else
		[sliderPCM setEnabled:TRUE];
	[sliderPCM setIntValue:info[currentChannel].mixerValues[3].value];

	if(!(info[currentChannel].mixerValues[4].enabled))
		[sliderSpeaker setEnabled:FALSE];
	else
		[sliderSpeaker setEnabled:TRUE];
	[sliderSpeaker setIntValue:info[currentChannel].mixerValues[4].value];

	if(!(info[currentChannel].mixerValues[5].enabled))
		[sliderLine setEnabled:FALSE];
	else
		[sliderLine setEnabled:TRUE];
	[sliderLine setIntValue:info[currentChannel].mixerValues[5].value];

	if(!(info[currentChannel].mixerValues[6].enabled))
		[sliderMic setEnabled:FALSE];
	else
		[sliderMic setEnabled:TRUE];
	[sliderMic setIntValue:info[currentChannel].mixerValues[6].value];

	if(!(info[currentChannel].mixerValues[7].enabled))
		[sliderCD setEnabled:FALSE];
	else
		[sliderCD setEnabled:TRUE];
	[sliderCD setIntValue:info[currentChannel].mixerValues[7].value];

	if(!(info[currentChannel].mixerValues[8].enabled))
		[sliderIMix setEnabled:FALSE];
	else
		[sliderIMix setEnabled:TRUE];
	[sliderIMix setIntValue:info[currentChannel].mixerValues[8].value];

	if(!(info[currentChannel].mixerValues[9].enabled))
		[sliderAltPCM setEnabled:FALSE];
	else
		[sliderAltPCM setEnabled:TRUE];
	[sliderAltPCM setIntValue:info[currentChannel].mixerValues[9].value];

	if(!(info[currentChannel].mixerValues[10].enabled))
		[sliderRecLev setEnabled:FALSE];
	else
		[sliderRecLev setEnabled:TRUE];
	[sliderRecLev setIntValue:info[currentChannel].mixerValues[10].value];

	if(!(info[currentChannel].mixerValues[11].enabled))
		[sliderIGain setEnabled:FALSE];
	else
		[sliderIGain setEnabled:TRUE];
	[sliderIGain setIntValue:info[currentChannel].mixerValues[11].value];

	if(!(info[currentChannel].mixerValues[12].enabled))
		[sliderOGain setEnabled:FALSE];
	else
		[sliderOGain setEnabled:TRUE];
	[sliderOGain setIntValue:info[currentChannel].mixerValues[12].value];

	if(!(info[currentChannel].mixerValues[13].enabled))
		[sliderLine1 setEnabled:FALSE];
	else
		[sliderLine1 setEnabled:TRUE];
	[sliderLine1 setIntValue:info[currentChannel].mixerValues[13].value];

	if(!(info[currentChannel].mixerValues[14].enabled))
		[sliderLine2 setEnabled:FALSE];
	else
		[sliderLine2 setEnabled:TRUE];
	[sliderLine2 setIntValue:info[currentChannel].mixerValues[14].value];

	if(!(info[currentChannel].mixerValues[15].enabled))
		[sliderLine3 setEnabled:FALSE];
	else
		[sliderLine3 setEnabled:TRUE];
	[sliderLine3 setIntValue:info[currentChannel].mixerValues[15].value];

	if(!(info[currentChannel].mixerValues[16].enabled))
		[sliderDigital1 setEnabled:FALSE];
	else
		[sliderDigital1 setEnabled:TRUE];
	[sliderDigital1 setIntValue:info[currentChannel].mixerValues[16].value];

	if(!(info[currentChannel].mixerValues[17].enabled))
		[sliderDigital2	setEnabled:FALSE];
	else
		[sliderDigital2 setEnabled:TRUE];
	[sliderDigital2 setIntValue:info[currentChannel].mixerValues[17].value];

	if(!(info[currentChannel].mixerValues[18].enabled))
		[sliderDigital3 setEnabled:FALSE];
	else
		[sliderDigital3 setEnabled:TRUE];
	[sliderDigital3 setIntValue:info[currentChannel].mixerValues[18].value];

	if(!(info[currentChannel].mixerValues[19].enabled))
		[sliderPhoneIn setEnabled:FALSE];
	else
		[sliderPhoneIn setEnabled:TRUE];
	[sliderPhoneIn setIntValue:info[currentChannel].mixerValues[19].value];

	if(!(info[currentChannel].mixerValues[20].enabled))
		[sliderPhoneOut setEnabled:FALSE];
	else
		[sliderPhoneOut setEnabled:TRUE];
	[sliderPhoneOut setIntValue:info[currentChannel].mixerValues[20].value];

	if(!(info[currentChannel].mixerValues[21].enabled))
		[sliderVideo setEnabled:FALSE];
	else
		[sliderVideo setEnabled:TRUE];
	[sliderVideo setIntValue:info[currentChannel].mixerValues[21].value];

	if(!(info[currentChannel].mixerValues[22].enabled))
		[sliderRadio setEnabled:FALSE];
	else
		[sliderRadio setEnabled:TRUE];
	[sliderRadio setIntValue:info[currentChannel].mixerValues[22].value];

	if(!(info[currentChannel].mixerValues[23].enabled))
		[sliderMonitor setEnabled:FALSE];
	else
		[sliderMonitor setEnabled:TRUE];
	[sliderMonitor setIntValue:info[currentChannel].mixerValues[23].value];

/*	if(!(info[currentChannel].mixerValues[24].enabled))
		[sliderVolume setEnabled:FALSE];
	else */
		[sliderVolume setEnabled:TRUE];
	[sliderVolume setIntValue:info[currentChannel].mixerValues[24].value];

    [sliderNoise setIntValue:info[currentChannel].noiseLevel];
	[sliderStereo setIntValue:info[currentChannel].StereoBase];

	[soundVector setState:info[currentChannel].vectorize?NSOnState:NSOffState];
	[stereoEnhance setState:info[currentChannel].useStereo?NSOnState:NSOffState];

	return true;
failure:
	return false;
}

- (bool) updateMath
{
	kern_return_t ret;
	io_connect_t connect = 0;
	actionInfo in, out;
	UInt8 ch;
	size_t outsize;

	if (!chInfo || !services || currentService < 0)
		goto failure;

	ret = connectToService((NSString*) [services objectAtIndex:currentService], &connect);
	if (ret != KERN_SUCCESS) {
		NSRunCriticalAlertPanel(
								NSLocalizedString( @"Error", "MsgBox"),
								NSLocalizedString( @"Can't open IO Service", "MsgBoxBody" ), nil, nil, nil );
		goto failure;
	}
	//to driver
	ch = currentChannel;
	in.value = 0;
	in.info.action = (UInt8)kVoodooHDAActionSetMath;
	in.info.channel = currentChannel;
	in.info.device = (chInfo[ch].vectorize?1:0) | (chInfo[ch].useStereo?2:0);
	in.info.val = (chInfo[ch].noiseLevel & 0x0f) | ((chInfo[ch].StereoBase & 0x0f) << 4);
//	[versionText setStringValue:[NSString stringWithFormat:@"Device=%d Val=0x%04x Volume=%d",
//								 in.info.device, in.info.val, chInfo[ch].mixerValues[24].value]];
	outsize = sizeof out;
#if MAC_OS_X_VERSION_MIN_REQUIRED <= MAC_OS_X_VERSION_10_4
    ret = IOConnectMethodStructureIStructureO( connect,
											   kVoodooHDAActionMethod,
											   sizeof in,	/* structureInputSize */
											   &outsize,	/* structureOutputSize */
											   &in,			/* inputStructure */
											   &out);		/* ouputStructure */
#else

	ret = IOConnectCallStructMethod(connect,
									kVoodooHDAActionMethod,
									&in,
									sizeof in,
									&out,
									&outsize
									);
#endif
	if (ret != KERN_SUCCESS) {
		NSRunCriticalAlertPanel(
								NSLocalizedString( @"Error", "MsgBox"),
								NSLocalizedString( @"Can't connect to StructMethod to send commands", "MsgBoxBody" ), nil, nil, nil );
		goto failure;
	}

	if(connect)
		IOServiceClose(connect);

	return true;

failure:
	if(connect)
		IOServiceClose(connect);

	return false;
}

static
NSString* trimIORegistryPathForDisplay(NSString* path)
{
	NSRange subName;
	NSUInteger i, l = [path length];
	subName.location = 0U;
	for (i = 0U; i < l - 1U; ++i)
		if ([path characterAtIndex:i] == L':') {
			subName.location = i + 1U;
			break;
		}
	subName.length = l - subName.location;
	for (i = l - 1U; i > subName.location; --i)
		if ([path characterAtIndex:i] == L'/') {
			subName.length = i - subName.location;
			break;
		}
	return [path substringWithRange:subName];
}

- (bool) populateHDASelector
{
	if (!services || currentService < 0)
		return false;
	[HDAselector removeAllItems];
	for (NSString* s in services)
		[HDAselector addItemWithTitle:trimIORegistryPathForDisplay(s)];
	[HDAselector selectItemAtIndex:currentService];
	return true;
}

- (bool) populateSelector
{
	ChannelInfo* info;
	int i, N;

	info = chInfo;
	if (!info)
		return false;

	[selector removeAllItems];

	i=0;
	N = info[0].numChannels;
	if (N<=0 || N>24) {
		N=3;
		NSRunCriticalAlertPanel(
								NSLocalizedString( @"Error", "MsgBox"),
								NSLocalizedString( @"Wrong Channels Number 0..24", "MsgBoxBody" ), nil, nil, nil );

	}

	for (; i < N; i++){
		//	if (sizeof(chInfo[i].name)) {
		[selector addItemWithTitle:[NSString stringWithFormat:@"%d: %s", i+1, info[i].name]];
		//	} else {
		//		[selector addItemWithTitle:[NSString stringWithFormat:@"%d: Empty", i+1]];
		//	}
	}
	[selector selectItemAtIndex:currentChannel];
	return true;
}

- (void) mainViewDidLoad
{
	[super mainViewDidLoad];
	services = getServices();
	if (services.count > 0U)
		currentService = 0;
	else
		currentService = -1;

	if (![self populateHDASelector])
		goto failure;

	[versionText setStringValue:@"Loaded"];

	return;

failure:
	[versionText setStringValue:@"ERROR"];
}

static
bool sendAction(NSString* servicePath, UInt8 ch, UInt8 dev, UInt8 val)
{  //value of slider to driver
	kern_return_t ret;
	io_connect_t connect = 0;
	actionInfo in, out;
	size_t outsize;

	ret = connectToService(servicePath, &connect);
	if (ret != KERN_SUCCESS) {
		NSRunCriticalAlertPanel(
								NSLocalizedString( @"Error", "MsgBox"),
								NSLocalizedString( @"Can't open IO Service", "MsgBoxBody" ), nil, nil, nil );
		goto failure;
	}

	in.value = 0;
	in.info.action = (UInt8)kVoodooHDAActionSetMixer;
	in.info.channel = ch;
	in.info.device = dev;
	in.info.val = val;

	outsize = sizeof out;
#if MAC_OS_X_VERSION_MIN_REQUIRED <= MAC_OS_X_VERSION_10_4
    ret = IOConnectMethodStructureIStructureO( connect, kVoodooHDAActionMethod,
											  sizeof in,	/* structureInputSize */
											  &outsize,		/* structureOutputSize */
											  &in,			/* inputStructure */
											  &out);		/* ouputStructure */
#else

	ret = IOConnectCallStructMethod(connect,
									kVoodooHDAActionMethod,
									&in,
									sizeof in,
									&out,
									&outsize
									);
#endif
	if (ret != KERN_SUCCESS) {
		NSRunCriticalAlertPanel(
								NSLocalizedString( @"Error", "MsgBox"),
								NSLocalizedString( @"Can't connect to StructMethod to send commands", "MsgBoxBody" ), nil, nil, nil );
//		goto failure;  //anyway
	}

failure:

	if(connect)
		IOServiceClose(connect);

	return ret == KERN_SUCCESS ? true : false;
}

- (IBAction)sliderMoved:(NSSlider *)sender
{
	UInt8 device = 0U;
	if (!chInfo || !services || currentService < 0)
		return;
	if (sender == sliderNoise){
		chInfo[currentChannel].noiseLevel = [sender intValue];
		[self updateMath];
		return;
	}
	if (sender == sliderStereo) {
		chInfo[currentChannel].StereoBase = [sender intValue] + 7;
		[self updateMath];
		return;
	}

	if(sender == sliderBass)			device=1;
	else if(sender == sliderTreble)		device=2;
	else if(sender == sliderSynth)		device=3;
	else if(sender == sliderPCM)		device=4;
	else if(sender == sliderSpeaker)	device=5;
	else if(sender == sliderLine)		device=6;
	else if(sender == sliderMic)		device=7;
	else if(sender == sliderCD)			device=8;
	else if(sender == sliderIMix)		device=9;
	else if(sender == sliderAltPCM)		device=10;
	else if(sender == sliderRecLev)		device=11;
	else if(sender == sliderIGain)		device=12;
	else if(sender == sliderOGain)		device=13;
	else if(sender == sliderLine1)		device=14;
	else if(sender == sliderLine2)		device=15;
	else if(sender == sliderLine3)		device=16;
	else if(sender == sliderDigital1)	device=17;
	else if(sender == sliderDigital2)	device=18;
	else if(sender == sliderDigital3)	device=19;
	else if(sender == sliderPhoneIn)	device=20;
	else if(sender == sliderPhoneOut)	device=21;
	else if(sender == sliderVideo)		device=22;
	else if(sender == sliderRadio)		device=23;
	else if(sender == sliderMonitor)	device=24;
	else if(sender == sliderVolume)		device=0;

	sendAction((NSString*) [services objectAtIndex:currentService], currentChannel, device, [sender intValue]);
}

- (IBAction)selectorChanged:(NSPopUpButton *)sender
{
	if (sender == selector) {
		currentChannel = (int)[sender indexOfSelectedItem];
		[self updateSliders];
		return;
	}
	if (sender == HDAselector) {
		if (!services)
			return;
		currentService = [sender indexOfSelectedItem];
		currentChannel = 0U;
		[self updateSliders];
		[self populateSelector];
	}
}

- (void) didUnselect
{
	[super didUnselect];
	[self saveSettings];
}

- (bool) saveSettings
{
	bool res = false;
	NSOutputStream *outputFile;
	NSMutableDictionary *outerSettings, *innerSettings;
	NSError *outputError;
	NSInteger bytesWritten;
	NSString *nPath = [@"~/Library/Preferences/VoodooHDA.settings.plist" stringByExpandingTildeInPath];
	if (!services || currentService < 0)
		goto failure;
	outerSettings = [NSMutableDictionary dictionaryWithCapacity:2U];
	innerSettings = [NSMutableDictionary dictionaryWithCapacity:services.count];
	[outerSettings setObject:@"1.2" forKey:@"Version"];
	for (NSString* device in services) {
		if (chInfo) {
			free(chInfo);
			chInfo = 0;
		}
		if (!(chInfo = updateChannelInfo(device)))
			continue;
		[innerSettings setObject:[NSData dataWithBytes:chInfo length:(chInfo->numChannels * sizeof *chInfo)]
						  forKey:device];
	}
	if (chInfo) {
		free(chInfo);
		chInfo = 0;
	}
	if (innerSettings.count < 1U)
		goto failure;
	[outerSettings setObject:innerSettings forKey:@"Devices"];
	outputFile = (NSOutputStream*) [NSOutputStream outputStreamToFileAtPath:nPath append:NO];
	if (!outputFile)
		goto failure;
	[outputFile open];
	if (outputFile.streamStatus != NSStreamStatusOpen) {
		NSRunCriticalAlertPanel(
								NSLocalizedString( @"Couldn't save settings file", "MsgBox"),
								NSLocalizedString( @"Error opening file", "MsgBoxBody" ), nil, nil, nil );
		goto failure;
	}
	bytesWritten = [NSPropertyListSerialization writePropertyList:outerSettings
														  toStream:outputFile
															format:NSPropertyListBinaryFormat_v1_0
														   options:0U
															 error:&outputError];
	[outputFile close];
	if (bytesWritten <= 0)
		goto failure;

	res = true;
failure:

	return res;
}
//Just a sample
/*- (void) changeVersionText
{
	[versionText setStringValue:@"Bla"];
}
*/
/*
- (IBAction)enableAllSLiders:(NSButton *)sender {
	[sliderBass setEnabled:TRUE];
	[sliderTreble setEnabled:TRUE];
	[sliderSynth setEnabled:TRUE];
	[sliderPCM setEnabled:TRUE];
	[sliderSpeaker setEnabled:TRUE];
	[sliderLine setEnabled:TRUE];
	[sliderMic setEnabled:TRUE];
	[sliderCD setEnabled:TRUE];
	[sliderIMix setEnabled:TRUE];
	[sliderAltPCM setEnabled:TRUE];
	[sliderRecLev setEnabled:TRUE];
	[sliderIGain setEnabled:TRUE];
	[sliderOGain setEnabled:TRUE];
	[sliderLine1 setEnabled:TRUE];
	[sliderLine2 setEnabled:TRUE];
	[sliderLine3 setEnabled:TRUE];
	[sliderDigital1 setEnabled:TRUE];
	[sliderDigital2 setEnabled:TRUE];
	[sliderDigital3 setEnabled:TRUE];
	[sliderPhoneIn setEnabled:TRUE];
	[sliderPhoneOut setEnabled:TRUE];
	[sliderVideo setEnabled:TRUE];
	[sliderRadio setEnabled:TRUE];
	[sliderMonitor setEnabled:TRUE];

//	[sliderVolume setEnabled:TRUE];
//	[sliderNoise setEnabled:TRUE];

}
*/
- (IBAction)useStereoEnhance:(NSButton *)sender
{
	bool useStereo;
	if (!chInfo || !services || currentService < 0)
		return;
	useStereo = ([stereoEnhance state]==NSOnState);
	chInfo[currentChannel].useStereo = useStereo;
	[self updateMath];
}

- (IBAction)SSEChanged:(NSButton *)sender
{
	bool vector;
	if (!chInfo || !services || currentService < 0)
		return;
	vector = ([soundVector state]==NSOnState);
	chInfo[currentChannel].vectorize = vector;
	[self updateMath];
}

static
void disableViewRecursive(NSView* view)
{
	NSArray* subViews = [view subviews];

	for (NSView* currentView in subViews) {
		if ([currentView isKindOfClass:[NSControl class]])
			[(NSControl*) currentView setEnabled:FALSE];
		disableViewRecursive(currentView);
	}
}

- (void) willSelect
{
	[super willSelect];
	if (currentService < 0) {
		NSRunCriticalAlertPanel(NSLocalizedString( @"Error", "MsgBox"),
								NSLocalizedString( @"No VoodooHDA Devices Detected", "MsgBoxBody" ), nil, nil, nil );
		disableViewRecursive([self mainView]);
		return;
	}

	currentChannel = 0U;

	if(![self updateSliders])
		goto failure;

	if (![self populateSelector])
		goto failure;

	[versionText setStringValue:@"Loaded"];

	return;

failure:
	[versionText setStringValue:@"ERROR"];
}

- (void) dealloc
{
	if (chInfo) {
		free(chInfo);
		chInfo = 0;
	}
	if (services) {
		services = nil;
	}
}

@end
