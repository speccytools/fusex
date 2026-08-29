/* PreferencesController.m: Routines for dealing with the Preferences Panel
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

#include <libspectrum.h>

#include <string.h>

#import "FuseController.h"
#import "DisplayOpenGLView.h"
#import "AppSandboxFileAccess.h"
#import "models/Emulator.h"
#import "JoystickConfigurationController.h"
#import "PreferencesController.h"
#import "CAMachines.h"
#import "Joysticks.h"
#import "HIDJoysticks.h"

#import "ScalerNameToIdTransformer.h"
#import "ScalerSupportsScanlines.h"
#import "MachineScalerIsEnabled.h"
#import "MachineNameToIdTransformer.h"
#import "VolumeSliderToPrefTransformer.h"

#include "fuse.h"
#include "joystick.h"
#include "options_cocoa.h"
#include "periph.h"
#include "printer.h"
#include "settings.h"
#include "settings_cocoa.h"
#include "sound.h"
#include "machine.h"
#include "ui.h"
#include "ui/scaler/scaler.h"
#include "ui/uidisplay.h"

#define NONE 100

@implementation PreferencesController

/* Set on NSApplicationWillTerminateNotification so handleWillClose:
   can skip the settings-apply / KVO-teardown work when the close is
   part of [NSApp terminate:]'s cascade. Touching NSArrayController
   content or driving graphics hotswaps from inside that cascade
   reaches the notification observer table while NSWindow is still
   walking it, producing the strcmp crash in
   __CFNOTIFICATIONCENTER_IS_CALLING_OUT_TO_AN_OBSERVER__. The work
   skipped here only matters when the emulator continues running
   after the window closes; at quit time the settings have already
   been persisted to NSUserDefaults via the binding chain. */
static BOOL appTerminating = NO;

+ (void)load
{
  [[NSNotificationCenter defaultCenter]
    addObserver:self
       selector:@selector(handleAppWillTerminate:)
           name:NSApplicationWillTerminateNotification
         object:nil];
}

+ (void)handleAppWillTerminate:(NSNotification *)note
{
  appTerminating = YES;
}

+(void) initialize
{
  ScalerNameToIdTransformer *sNToITransformer;
  ScalerSupportsScanlines *scalerSupportsScanlines;
  MachineScalerIsEnabled *machineScalerIsEnabled;
  MachineNameToIdTransformer *mToITransformer;
  VolumeSliderToPrefTransformer *vsToPTransformer;

  sNToITransformer = [[[ScalerNameToIdTransformer alloc] init] autorelease];

  [NSValueTransformer setValueTransformer:sNToITransformer
                                  forName:@"ScalerNameToIdTransformer"];

  scalerSupportsScanlines = [[[ScalerSupportsScanlines alloc] init] autorelease];

  [NSValueTransformer setValueTransformer:scalerSupportsScanlines
                                  forName:@"ScalerSupportsScanlines"];

  machineScalerIsEnabled = [MachineScalerIsEnabled
                                machineScalerIsEnabledWithInt:1];

  [NSValueTransformer setValueTransformer:machineScalerIsEnabled
                                  forName:@"MachineTimexIsEnabled"];

  machineScalerIsEnabled = [MachineScalerIsEnabled
                                machineScalerIsEnabledWithInt:0];

  [NSValueTransformer setValueTransformer:machineScalerIsEnabled
                                  forName:@"MachineTimexIsDisabled"];

  mToITransformer = [[[MachineNameToIdTransformer alloc] init] autorelease];

  [NSValueTransformer setValueTransformer:mToITransformer
                                  forName:@"MachineNameToIdTransformer"];
								  
  vsToPTransformer = [[[VolumeSliderToPrefTransformer alloc] init] autorelease];

  [NSValueTransformer setValueTransformer:vsToPTransformer
                                  forName:@"VolumeSliderToPrefTransformer"];

  /* Force the saved bilinear flag off so the disabled Bilinear checkbox
     shows as unchecked rather than checked-but-greyed for users upgrading
     from a build where they had enabled it. Drop this when the TODO in
     DisplayOpenGLView.m re-enables the setting. */
  [[NSUserDefaults standardUserDefaults] setBool:NO forKey:@"bilinear"];
}

- (void)windowDidLoad 
{ 
  [super windowDidLoad];
  NSWindow *window = [self window];
  
  // Try to restore saved frame, but if it doesn't exist, center the window
  if (![window setFrameUsingName:@"PreferencesWindow"]) {
    [window center];
  }
} 

- (void)windowDidMove: (NSNotification *)aNotification 
{ 
  [[self window] saveFrameUsingName:@"PreferencesWindow"];
} 

- (id)init
{
  self = [super initWithWindowNibName:@"Preferences"];

  speakerTypes = cocoa_sound_speaker_type();
  soundStereoAY = cocoa_sound_stereo_ay();
  diskTryMerge = cocoa_diskoptions_disk_try_merge();
  movieCompression = cocoa_movie_movie_compr();
  phantomTypistMode = cocoa_media_phantom_typist_mode();
  generalSoundRam = cocoa_peripherals_sound_general_sound_ram();

  return self;
}

- (void)dealloc
{
  [machineRomsController setContent:nil];
  [machineRoms release];
  machineRoms = nil;

  [super dealloc];
}

- (void)awakeFromNib
{
  NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
  NSToolbarItem *item = [[toolbar items] objectAtIndex:[defaults integerForKey:@"preferencestab"]];
  [toolbar setSelectedItemIdentifier:[item itemIdentifier]];
  [self selectPrefPanel:item];

  /* The xib's static enabled="NO" attribute is silently dropped by ibtool
     for buttons with a value binding (the compiled nib has NSEnabled=true),
     so disable it here instead. Drop this when the TODO in
     DisplayOpenGLView.m re-enables bilinear filtering. */
  [bilinearCheckbox setEnabled:NO];
}

- (void)showWindow:(id)sender
{
  if( [[self window] isVisible] ) {
    [[self window] makeKeyAndOrderFront:sender];
    return;
  }

  [[DisplayOpenGLView instance] pause];

  [machineRomsController setContent:nil];
  [machineRoms release];
  machineRoms = nil;
  
  /* Values in Fuse may have been updated, put them in saved settings */
  settings_write_config( &settings_current );

  machineRoms = settings_set_rom_array( &settings_current );
  [machineRoms retain];
  [machineRomsController setContent:machineRoms];

  [super showWindow:sender];

  /* Pin to the main emulator window. The Joystick sub-dialog adds
     itself as a child of Preferences when opened, producing a
     main → Preferences → Joystick chain. */
  [[self window] pinAsChildOf:[[DisplayOpenGLView instance] window]];

  [self setMassStorageType];
  [self setExternalSoundType];
  
  [self fixPhantomTypistMode];

  [[NSNotificationCenter defaultCenter] addObserver:self
		 selector:@selector(handleWillClose:)
			 name:@"NSWindowWillCloseNotification"
		   object:[self window]];
}

- (void)fixPhantomTypistMode
{
  NSUserDefaults *currentValues = [NSUserDefaults standardUserDefaults];
  const char *setting = settings_current.phantom_typist_mode;
  
  if( strcasecmp( setting, "Keyword" ) == 0 ) {
    [currentValues setObject:@"Keyword" forKey:@"phantomtypistmode"];
  } else if( strcasecmp( setting, "Keystroke" ) == 0) {
    [currentValues setObject:@"Keystroke" forKey:@"phantomtypistmode"];
  } else if( strcasecmp( setting, "Menu" ) == 0) {
    [currentValues setObject:@"Menu" forKey:@"phantomtypistmode"];
  } else if( strcasecmp( setting, "Plus 2A" ) == 0 ||
            strcasecmp( setting, "plus2a" ) == 0) {
    [currentValues setObject:@"Plus 2A" forKey:@"phantomtypistmode"];
  } else if( strcasecmp( setting, "Plus 3" ) == 0 ||
            strcasecmp( setting, "plus3" ) == 0) {
    [currentValues setObject:@"Plus 3" forKey:@"phantomtypistmode"];
  } else if( strcasecmp( setting, "Disabled" ) == 0) {
    [currentValues setObject:@"Disabled" forKey:@"phantomtypistmode"];
  } else {
    [currentValues setObject:@"Auto" forKey:@"phantomtypistmode"];
  }
}

- (void)handleWillClose:(NSNotification *)note
{
  [[NSNotificationCenter defaultCenter] removeObserver:self];

  if( appTerminating ) return;

  int old_bilinear = settings_current.bilinear_filter;
  int old_joy1_number = settings_current.joy1_number;
  int old_joy2_number = settings_current.joy2_number;

  /* Values in shared defaults have been updated, pass them onto Fuse */
  read_config_file( &settings_current );

  if( strcmp( machine_current->id, settings_current.start_machine ) ) {
    NSArray *romURLs = [Emulator beginROMScopedAccess];
    machine_select_id( settings_current.start_machine );
    [Emulator endROMScopedAccess:romURLs];
  }

  // B&W TV status may have changed
  display_refresh_all();

  scaler_type prev_scaler = current_scaler;

  /* Skip silently for unknown ids (e.g. defaults written by an older build
     with a now-removed scaler) rather than show ui_error from
     scaler_select_id every time Preferences closes. */
  int new_scaler = scaler_get_type( settings_current.start_scaler_mode );
  if( new_scaler >= 0 && new_scaler != current_scaler ) {
    /* Changing the scaler hotswaps graphics; selecting the current scaler
       would also hotswap and resize the emulator window when Preferences closes. */
    scaler_select_scaler( new_scaler );
  }

  /* For bilinear-only changes, the hotswap inside scaler_select_scaler
     didn't fire; trigger one explicitly so createTexture picks up the new
     filter. */
  if( current_scaler == prev_scaler &&
      old_bilinear != settings_current.bilinear_filter ) {
    uidisplay_hotswap_gfx_mode();
  }

  settings_get_rom_array( &settings_current, machineRoms );

  int joystick_changed = old_joy1_number != settings_current.joy1_number ||
                         old_joy2_number != settings_current.joy2_number;
  if( joystick_changed ) {
    joystick_end();
    joystick_init( NULL );
  }

  {
    NSArray *romURLs = [Emulator beginROMScopedAccess];
    periph_posthook();
    [Emulator endROMScopedAccess:romURLs];
  }

  [machineRomsController setContent:nil];
  [machineRoms release];
  machineRoms = nil;

  [[self window] unpinFromParent];

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)chooseFile:(id)sender
{
  NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];

  char buffer[PATH_MAX+1];
  int result;
  NSSavePanel *sPanel = [NSSavePanel savePanel];

  switch( [sender tag] ) {
  case 0:	/* graphic */
    [sPanel setAllowedFileTypes:@[@"pbm"]];
    break;
  case 1:	/* text */
    [sPanel setAllowedFileTypes:@[@"txt"]];
    break;
  }

  result = [sPanel runModal];
  if (result == NSOKButton) {
    NSString *oFile = [[sPanel URL] path];
    [oFile getFileSystemRepresentation:buffer maxLength:PATH_MAX];

    switch( [sender tag] ) {
    case 0:	/* graphic */
      [defaults setObject:@(buffer) forKey:@"graphicsfile"];
      break;
    case 1:	/* text */
      [defaults setObject:@(buffer) forKey:@"textfile"];
      break;
    }
    
    printer_end();
    printer_init( NULL );
  }
}

- (IBAction)setup:(id)sender
{
  if( !joystickConfigurationController ) {
    joystickConfigurationController = [[JoystickConfigurationController alloc]
                                        init];
  }

  [joystickConfigurationController showWindow:sender];
}

- (NSArray *)machines
{
    return [Machine allMachines];
}

- (NSArray *)joysticks
{
    return [Joystick allJoysticks];
}

- (NSArray *)sdlJoysticks
{
    return [HIDJoystick allJoysticks];
}

- (IBAction)chooseROMFile:(id)sender
{
  char buffer[PATH_MAX+1];
  int result;
  NSOpenPanel *oPanel = [NSOpenPanel openPanel];
  NSArray *romFileTypes = @[@"rom", @"ROM"];
  NSString *romString;

  [oPanel setAllowedFileTypes:romFileTypes];
  result = [oPanel runModal];
  if (result == NSOKButton) {
    NSString *key = NULL;
    NSString *oFile = [[oPanel URL] path];
    [oFile getFileSystemRepresentation:buffer maxLength:PATH_MAX];

    romString = @(buffer);

    [[AppSandboxFileAccess fileAccess] persistPermissionPath:oFile];

    switch( [sender tag] ) {
    case 0:
      key = @"rom0";
      break;
    case 1:
      key = @"rom1";
      break;
    case 2:
      key = @"rom2";
      break;
    case 3:
      key = @"rom3";
      break;
    }

    // Update underlying model
    [[machineRomsController selection] setValue:romString forKey:key];
  }
}

/* The General Sound ROM is the card's own, not one of the machine's, so it
   is held in the defaults rather than the machine ROM table. */
- (IBAction)chooseGeneralSoundROM:(id)sender
{
  NSOpenPanel *oPanel = [NSOpenPanel openPanel];

  [oPanel setAllowedFileTypes:@[@"rom", @"ROM"]];

  if( [oPanel runModal] == NSOKButton ) {
    NSString *oFile = [[oPanel URL] path];

    [[AppSandboxFileAccess fileAccess] persistPermissionPath:oFile];
    [[NSUserDefaults standardUserDefaults] setObject:oFile
                                              forKey:@"romgeneralsound"];
  }
}

- (IBAction)resetROMFile:(id)sender
{
  NSString *romString;
  NSString *source_key = nil;
  NSString *dest_key = nil;

  switch( [sender tag] ) {
  case 0:
    source_key = @"default_rom0";
    dest_key = @"rom0";
    break;
  case 1:
    source_key = @"default_rom1";
    dest_key = @"rom1";
    break;
  case 2:
    source_key = @"default_rom2";
    dest_key = @"rom2";
    break;
  case 3:
    source_key = @"default_rom3";
    dest_key = @"rom3";
    break;
  }

  romString = [[machineRomsController selection] valueForKey:source_key];

  // Update underlying model
  [[machineRomsController selection] setValue:romString forKey:dest_key];
}

- (IBAction)resetUserDefaults:(id)sender
{
  int error;
  NSMutableArray *newMachineRoms;
  unsigned int i;

  error = NSRunAlertPanel(@"Are you sure you want to reset all your preferences to the default settings?", @"Fuse will change all custom settings to the values set when the program was first installed.", @"Cancel", @"OK", nil);

  if( error != NSAlertAlternateReturn ) return;

  [[NSUserDefaultsController sharedUserDefaultsController] revertToInitialValues:self];

  error = read_config_file( &settings_current );
  if( error ) ui_error( UI_ERROR_ERROR, "Error resetting preferences" );

  newMachineRoms = settings_set_rom_array( &settings_current );
  for( i=0; i<[newMachineRoms count]; i++ ) {
	[self replaceObjectInMachineRomsAtIndex:i withObject:[newMachineRoms objectAtIndex:i]];
  }
}

- (void)setCurrentValue:(int)value forKey:(NSString *)key inValues:(NSUserDefaults *)currentValues
{
  BOOL val = value ? YES : NO;
  [currentValues setObject:@(val) forKey:key];
}

- (IBAction)massStorageTypeClicked:(id)sender
{
  NSUserDefaults *currentValues = [NSUserDefaults standardUserDefaults];

  settings_current.interface1 = 0;
  settings_current.simpleide_active = 0;
  settings_current.zxatasp_active = 0;
  settings_current.zxcf_active = 0;
  settings_current.divide_enabled = 0;
  settings_current.plusd = 0;
  settings_current.beta128 = 0;
  settings_current.opus = 0;
  settings_current.disciple = 0;
  settings_current.spectranet = 0;
  settings_current.didaktik80 = 0;
  settings_current.usource = 0;
  settings_current.divmmc_enabled = 0;
  settings_current.zxmmc_enabled = 0;
  settings_current.ttx2000s = 0;

  // Read mass storage type box and set text boxes appropriately
  switch( [[massStorageType selectedCell] tag] ) {
  case 0: // None
    break;
  case 1: // Interface 1
    settings_current.interface1 = 1;
    break;
  case 2: // Simple 8-bit IDE
    settings_current.simpleide_active = 1;
    break;
  case 3: // ZXATASP
    settings_current.zxatasp_active = 1;
    break;
  case 4: // ZXCF
    settings_current.zxcf_active = 1;
    break;
  case 5: // DivIDE
    settings_current.divide_enabled = 1;
    break;
  case 6: // PlusD
    settings_current.plusd = 1;
    break;
  case 7: // Beta 128
    settings_current.beta128 = 1;
    break;
  case 8: // Opus Discovery
    settings_current.opus = 1;
    break;
  case 9: // DISCiPLE
    settings_current.disciple = 1;
    break;
  case 10: // Spectranet
    settings_current.spectranet = 1;
    break;
  case 11: // Didaktik 80
    settings_current.didaktik80 = 1;
    break;
  case 12: // Currah µSource
    settings_current.usource = 1;
    break;
  case 13: // DivMMC
    settings_current.divmmc_enabled = 1;
    break;
  case 14: // ZXMMC
    settings_current.zxmmc_enabled = 1;
    break;
  case 15: // ttx2000s
    settings_current.ttx2000s = 1;
    break;
  default: // WTF?
    break;
  }

  [self setCurrentValue:settings_current.interface1 forKey:@"interface1" inValues:currentValues];
  [self setCurrentValue:settings_current.simpleide_active forKey:@"simpleide" inValues:currentValues];
  [self setCurrentValue:settings_current.zxatasp_active forKey:@"zxatasp" inValues:currentValues];
  [self setCurrentValue:settings_current.zxcf_active forKey:@"zxcf" inValues:currentValues];
  [self setCurrentValue:settings_current.divide_enabled forKey:@"divide" inValues:currentValues];
  [self setCurrentValue:settings_current.plusd forKey:@"plusd" inValues:currentValues];
  [self setCurrentValue:settings_current.beta128 forKey:@"beta128" inValues:currentValues];
  [self setCurrentValue:settings_current.opus forKey:@"opus" inValues:currentValues];
  [self setCurrentValue:settings_current.disciple forKey:@"disciple" inValues:currentValues];
  [self setCurrentValue:settings_current.spectranet forKey:@"spectranet" inValues:currentValues];
  [self setCurrentValue:settings_current.didaktik80 forKey:@"didaktik80" inValues:currentValues];
  [self setCurrentValue:settings_current.usource forKey:@"usource" inValues:currentValues];
  [self setCurrentValue:settings_current.divmmc_enabled forKey:@"divmmc" inValues:currentValues];
  [self setCurrentValue:settings_current.zxmmc_enabled forKey:@"zxmmc" inValues:currentValues];
  [self setCurrentValue:settings_current.ttx2000s forKey:@"ttx2000s" inValues:currentValues];

  [currentValues synchronize];
}

- (IBAction)externalSoundTypeClicked:(id)sender
{
  NSUserDefaults *currentValues = [NSUserDefaults standardUserDefaults];

  settings_current.fuller = 0;
  settings_current.melodik = 0;
  settings_current.specdrum = 0;
  settings_current.covox = 0;

  // Read external sound interface type box and set text boxes appropriately
  switch( [[externalSoundType selectedCell] tag] ) {
  case 0: // None
    break;
  case 1: // Fuller
    settings_current.fuller = 1;
    break;
  case 2: // Melodik
    settings_current.melodik = 1;
    break;
  case 3: // SpecDrum
    settings_current.specdrum = 1;
    break;
  case 4: // Covox
    settings_current.covox = 1;
    break;
  default: // WTF?
    break;
  }

  [self setCurrentValue:settings_current.fuller forKey:@"fuller" inValues:currentValues];
  [self setCurrentValue:settings_current.melodik forKey:@"melodik" inValues:currentValues];
  [self setCurrentValue:settings_current.specdrum forKey:@"specdrum" inValues:currentValues];
  [self setCurrentValue:settings_current.covox forKey:@"covox" inValues:currentValues];

  [currentValues synchronize];
}

- (IBAction)multifaceTypeClicked:(id)sender
{
  NSUserDefaults *currentValues = [NSUserDefaults standardUserDefaults];

  settings_current.multiface1 = 0;
  settings_current.multiface128 = 0;
  settings_current.multiface3 = 0;

  // Read external sound interface type box and set text boxes appropriately
  switch( [[multifaceType selectedCell] tag] ) {
    case 0: // None
      break;
    case 1: // Multiface One
      settings_current.multiface1 = 1;
      break;
    case 2: // Multiface 128
      settings_current.multiface128 = 1;
      break;
    case 3: // Multiface 3
      settings_current.multiface3 = 1;
      break;
    default: // WTF?
      break;
  }

  [self setCurrentValue:settings_current.multiface1 forKey:@"multiface1" inValues:currentValues];
  [self setCurrentValue:settings_current.multiface128 forKey:@"multiface128" inValues:currentValues];
  [self setCurrentValue:settings_current.multiface3 forKey:@"multiface3" inValues:currentValues];

  [currentValues synchronize];
}

- (IBAction)selectPrefPanel:(id)item
{
  NSString *sender;

  if( item == nil ){  //set the pane to the default.
    sender = @"General";
    [toolbar setSelectedItemIdentifier:sender];
  } else {
    sender = [item label];
  }

  NSWindow *window = [self window];

  // make a temp pointer.
  NSView *prefsView = generalPrefsView;

  // set the title to the name of the Preference Item.
  [window setTitle:sender];

  if( [sender isEqualToString:@"Sound"] ) {
    prefsView = soundPrefsView;
  } else if( [sender isEqualToString:@"Peripherals"] ) {
    prefsView = peripheralsPrefsView;
  } else if( [sender isEqualToString:@"Recording"] ) {
    prefsView = rzxPrefsView;
  } else if( [sender isEqualToString:@"Inputs"] ) {
    prefsView = joysticksPrefsView;
  } else if( [sender isEqualToString:@"ROM"] ) {
    prefsView = romPrefsView;
  } else if( [sender isEqualToString:@"Machine"] ) {
    prefsView = machinePrefsView;
  } else if( [sender isEqualToString:@"Video"] ) {
    prefsView = filterPrefsView;
  } else if( [sender isEqualToString:@"Debugger"] ) {
     prefsView = debuggerPrefsView;
   }

  // to stop flicker, we make a temp blank view.
  NSView *tempView = [[NSView alloc] initWithFrame:[[window contentView] frame]];
  [window setContentView:tempView];
  [tempView release];

  // mojo to get the right frame for the new window.
  NSRect newFrame = [window frame];
  newFrame.size.height = [prefsView frame].size.height +
    ([window frame].size.height - [[window contentView] frame].size.height);
  newFrame.origin.y += ([[window contentView] frame].size.height -
                        [prefsView frame].size.height);

  // set the frame to newFrame and animate it.
  [window setShowsResizeIndicator:YES];
  [window setFrame:newFrame display:YES animate:YES];
  // set the main content view to the new view we have picked through the if structure above.
  [window setContentView:prefsView];

  [[NSUserDefaults standardUserDefaults]
    setObject:@([item tag]) forKey:@"preferencestab"];
}

// NSToolbar delegate method
- (NSArray *)toolbarSelectableItemIdentifiers:(NSToolbar *)bar
{
  // Every toolbar icon is selectable
  return [[bar items] valueForKey:@"itemIdentifier"];
}

- (void)setMassStorageType
{
  int value = 0;
  if( settings_current.interface1 ) {
    value = 1;
  } else if ( settings_current.simpleide_active ) {
    value = 2;
  } else if ( settings_current.zxatasp_active ) {
    value = 3;
  } else if ( settings_current.zxcf_active ) {
    value = 4;
  } else if ( settings_current.divide_enabled ) {
    value = 5;
  } else if ( settings_current.plusd ) {
    value = 6;
  } else if ( settings_current.beta128 ) {
    value = 7;
  } else if ( settings_current.opus ) {
    value = 8;
  } else if ( settings_current.disciple ) {
    value = 9;
  } else if ( settings_current.spectranet ) {
    value = 10;
  } else if ( settings_current.didaktik80 ) {
    value = 11;
  } else if ( settings_current.usource ) {
    value = 12;
  } else if ( settings_current.divmmc_enabled ) {
    value = 13;
  } else if ( settings_current.zxmmc_enabled ) {
    value = 14;
  } else if ( settings_current.ttx2000s ) {
    value = 15;
  }

  [massStorageType selectCellWithTag:value];
}

- (void)setExternalSoundType
{
  int value = 0;
  if( settings_current.fuller ) {
    value = 1;
  } else if ( settings_current.melodik ) {
    value = 2;
  } else if ( settings_current.specdrum ) {
    value = 3;
  } else if ( settings_current.covox ) {
    value = 4;
  }

  [externalSoundType selectCellWithTag:value];
}

- (unsigned int)countOfMachineRoms
{
  return [machineRoms count];
}

- (id)objectInMachineRomsAtIndex:(unsigned int)index
{
  return [machineRoms objectAtIndex:index];
}

- (void)insertObject:(id)anObject inMachineRomsAtIndex:(unsigned int)index
{
  [machineRoms insertObject:anObject atIndex:index];
}

- (void)removeObjectFromMachineRomsAtIndex:(unsigned int)index
{
  [machineRoms removeObjectAtIndex:index];
}

- (void)replaceObjectInMachineRomsAtIndex:(unsigned int)index withObject:(id)anObject
{
  [machineRoms replaceObjectAtIndex:index withObject:anObject];
}

@end
