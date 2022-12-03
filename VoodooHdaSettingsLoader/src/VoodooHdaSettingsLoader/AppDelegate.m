//
//  AppDelegate.m
//  VoodooHdaSettingsLoader
//
//  Created by Ben on 10/11/11.
//  Copyright (c) 2011-2013 VoodooHDA. All rights reserved.
//

#import "AppDelegate.h"

#import "VoodooHdaSettingsLoader.h"

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)aNotification
{
	VoodooHdaSettingsLoader* loader = [[VoodooHdaSettingsLoader alloc] init];
	[loader load];
    exit(0);
}

@end
