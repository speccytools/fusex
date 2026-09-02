/* PreferencesController.h: Routines for dealing with the Preferences Panel
   Copyright (c) 2005 Fredrick Meunier

   $Id$

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

   Author contact information:

   E-mail: pak21-fuse@srcf.ucam.org

*/

#import <AppKit/AppKit.h>

@class JoystickConfigurationController;

@interface PreferencesController : NSWindowController
{
  IBOutlet NSTextField *rom0Filename;
  IBOutlet NSTextField *rom1Filename;
  IBOutlet NSTextField *rom2Filename;
  IBOutlet NSTextField *rom3Filename;
  IBOutlet NSArrayController *machineRomsController;
  IBOutlet NSMatrix *massStorageType;
  IBOutlet NSMatrix *externalSoundType;
  IBOutlet NSMatrix *multifaceType;
  IBOutlet NSTabView *tabView;
  IBOutlet NSToolbar *toolbar;
  IBOutlet NSView *generalPrefsView;
  IBOutlet NSView *soundPrefsView;
  IBOutlet NSView *peripheralsPrefsView;
  IBOutlet NSView *rzxPrefsView;
  IBOutlet NSView *joysticksPrefsView;
  IBOutlet NSView *romPrefsView;
  IBOutlet NSView *machinePrefsView;
  IBOutlet NSView *filterPrefsView;
  IBOutlet NSView *debuggerPrefsView;
  IBOutlet NSButton *bilinearCheckbox;

  JoystickConfigurationController *joystickConfigurationController;

  NSMutableArray *machineRoms;
  NSArray *speakerTypes;
  NSArray *soundStereoAY;
  NSArray *diskTryMerge;
  NSArray *movieCompression;
  NSArray *phantomTypistMode;
  NSArray *generalSoundRam;
}
+ (void)initialize;

- (void)windowDidLoad;
- (void)windowDidMove:(NSNotification *)aNotification;
- (void)showWindow:(id)sender;
- (void)handleWillClose:(NSNotification *)note;
- (IBAction)chooseFile:(id)sender;
- (IBAction)setup:(id)sender;
- (NSArray *)machines;
- (NSArray *)joysticks;
- (NSArray *)sdlJoysticks;
- (IBAction)chooseROMFile:(id)sender;
- (IBAction)resetROMFile:(id)sender;
- (IBAction)resetUserDefaults:(id)sender;
- (IBAction)massStorageTypeClicked:(id)sender;
- (IBAction)externalSoundTypeClicked:(id)sender;
- (IBAction)chooseGeneralSoundROM:(id)sender;
- (IBAction)multifaceTypeClicked:(id)sender;
- (IBAction)selectPrefPanel:(id)item;
- (NSArray *)toolbarSelectableItemIdentifiers:(NSToolbar *)bar;
- (void)setMassStorageType;
- (void)setExternalSoundType;
- (void)fixPhantomTypistMode;

- (unsigned int)countOfMachineRoms;
- (id)objectInMachineRomsAtIndex:(unsigned int)index;
- (void)insertObject:(id)anObject inMachineRomsAtIndex:(unsigned int)index;
- (void)removeObjectFromMachineRomsAtIndex:(unsigned int)index;
- (void)replaceObjectInMachineRomsAtIndex:(unsigned int)index withObject:(id)anObject;

@end
