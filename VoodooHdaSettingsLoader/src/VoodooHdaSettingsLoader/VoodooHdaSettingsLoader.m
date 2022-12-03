//
//  VoodooHdaSettingsLoader.m
//  VoodooHdaSettingsLoader
//
//  Created by Ben on 10/11/11.
//  Copyright (c) 2011-2013 VoodooHDA. All rights reserved.
//
//  Modded by Zenith432 2013 - Support systems with multiple HDA controllers

#import "VoodooHdaSettingsLoader.h"

static
NSString* getFirstMatchingService(void)
{
	io_service_t service;
	mach_port_t masterPort;
	io_iterator_t iter = 0;
	kern_return_t ret;
	io_string_t path;
	NSString* servicePath = nil;

	ret = IOMasterPort(MACH_PORT_NULL, &masterPort);
	if (ret != KERN_SUCCESS) {
		NSLog(@"%s: Can't get masterport\n", __FUNCTION__);
		goto failure;
	}

	ret = IOServiceGetMatchingServices(masterPort, IOServiceMatching(kVoodooHDAClassName), &iter);
	if (ret != KERN_SUCCESS) {
	handle_not_running:
		NSLog(@"VoodooHDA is not running\n");
		goto failure;
	}

	service = IOIteratorNext(iter);
	if (!service)
		goto handle_not_running;

	ret = IORegistryEntryGetPath(service, kIOServicePlane, path);
	IOObjectRelease(service);
	if (ret != KERN_SUCCESS) {
		NSLog(@"%s: Can't get registry-entry path\n", __FUNCTION__);
		goto failure;
	}

	servicePath = [NSString stringWithUTF8String:path];

failure:
	if (iter)
		IOObjectRelease(iter);
	return servicePath;
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

static
bool sendAction(io_connect_t connect, UInt8 ch, UInt8 dev, UInt8 val)
{
	//value of slider to driver
	kern_return_t ret;
	actionInfo in, out;
	size_t outsize;

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
		NSLog(@"%s: Can't connect to StructMethod to send commands\n", __FUNCTION__);
		return false;
	}
	return true;
}

static
bool sendMath(io_connect_t connect, UInt8 ch, bool vectorize, bool useStereo, UInt8 noiseLevel, UInt8 stereoBase)
{
	kern_return_t ret;
	actionInfo in, out;
	size_t outsize;

	//to driver

	in.value = 0;
	in.info.action = (UInt8)kVoodooHDAActionSetMath;
	in.info.channel = ch;
	in.info.device = (vectorize?1:0) | (useStereo?2:0);
	in.info.val = (noiseLevel & 0x0f) | ((stereoBase & 0x0f) << 4);

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
		NSLog(@"%s: Can't connect to StructMethod to send commands\n", __FUNCTION__);
		return false;
	}
	return true;
}

//get channel info from driver
static
ChannelInfo *getChannelInfoFromDriver(io_connect_t connect)
{
	kern_return_t ret;
	ChannelInfo *info = 0;
#if __LP64__
	mach_vm_address_t address;
	mach_vm_size_t size;
#else
	vm_address_t address;
	vm_size_t size;
#endif

	ret = IOConnectMapMemory(connect, kVoodooHDAChannelNames, mach_task_self(), &address, &size,
							 kIOMapAnywhere | kIOMapDefaultCache | kIOMapReadOnly);
	if (ret != kIOReturnSuccess) {
		NSLog(@"%s: Can't map Memory\n", __FUNCTION__);
		goto failure;
	}

	info = malloc((size_t) size);
	if (!info) {
		NSLog(@"%s: Can't allocate memory\n", __FUNCTION__);
		goto failure;
	}
	memcpy(info, (void const*) address, (size_t) size);

failure:

	return info;
}

static
bool loadOneDevice(NSString* servicePath, ChannelInfo const* chInfo)
{
	bool res = false;
	ChannelInfo* driverInfo = 0;
	int c, d;
	kern_return_t ret;
	io_connect_t connect = 0;

	ret = connectToService(servicePath, &connect);
	if (ret != KERN_SUCCESS) {
		NSLog(@"%s: Can't open IO Service\n", __FUNCTION__);
		goto failure;
	}

	NSLog(@"%u channels to read.\n", (unsigned) chInfo->numChannels);

	NSLog(@"Reading driver current info\n");
	driverInfo = getChannelInfoFromDriver(connect);
	if (!driverInfo)
		goto failure;

	NSLog(@"Checking that driver matches settings file:\n");

	NSLog(@"Driver has %u channels.\n", (unsigned) driverInfo->numChannels);
	if (driverInfo->numChannels != chInfo->numChannels) {
		NSLog(@"Channel count not matching.\n");
		goto failure;
	}

	NSLog(@"Checking all channels devices\n");
	for(c = 0; c < (int) chInfo->numChannels; ++c) {
		if (strcmp(driverInfo[c].name, chInfo[c].name) != 0) {
			NSLog(@"Channel names do not match.\n");
			goto failure;
		}

		for (d = 0; d < SOUND_MIXER_NRDEVICES; ++d) {
			if (strcmp(driverInfo[c].mixerValues[d].name, chInfo[c].mixerValues[d].name) != 0) {
				NSLog(@"Device names do not match.\n");
				goto failure;
			}

			if (driverInfo[c].mixerValues[d].enabled != chInfo[c].mixerValues[d].enabled) {
				NSLog(@"Device configurations do not match.\n");
				goto failure;
			}
		}
	}

	NSLog(@"Everything looks good, restoring settings...\n");

	for(c = 0; c < (int) chInfo->numChannels; ++c) {
		for ( d =0; d < SOUND_MIXER_NRDEVICES; ++d) {
			if (chInfo[c].mixerValues[d].enabled) {
				NSLog(@"Setting back %s, %s to %d",
					  chInfo[c].name,
					  chInfo[c].mixerValues[d].name,
					  chInfo[c].mixerValues[d].value);

				sendAction(connect, c, chInfo[c].mixerValues[d].mixId, chInfo[c].mixerValues[d].value);
			}
		}

		sendMath(connect, c, chInfo[c].vectorize,
				 chInfo[c].useStereo,
				 chInfo[c].noiseLevel,
				 chInfo[c].StereoBase);
	}

	res = true;

failure:
	if (connect)
		IOServiceClose(connect);
	if (driverInfo)
		free(driverInfo);
	return res;
}

@implementation VoodooHdaSettingsLoader

- (bool) tryImportOldSettings
{
	bool res = false;
	NSString *oPath = [@"~/Library/Preferences/VoodooHDA.settings" stringByExpandingTildeInPath];
	FILE* inputFile = 0;
	long size, channelCount, numRead;
	ChannelInfo* channels = 0;
	NSMutableDictionary *settings, *devices;
	NSString* servicePath;
	NSOutputStream *convertedFile;
	NSError* streamingError;

	inputFile = fopen([oPath UTF8String], "rb");
	if (!inputFile)
		goto failure;

	/*
	 * See if HDA service exists before importing
	 */
	servicePath = getFirstMatchingService();
	if (!servicePath)
		goto failure;

	fseek(inputFile, 0, SEEK_END);
	size = ftell(inputFile);
	rewind(inputFile);

	if( ( (size-1) /sizeof(ChannelInfo))*sizeof(ChannelInfo) != (size-1) ) // Remove the \n at the end
	{
		NSLog(@"Settings file is not compatible with this version of VoodooHdaSettingsLoader (channel sizes are different)\n");
		goto failure;
	}

	channelCount = size/sizeof(ChannelInfo);

	channels = (ChannelInfo*) malloc(channelCount * sizeof *channels);
	if (!channels) {
		NSLog(@"%s: Can't allocate memory\n", __FUNCTION__);
		goto failure;
	}

	numRead = (long) fread(channels, sizeof *channels, (size_t) channelCount, inputFile);
	if (numRead < channelCount) {
		NSLog(@"%s: fread came up short\n", __FUNCTION__);
		goto failure;
	}
	fclose(inputFile);
	inputFile = 0;
	settings = [NSMutableDictionary dictionaryWithCapacity:2U];
	devices = [NSMutableDictionary dictionaryWithCapacity:1U];
	[settings setObject:@"1.2" forKey:@"Version"];
	[devices setObject:[NSData dataWithBytes:channels length:(channelCount * sizeof *channels)] forKey:servicePath];
	free(channels);
	channels = 0;
	[settings setObject:devices forKey:@"Devices"];
	convertedFile = (NSOutputStream*) [NSOutputStream outputStreamToFileAtPath:[oPath stringByAppendingString:@".plist"]
																		append:NO];
	[convertedFile open];
	[NSPropertyListSerialization writePropertyList:settings
										  toStream:convertedFile
											format:NSPropertyListBinaryFormat_v1_0
										   options:0U
											 error:&streamingError];
	[convertedFile close];
//	[[NSFileManager defaultManager] removeItemAtPath:oPath error:&streamingError];

	res = true;

failure:
	if (channels)
		free(channels);
	if (inputFile)
		fclose(inputFile);
	return res;
}

- (bool) loadSettings
{
	bool res = false, triedImport = false;
	NSString *nPath = [@"~/Library/Preferences/VoodooHDA.settings.plist" stringByExpandingTildeInPath];
	NSInputStream* inputFile;
	id parseResult;
	NSError* parseError;
	NSDictionary *settings, *devices;
	NSData* d;
	size_t d_length;
	ChannelInfo const *chInfo;

	while (true) {
		inputFile = (NSInputStream*) [NSInputStream inputStreamWithFileAtPath:nPath];
		if (!inputFile) {
		handle_file_error:
			if (!triedImport) {
				triedImport = true;
				if ([self tryImportOldSettings])
					continue;
			}
			NSLog(@"Couldn't load settings file (file opening problem: not here, or read protected)\n");
			goto failure;
		}
		[inputFile open];
		if (inputFile.streamStatus != NSStreamStatusOpen) {
			// [inputFile close];
			inputFile = nil;
			goto handle_file_error;
		}
		break;
	}

	parseResult = [NSPropertyListSerialization propertyListWithStream:inputFile
															  options:NSPropertyListImmutable
															   format:nil
																error:&parseError];
	[inputFile close];
	if (!parseResult || ![parseResult isKindOfClass:[NSDictionary class]]) {
	handle_parse_error:
		NSLog(@"Couldn't parse settings file\n");
		goto failure;
	}
	settings = (NSDictionary*) parseResult;
	/*
	 * Ignore settings version
	 */
	parseResult = [settings objectForKey:@"Devices"];
	if (!parseResult || ![parseResult isKindOfClass:[NSDictionary class]])
		goto handle_parse_error;
	devices = (NSDictionary*) parseResult;
	for (NSString* device in devices) {
		parseResult = [devices objectForKey:device];
		if (!parseResult || ![parseResult isKindOfClass:[NSData class]]) {
			NSLog(@"Warning - can't parse device %@\n", device);
			continue;		// skip device
		}
		d = (NSData*) parseResult;
		d_length = (size_t) [d length];
		if (d_length <= 0U ||
			(d_length % sizeof *chInfo) != 0U) {
		handle_length_error:
			NSLog(@"Warning - device %@ data invalid length %lu\n", device, d_length);
			continue;		// skip device
		}
		chInfo = (ChannelInfo const*) [d bytes];
		if (d_length < chInfo->numChannels * sizeof *chInfo)
			goto handle_length_error;
		loadOneDevice(device, chInfo);
	}

	res = true;

	NSLog(@"Settings restored succesfully!\n");

failure:

	return res;
}

- (void) load
{
	[self loadSettings];
}

@end
