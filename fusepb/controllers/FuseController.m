/* FuseController.m: Routines for dealing with the Cocoa user interface
   Copyright (c) 2000-2017 Philip Kendall, Russell Marks, Fredrick Meunier,
                           Mark Grebe <atarimac@cox.net>

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

#include <config.h>
#include <limits.h>
#include <string.h>
#include <ctype.h>

#import "DebuggerController.h"
#import "FuseController.h"
#import "KeyboardController.h"
#import "LoadBinaryController.h"
#import "MemoryBrowserController.h"
#import "PokeFinderController.h"
#import "PokeMemoryController.h"
#import "PreferencesController.h"
#import "RollbackController.h"
#import "SaveBinaryController.h"
#import "TapeBrowserController.h"
#import "AppSandboxFileAccess.h"
#import <Sparkle/Sparkle.h>

#import "DisplayOpenGLView.h"

#include "compat.h"
#include "debugger/debugger.h"
#include "event.h"
#include "fuse.h"
#include "if1.h"
#include "libspectrum.h"
#include "movie.h"
#include "rzx.h"
#include "psg.h"
#include "settings.h"
#include "settings_cocoa.h"
#include "snapshot.h"
#include "tape.h"
#include "timer.h"
#include "ui.h"
#include "uimedia.h"
#include "uidisplay.h"
#include "utils.h"

static char* cocoaui_openpanel_get_filename( NSString *title, NSArray *fileTypes );
static char* cocoaui_savepanel_get_filename( NSString *title, NSArray *fileTypes );
static void cocoaui_disk_eject( int drive );
static void cocoaui_disk_save( int drive, int saveas );
static void cocoaui_disk_flip( int drive, int flip );
static void cocoaui_disk_write_protect( int drive, int wrprot );
static void cocoaui_mdr_eject( int drive );
static void cocoaui_mdr_save( int drive, int saveas );
int cocoaui_confirm( const char *message );

static int dockEject = 0;
static int didaktik80Snap = 0;
static int debuggerEnabled = 1;
static int if2Eject = 0;
static int diskPlus3EjectA = 0;
static int diskPlus3EjectB = 0;
static int diskTrdosEjectA = 0;
static int diskTrdosEjectB = 0;
static int diskOpusEjectA = 0;
static int diskOpusEjectB = 0;
static int diskPlusDEjectA = 0;
static int diskPlusDEjectB = 0;
static int diskDiscipleEjectA = 0;
static int diskDiscipleEjectB = 0;
static int diskDidaktikEjectA = 0;
static int diskDidaktikEjectB = 0;
static BOOL diskPlus3FlipA = NO;
static BOOL diskPlus3FlipB = NO;
static BOOL diskTrDosFlipA = NO;
static BOOL diskTrDosFlipB = NO;
static BOOL diskOpusFlipA = NO;
static BOOL diskOpusFlipB = NO;
static BOOL diskPlusDFlipA = NO;
static BOOL diskPlusDFlipB = NO;
static BOOL diskDiscipleFlipA = NO;
static BOOL diskDiscipleFlipB = NO;
static BOOL diskDidaktikFlipA = NO;
static BOOL diskDidaktikFlipB = NO;
static BOOL diskPlus3WpA = NO;
static BOOL diskPlus3WpB = NO;
static BOOL diskTrDosWpA = NO;
static BOOL diskTrDosWpB = NO;
static BOOL diskOpusWpA = NO;
static BOOL diskOpusWpB = NO;
static BOOL diskPlusDWpA = NO;
static BOOL diskPlusDWpB = NO;
static BOOL diskDiscipleWpA = NO;
static BOOL diskDiscipleWpB = NO;
static BOOL diskDidaktikWpA = NO;
static BOOL diskDidaktikWpB = NO;
static int record = 1;
static int recordFromSnapshot = 1;
static int play = 1;
static int stop = 0;
static int insert_snap = 0;
static int rollback = 0;
static int rollback_to = 0;
static int movieRecord = 1;
static int movieRecordFromSnapshot = 1;
static int movieStop = 0;
static int moviePause = 0;
static int multiface = 0;
static int recordPsg = 1;
static int stopPsg = 0;
static int ideDivideEjectMaster = 0;
static int ideDivideEjectSlave = 0;
static int ideSimple8BitEjectMaster = 0;
static int ideSimple8BitEjectSlave = 0;
static int ideZxataspEjectMaster = 0;
static int ideZxataspEjectSlave = 0;
static int ideZxcfEject = 0;
static int ideDivmmcEject = 0;
static int ideZxmmcEject = 0;
static int if1M1Eject = 0;
static int if1M2Eject = 0;
static int if1M3Eject = 0;
static int if1M4Eject = 0;
static int if1M5Eject = 0;
static int if1M6Eject = 0;
static int if1M7Eject = 0;
static int if1M8Eject = 0;
static int profileStart = 1;
static int profileStop = 0;
static int playTape = 1;

/* True if we were paused via the Machine/Pause menu item */
static int paused = 0;

static NSMutableArray *allFileTypes = nil;
static NSMutableArray *dckFileTypes = nil;
static NSMutableArray *ideFileTypes = nil;
static NSMutableArray *mdrFileTypes = nil;
static NSMutableArray *plus3FileTypes = nil;
static NSMutableArray *romFileTypes = nil;
static NSMutableArray *rzxFileTypes = nil;
static NSMutableArray *scrFileTypes = nil;
static NSMutableArray *snapFileTypes = nil;
static NSMutableArray *tapeFileTypes = nil;
static NSMutableArray *betaFileTypes = nil;
static NSMutableArray *plusdFileTypes = nil;
static NSMutableArray *opusFileTypes = nil;
static NSMutableArray *didaktikFileTypes = nil;
static NSMutableArray *pokFileTypes = nil;

static NSSavePanel *sPanel = nil;

const char* connection_names[] = {
  "the keyboard",
  "joystick 1",
  "joystick 2",
};

#define QZ_s      0x01
#define QZ_h      0x04
#define QZ_z      0x06
#define QZ_b      0x0B
#define QZ_q      0x0C
#define QZ_r      0x0F
#define QZ_o      0x1F
#define QZ_p      0x23
#define QZ_j      0x26
#define QZ_k      0x28
#define QZ_SLASH  0x2C
#define QZ_PERIOD 0x2F

static int
get_microdrive_no( int tag ) {
  switch( tag ) {
  case 30:  case 31:  case 32:  case 33:  case 34:  return 0; break;
  case 80:  case 81:  case 82:  case 83:  case 84:  return 1; break;
  case 90:  case 91:  case 92:  case 93:  case 94:  return 2; break;
  case 100: case 101: case 102: case 103: case 104: return 3; break;
  case 110: case 111: case 112: case 113: case 114: return 4; break;
  case 120: case 121: case 122: case 123: case 124: return 5; break;
  case 130: case 131: case 132: case 133: case 134: return 6; break;
  case 140: case 141: case 142: case 143: case 144: return 7; break;
  }

  return 0;
}

static int
is_beta_active( void ) {
  return ( machine_current->capabilities &
                      LIBSPECTRUM_MACHINE_CAPABILITY_TRDOS_DISK ||
           periph_is_active( PERIPH_TYPE_BETA128 ) ||
           periph_is_active( PERIPH_TYPE_BETA128_PENTAGON ) ||
           periph_is_active( PERIPH_TYPE_BETA128_PENTAGON_LATE ) );
}

static NSString *
cocoaui_user_folder_path( NSString *folderName )
{
  return [[NSString stringWithUTF8String:compat_get_config_path()]
          stringByAppendingPathComponent:folderName];
}

static void
cocoaui_open_user_folder( NSString *folderName, NSString *displayName )
{
  NSString *path = cocoaui_user_folder_path( folderName );
  NSFileManager *fileManager = [NSFileManager defaultManager];
  NSError *error = nil;
  BOOL isDirectory = NO;

  if( [fileManager fileExistsAtPath:path isDirectory:&isDirectory] &&
      !isDirectory ) {
    NSRunAlertPanel( @"Cannot Open Folder",
                     @"%@ exists, but it is not a folder.", @"OK", nil, nil,
                     path );
    return;
  }

  if( !isDirectory &&
      ![fileManager createDirectoryAtPath:path withIntermediateDirectories:YES
                              attributes:nil error:&error] ) {
    NSRunAlertPanel( @"Cannot Open Folder",
                     @"Could not create the %@ folder at %@. %@", @"OK", nil,
                     nil, displayName, path, [error localizedDescription] );
    return;
  }

  if( ![[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:path]] ) {
    NSRunAlertPanel( @"Cannot Open Folder",
                     @"Could not open the %@ folder at %@.", @"OK", nil, nil,
                     displayName, path );
  }
}

@implementation FuseController

static FuseController *singleton = nil;

static NSMutableArray *recentSnapFileNames = nil;

+ (FuseController *)singleton
{
  return singleton ? singleton : [[self alloc] init];
}

- (id)init
{
  if ( singleton ) {
    [self dealloc];
  } else {
    [super init];
    singleton = self;
    sparkleUpdaterController = [[SPUStandardUpdaterController alloc] initWithStartingUpdater:YES updaterDelegate:nil userDriverDelegate:nil];

    NSArray *compressedFileTypes = @[@"gz", @"GZ", @"bz2", @"BZ2", @"zip",
                                     @"ZIP"];

    snapFileTypes = [NSMutableArray arrayWithObjects:@"mgtsnp", @"MGTSNP",
                      @"slt", @"SLT", @"sna", @"SNA", @"sp", @"SP", @"szx",
                      @"SZX", @"snp", @"SNP", @"z80", @"Z80", @"zxs", @"ZXS",
                      nil];
    [snapFileTypes retain];

    dckFileTypes = [NSMutableArray arrayWithObjects:@"dck", @"DCK", nil];
    [dckFileTypes retain];

    romFileTypes = [NSMutableArray arrayWithObjects:@"rom", @"ROM", nil];
    [romFileTypes retain];

    ideFileTypes = [NSMutableArray arrayWithObjects:@"hdf", @"HDF", nil];
    [ideFileTypes retain];

    mdrFileTypes = [NSMutableArray arrayWithObjects:@"mdr", @"MDR", nil];
    [mdrFileTypes retain];

    rzxFileTypes = [NSMutableArray arrayWithObjects:@"rzx", @"RZX", nil];
    [rzxFileTypes retain];

    scrFileTypes = [NSMutableArray arrayWithObjects:@"scr", @"SCR", @"mlt",
                    @"MLT", nil];
    [scrFileTypes retain];

    tapeFileTypes = [NSMutableArray arrayWithObjects:@"csw", @"ltp", @"pzx",
                      @"raw", @"spc", @"sta", @"tap", @"tzx", @"wav", @"CSW",
                      @"LTP", @"PZX", @"RAW", @"SPC", @"STA", @"TAP", @"TZX",
                      @"WAV", nil];
    [tapeFileTypes retain];

    plus3FileTypes = [NSMutableArray arrayWithObjects:@"dsk", @"DSK", nil];
    [plus3FileTypes retain];

    betaFileTypes = [NSMutableArray arrayWithObjects:@"trd", @"TRD", @"scl",
                        @"SCL", @"udi", @"UDI", @"fdi", @"FDI", nil];
    [betaFileTypes retain];

    plusdFileTypes = [NSMutableArray arrayWithObjects:@"dsk", @"DSK", @"mgt",
                                                      @"MGT",  @"img", @"IMG",
                                                      nil];
    [plusdFileTypes retain];

    opusFileTypes = [NSMutableArray arrayWithObjects:@"dsk", @"DSK", @"opd",
                                                      @"OPD",  @"opu", @"OPU",
                                                      nil];
    [opusFileTypes retain];

    didaktikFileTypes = [NSMutableArray arrayWithObjects:@"d40", @"D40", @"d80",
                                                         @"D80", nil];
    [didaktikFileTypes retain];
    
    pokFileTypes = [NSMutableArray arrayWithObjects:@"pok", @"POK", nil];
    [pokFileTypes retain];
    
    allFileTypes = [NSMutableArray arrayWithArray:snapFileTypes];
    [allFileTypes addObjectsFromArray:dckFileTypes];
    [allFileTypes addObjectsFromArray:romFileTypes];
    [allFileTypes addObjectsFromArray:ideFileTypes];
    [allFileTypes addObjectsFromArray:mdrFileTypes];
    [allFileTypes addObjectsFromArray:rzxFileTypes];
    [allFileTypes addObjectsFromArray:scrFileTypes];
    [allFileTypes addObjectsFromArray:tapeFileTypes];
    [allFileTypes addObjectsFromArray:plus3FileTypes];
    [allFileTypes addObjectsFromArray:betaFileTypes];
    [allFileTypes addObjectsFromArray:opusFileTypes];
    [allFileTypes addObjectsFromArray:plusdFileTypes];
    [allFileTypes addObjectsFromArray:didaktikFileTypes];
    [allFileTypes addObjectsFromArray:pokFileTypes];
    [allFileTypes retain];

    [snapFileTypes addObjectsFromArray:compressedFileTypes];
    [dckFileTypes addObjectsFromArray:compressedFileTypes];
    [romFileTypes addObjectsFromArray:compressedFileTypes];
    [ideFileTypes addObjectsFromArray:compressedFileTypes];
    [mdrFileTypes addObjectsFromArray:compressedFileTypes];
    [rzxFileTypes addObjectsFromArray:compressedFileTypes];
    [scrFileTypes addObjectsFromArray:compressedFileTypes];
    [tapeFileTypes addObjectsFromArray:compressedFileTypes];
    [plus3FileTypes addObjectsFromArray:compressedFileTypes];
    [betaFileTypes addObjectsFromArray:compressedFileTypes];
    [opusFileTypes addObjectsFromArray:compressedFileTypes];
    [plusdFileTypes addObjectsFromArray:compressedFileTypes];
    [didaktikFileTypes addObjectsFromArray:compressedFileTypes];
    [allFileTypes addObjectsFromArray:compressedFileTypes];

    recentSnapFileNames = [NSMutableArray arrayWithCapacity:NUM_RECENT_ITEMS];
    [recentSnapFileNames retain];

    /* Resync the emulator's modifier ivars whenever the main window
       regains key status. AppKit delivers NSFlagsChanged to the key
       window's first responder, so a modifier release while a utility
       window was key never reaches DisplayOpenGLView and
       Emulator.commandDown stays stuck — dropping every subsequent
       keystroke in Emulator.keyDown:. The user-visible form is the
       Cmd+, → Preferences flow: open Settings, close, then the
       Spectrum keyboard ignores everything until the next modifier
       press triggers a real NSFlagsChanged on the main window. The
       filter inside the handler skips notifications for other
       windows. */
    [[NSNotificationCenter defaultCenter]
      addObserver:self
         selector:@selector(mainWindowDidBecomeKey:)
             name:NSWindowDidBecomeKeyNotification
           object:nil];
  }

  return singleton;
}

- (void)mainWindowDidBecomeKey:(NSNotification *)note
{
  /* The observer is registered with object:nil because the main window
     does not yet exist when the FuseController singleton is constructed
     during MainMenu.xib load. Filter to the main emulator window inside
     the handler — and bail out early if the main window does not yet
     exist, otherwise a non-nil note object would falsely pass the !=
     comparison against nil during startup. */
  NSWindow *mainWindow = [[DisplayOpenGLView instance] window];
  if( !mainWindow ) return;
  if( [note object] != mainWindow ) return;

  Emulator *emu = [Emulator instance];
  if( !emu ) return;

  NSEvent *sync = [NSEvent keyEventWithType:NSFlagsChanged
                                   location:NSZeroPoint
                              modifierFlags:[NSEvent modifierFlags]
                                  timestamp:0
                               windowNumber:0
                                    context:nil
                                 characters:@""
                charactersIgnoringModifiers:@""
                                  isARepeat:NO
                                    keyCode:0];
  [emu flagsChanged:sync];
}

- (IBAction)disk_eject_a:(id)sender
{
  cocoaui_disk_eject( 0 );
}

- (IBAction)disk_save_a:(id)sender
{
  cocoaui_disk_save( 0, 0 );
}

- (IBAction)disk_save_as_a:(id)sender
{
  cocoaui_disk_save( 0, 1 );
}

- (IBAction)disk_write_protect_a:(id)sender
{
  cocoaui_disk_write_protect( 0, [sender state] == NSOffState );
}

- (IBAction)disk_flip_a:(id)sender
{
  cocoaui_disk_flip( 0, [sender state] == NSOffState );
}

- (IBAction)disk_eject_b:(id)sender
{
  cocoaui_disk_eject( 1 );
}

- (IBAction)disk_save_b:(id)sender
{
  cocoaui_disk_save( 1, 0 );
}

- (IBAction)disk_save_as_b:(id)sender
{
  cocoaui_disk_save( 1, 1 );
}

- (IBAction)disk_write_protect_b:(id)sender
{
  cocoaui_disk_write_protect( 1, [sender state] == NSOffState );
}

- (IBAction)disk_flip_b:(id)sender
{
  cocoaui_disk_flip( 1, [sender state] == NSOffState );
}

- (IBAction)disk_open_a:(id)sender
{
  [self openDisk:0];
}

- (IBAction)disk_new_a:(id)sender
{
  [self newDisk:0];
}

- (IBAction)disk_open_b:(id)sender
{
  [self openDisk:1];
}

- (IBAction)disk_new_b:(id)sender
{
  [self newDisk:1];
}

- (IBAction)cart_eject:(id)sender
{
  [[DisplayOpenGLView instance] pause];

  if ( libspectrum_machine_capabilities( machine_current->machine ) &
        LIBSPECTRUM_MACHINE_CAPABILITY_TIMEX_DOCK ) {
    [[DisplayOpenGLView instance] dckEject];
  } else {
    [[DisplayOpenGLView instance] if2Eject];
  }

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)cart_open:(id)sender
{
  int error;
  char *filename = NULL;
  NSArray *fileTypes;
  NSString *message;

  [[DisplayOpenGLView instance] pause];

  if ( libspectrum_machine_capabilities( machine_current->machine ) &
        LIBSPECTRUM_MACHINE_CAPABILITY_TIMEX_DOCK ) {
    message = @"Insert Timex dock cartridge";
    fileTypes = dckFileTypes;
  } else {
    message = @"Insert Interface II cartridge";
    fileTypes = romFileTypes;
  }

  filename = cocoaui_openpanel_get_filename( message, fileTypes );

  if( !filename ) { [[DisplayOpenGLView instance] unpause]; return; }

  if ( libspectrum_machine_capabilities( machine_current->machine ) &
        LIBSPECTRUM_MACHINE_CAPABILITY_TIMEX_DOCK ) {
    error = [[DisplayOpenGLView instance] dckInsert:filename];
  } else {
    error = [[DisplayOpenGLView instance] if2Insert:filename];
  }

  if(error) goto error;

  [self addRecentSnapshot:filename];

error:
  free( filename );

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)dock_open:(id)sender
{
  int error;
  char *filename = NULL;

  [[DisplayOpenGLView instance] pause];

  filename = cocoaui_openpanel_get_filename( @"Insert Timex dock cartridge", dckFileTypes );

  if( !filename ) { [[DisplayOpenGLView instance] unpause]; return; }

  error = [[DisplayOpenGLView instance] dckInsert:filename];
  if(error) goto error;

  [self addRecentSnapshot:filename];

error:
  free( filename );

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)dock_eject:(id)sender
{
  [[DisplayOpenGLView instance] pause];

  [[DisplayOpenGLView instance] dckEject];

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)if2_open:(id)sender
{
  int error;
  char *filename = NULL;

  [[DisplayOpenGLView instance] pause];

  filename = cocoaui_openpanel_get_filename( @"Insert Interface II cartridge", romFileTypes );

  if( !filename ) { [[DisplayOpenGLView instance] unpause]; return; }

  error = [[DisplayOpenGLView instance] if2Insert:filename];
  if(error) goto error;

  [self addRecentSnapshot:filename];

error:
  free( filename );

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)if2_eject:(id)sender
{
  [[DisplayOpenGLView instance] pause];

  [[DisplayOpenGLView instance] if2Eject];

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)ide_insert:(id)sender
{
  int error=0;
  char *filename = NULL;
  libspectrum_ide_unit unit;

  [[DisplayOpenGLView instance] pause];

  filename = cocoaui_openpanel_get_filename( @"Insert hard disk file", ideFileTypes );

  if( !filename ) { [[DisplayOpenGLView instance] unpause]; return; }

  unit = [sender tag] == 41 ? LIBSPECTRUM_IDE_MASTER : LIBSPECTRUM_IDE_SLAVE;

  if( settings_current.divide_enabled ) {
    error = [[DisplayOpenGLView instance] divideInsert:filename inUnit:unit];
  } else if( settings_current.simpleide_active ) {
    error = [[DisplayOpenGLView instance] simpleideInsert:filename inUnit:unit];
  } else if( settings_current.zxatasp_active ) {
    error = [[DisplayOpenGLView instance] zxataspInsert:filename inUnit:unit];
  } else if( settings_current.zxcf_active ) {
    error = [[DisplayOpenGLView instance] zxcfInsert:filename];
  } else if( settings_current.divmmc_enabled ) {
    error = [[DisplayOpenGLView instance] divmmcInsert:filename];
  } else if( settings_current.zxmmc_enabled ) {
    error = [[DisplayOpenGLView instance] zxmmcInsert:filename];
  }

  if(error) goto error;

  [self addRecentSnapshot:filename];

error:
  free( filename );

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)ide_commit:(id)sender
{
  libspectrum_ide_unit unit;

  [[DisplayOpenGLView instance] pause];

  unit = [sender tag] == 51 ? LIBSPECTRUM_IDE_MASTER : LIBSPECTRUM_IDE_SLAVE;

  if( settings_current.divide_enabled ) {
    [[DisplayOpenGLView instance] divideCommit:unit];
  } else if( settings_current.simpleide_active ) {
    [[DisplayOpenGLView instance] simpleideCommit:unit];
  } else if( settings_current.zxatasp_active ) {
    [[DisplayOpenGLView instance] zxataspCommit:unit];
  } else if( settings_current.zxcf_active ) {
    [[DisplayOpenGLView instance] zxcfCommit];
  }

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)ide_eject:(id)sender
{
  libspectrum_ide_unit unit;

  unit = [sender tag] == 61 ? LIBSPECTRUM_IDE_MASTER : LIBSPECTRUM_IDE_SLAVE;

  if( settings_current.divide_enabled ) {
    [[DisplayOpenGLView instance] divideEject:unit];
  } else if( settings_current.simpleide_active ) {
    [[DisplayOpenGLView instance] simpleideEject:unit];
  } else if( settings_current.zxatasp_active ) {
    [[DisplayOpenGLView instance] zxataspEject:unit];
  } else if( settings_current.zxcf_active ) {
    [[DisplayOpenGLView instance] zxcfEject];
  }
}

- (IBAction)mdr_insert_new:(id)sender
{
  [[DisplayOpenGLView instance] if1MdrNew:get_microdrive_no( [sender tag] )];
}

- (IBAction)mdr_insert:(id)sender
{
  char *filename = NULL;

  [[DisplayOpenGLView instance] pause];

  filename = cocoaui_openpanel_get_filename( @"Insert microdrive disk file", mdrFileTypes );
  if( !filename ) { [[DisplayOpenGLView instance] unpause]; return; }

  [[DisplayOpenGLView instance] if1MdrInsert:filename
                                     inDrive:get_microdrive_no( [sender tag] )];

  [self addRecentSnapshot:filename];

  free( filename );

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)mdr_eject:(id)sender
{
  cocoaui_mdr_eject( get_microdrive_no( [sender tag] ) );
}

- (IBAction)mdr_save:(id)sender
{
  cocoaui_mdr_save( get_microdrive_no( [sender tag] ), 0 );
}

- (IBAction)mdr_save_as:(id)sender
{
  cocoaui_mdr_save( get_microdrive_no( [sender tag] ), 1 );
}

- (IBAction)mdr_writep:(id)sender
{
  int no = get_microdrive_no( [sender tag] );

  [[DisplayOpenGLView instance] if1MdrWriteProtect:[sender state] == NSOffState
                                inDrive:no];
}

- (IBAction)open:(id)sender
{
  char *filename = NULL;

  [[DisplayOpenGLView instance] pause];

  filename = cocoaui_openpanel_get_filename( @"Open Spectrum File", allFileTypes );

  if( !filename ) { [[DisplayOpenGLView instance] unpause]; return; }

  [self addRecentSnapshot:filename];

  [self openFile:filename];

  free(filename);

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)openSpectranetRAMFSFolder:(id)sender
{
  cocoaui_open_user_folder( @"xfs", @"Spectranext RAMFS" );
}

- (IBAction)openSupplementaryROMsFolder:(id)sender
{
  cocoaui_open_user_folder( @"roms", @"Supplementary ROMs" );
}

- (IBAction)reset:(id)sender
{
  [[DisplayOpenGLView instance] pause];

  [[DisplayOpenGLView instance] reset];

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)nmi:(id)sender
{
  [[DisplayOpenGLView instance] pause];

  [[DisplayOpenGLView instance] nmi];

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)rzx_play:(id)sender
{
  char *recording;

  if( rzx_playback || rzx_recording ) return;

  [[DisplayOpenGLView instance] pause];

  recording = cocoaui_openpanel_get_filename( @"Start Replay", rzxFileTypes );

  if( !recording ) { [[DisplayOpenGLView instance] unpause]; return; }

  [self openFile:recording];

  free( recording );

  display_refresh_all();

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)rzx_insert_snap:(id)sender
{
  [[DisplayOpenGLView instance] rzxInsertSnap];
}

- (IBAction)rzx_rollback:(id)sender
{
  [[DisplayOpenGLView instance] rzxRollback];
}

- (IBAction)rzx_start:(id)sender
{
  char *recording;

  if( rzx_playback || rzx_recording ) return;

  [[DisplayOpenGLView instance] pause];

  recording = cocoaui_savepanel_get_filename( @"Start Recording", @[@"rzx"] );
  if( !recording ) { [[DisplayOpenGLView instance] unpause]; return; }

  [[DisplayOpenGLView instance] rzxStartRecording:recording embedSnapshot:1];

  free( recording );

  ui_menu_activate( UI_MENU_ITEM_RECORDING, 1 );

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)rzx_start_snap:(id)sender
{
  char *snap, *recording;

  if( rzx_playback || rzx_recording ) return;

  [[DisplayOpenGLView instance] pause];

  snap = cocoaui_openpanel_get_filename( @"Load Snapshot", snapFileTypes );
  if( !snap ) { [[DisplayOpenGLView instance] unpause]; return; }

  recording = cocoaui_savepanel_get_filename( @"Start Recording", @[@"rzx"] );
  if( !recording ) { free( snap ); [[DisplayOpenGLView instance] unpause]; return; }

  if( snapshot_read( snap ) ) {
    free( snap ); free( recording ); [[DisplayOpenGLView instance] unpause]; return;
  }

  [[DisplayOpenGLView instance] rzxStartRecording:recording
                                embedSnapshot:settings_current.embed_snapshot];

  free( recording );

  display_refresh_all();

  ui_menu_activate( UI_MENU_ITEM_RECORDING, 1 );

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)rzx_stop:(id)sender
{
  [[DisplayOpenGLView instance] rzxStop];

  ui_menu_activate( UI_MENU_ITEM_RECORDING, 0 );
}

- (IBAction)rzx_continue:(id)sender
{
  char *rzx_filename;
  int error;
  
  if( rzx_playback || rzx_recording ) return;
  
  [[DisplayOpenGLView instance] pause];
  
  rzx_filename = cocoaui_openpanel_get_filename( @"Continue Recording", rzxFileTypes );
  if( !rzx_filename ) { [[DisplayOpenGLView instance] unpause]; return; }
  
  error = [[DisplayOpenGLView instance] rzxContinueRecording:rzx_filename];
  
  if( error != LIBSPECTRUM_ERROR_NONE ) {
    ui_error( UI_ERROR_WARNING, "RZX file cannot be continued" );
  }
  
  free( rzx_filename );
  
  ui_menu_activate( UI_MENU_ITEM_RECORDING, 1 );
  
  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)rzx_finalise:(id)sender
{
  char *rzx_filename;
  int error;
  
  if( rzx_playback || rzx_recording ) return;
  
  [[DisplayOpenGLView instance] pause];
  
  rzx_filename = cocoaui_openpanel_get_filename( @"Finalise Recording", rzxFileTypes );
  if( !rzx_filename ) { [[DisplayOpenGLView instance] unpause]; return; }
  
  error = [[DisplayOpenGLView instance] rzxFinaliseRecording:rzx_filename];
  
  if( error == LIBSPECTRUM_ERROR_NONE ) {
    ui_error( UI_ERROR_INFO, "Emulator recording file finalised" );
  } else {
    ui_error( UI_ERROR_WARNING, "Emulator recording file cannot be finalised" );
  }
  
  free( rzx_filename );
  
  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)psg_start:(id)sender
{
  char *psgfile;

  if( psg_recording ) return;

  [[DisplayOpenGLView instance] pause];

  psgfile = cocoaui_savepanel_get_filename( @"Start AY Sound Recording", @[@"psg"] );
  if( !psgfile ) { [[DisplayOpenGLView instance] unpause]; return; }

  [[DisplayOpenGLView instance] psgStart:psgfile];

  free( psgfile );

  display_refresh_all();

  ui_menu_activate( UI_MENU_ITEM_AY_LOGGING, 1 );

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)psg_stop:(id)sender
{
  if ( !psg_recording ) return;
  [[DisplayOpenGLView instance] psgStop];

  ui_menu_activate( UI_MENU_ITEM_AY_LOGGING, 0 );
}

- (IBAction)save_as:(id)sender
{
  char *filename = NULL;

  [[DisplayOpenGLView instance] pause];

  filename = cocoaui_savepanel_get_filename( @"Save Snapshot As", @[@"szx", @"z80", @"sna"] );

  if( !filename ) goto save_as_exit;

  [[DisplayOpenGLView instance] snapshotWrite:filename];

  [self addRecentSnapshot:filename];

  free( filename );

save_as_exit:
  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)open_screen:(id)sender
{
  char *filename = NULL;

  [[DisplayOpenGLView instance] pause];

  filename = cocoaui_openpanel_get_filename( @"Open Screenshot", scrFileTypes );

  if( !filename ) { [[DisplayOpenGLView instance] unpause]; return; }

  [[DisplayOpenGLView instance] screenshotScrRead:filename];

  [self addRecentSnapshot:filename];

  free( filename );

  display_refresh_all();

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)pause:(id)sender
{
  int error;

  if( paused ) {
    paused = 0;
    ui_statusbar_update( UI_STATUSBAR_ITEM_PAUSED,
                         UI_STATUSBAR_STATE_INACTIVE );
    [[DisplayOpenGLView instance] unpause];
  } else {
    [[DisplayOpenGLView instance] pause];

    /* Stop recording any competition mode RZX file */
    if( rzx_recording && rzx_competition_mode ) {
      ui_error( UI_ERROR_INFO, "Stopping competition mode RZX recording" );
      error = rzx_stop_recording(); if( error ) return;
    }

    paused = 1;
    ui_statusbar_update( UI_STATUSBAR_ITEM_PAUSED, UI_STATUSBAR_STATE_ACTIVE );
  }
  [self setPauseState];
}

- (IBAction)profiler_start:(id)sender
{
  [[DisplayOpenGLView instance] profileStart];
}

- (IBAction)profiler_stop:(id)sender
{
  char *filename = NULL;

  [[DisplayOpenGLView instance] pause];

  filename = cocoaui_savepanel_get_filename( @"Save Profile Data As", @[@"profile"] );
  if( !filename ) { [[DisplayOpenGLView instance] unpause]; return; }

  [[DisplayOpenGLView instance] profileFinish:filename];

  free( filename );

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)save_screen:(id)sender
{
  char *filename = NULL;

  [[DisplayOpenGLView instance] pause];

  filename = cocoaui_savepanel_get_filename( @"Save Screenshot As",
                                             @[@"scr", @"mlt"] );

  if( !filename ) { [[DisplayOpenGLView instance] unpause]; return; }

  [[DisplayOpenGLView instance] screenshotScrWrite:filename];

  [self addRecentSnapshot:filename];

  free( filename );

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)export_screen:(id)sender
{
  char *filename = NULL;

  [[DisplayOpenGLView instance] pause];

  filename = cocoaui_savepanel_get_filename( @"Export Screenshot", @[@"png", @"tiff", @"bmp", @"jpg", @"gif"] );

  if( !filename ) { [[DisplayOpenGLView instance] unpause]; return; }

  [[DisplayOpenGLView instance] screenshotWrite:filename];

  free( filename );

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)save_options:(id)sender
{
  [[DisplayOpenGLView instance] settingsSave];
}

- (IBAction)hard_reset:(id)sender
{
  [[DisplayOpenGLView instance] pause];

  [[DisplayOpenGLView instance] hard_reset];

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)joystick_keyboard:(id)sender
{
  [[DisplayOpenGLView instance] joystickToggleKeyboard];
}

- (IBAction)keyboard_recreated:(id)sender
{
  [[DisplayOpenGLView instance] keyboardToggleRecreatedZXSpectrum];
}

- (IBAction)keyboard_arrows_shifted:(id)sender
{
  [[DisplayOpenGLView instance] keyboardToggleArrowsShifted];
}

- (IBAction)tape_clear:(id)sender
{
  [[DisplayOpenGLView instance] tapeClear];
}

- (IBAction)tape_open:(id)sender
{
  char *filename = NULL;

  [[DisplayOpenGLView instance] pause];

  filename = cocoaui_openpanel_get_filename( @"Open Tape", tapeFileTypes );

  if( !filename ) { [[DisplayOpenGLView instance] unpause]; return; }

  [[DisplayOpenGLView instance] tapeOpen:filename];

  [self addRecentSnapshot:filename];

  free( filename );

  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)tape_play:(id)sender
{
  [[DisplayOpenGLView instance] tapeTogglePlay];
}

- (IBAction)tape_rewind:(id)sender
{
  [[DisplayOpenGLView instance] tapeRewind];
}

- (IBAction)tape_write:(id)sender
{
  [self tapeWrite];
}

- (IBAction)tape_record:(id)sender
{
  [[DisplayOpenGLView instance] tapeToggleRecord];
}

- (IBAction)movie_record:(id)sender
{
  char *filename = NULL;
  
  [[DisplayOpenGLView instance] pause];
  
  filename = cocoaui_savepanel_get_filename( @"Record Movie File", @[@"fmf"] );
  
  if( !filename ) { [[DisplayOpenGLView instance] unpause]; return; }
  
  [[DisplayOpenGLView instance] movieStartRecording:filename];
  
  free( filename );
  
  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)movie_record_from_rzx:(id)sender
{
  char *rzx_file, *fmf_file;
  
  if( rzx_playback || rzx_recording || movie_recording ) return;
  
  [[DisplayOpenGLView instance] pause];
  
  rzx_file = cocoaui_openpanel_get_filename( @"Load Recording", rzxFileTypes );
  if( !rzx_file ) { [[DisplayOpenGLView instance] unpause]; return; }
  
  rzx_start_playback( rzx_file, 1 );
  free( rzx_file );
  display_refresh_all();
  
  if( rzx_playback ) {
    fmf_file = cocoaui_savepanel_get_filename( @"Record Movie File", @[@"fmf"] );
    if( !fmf_file ) {
      rzx_stop_playback( 1 );
      [[DisplayOpenGLView instance] unpause];
      return;
    }
    
    [[DisplayOpenGLView instance] movieStartRecording:fmf_file];
    
    free( fmf_file );
    ui_menu_activate( UI_MENU_ITEM_RECORDING, 1 );
  }
  
  [[DisplayOpenGLView instance] unpause];
}

- (IBAction)movie_pause:(id)sender
{
  [[DisplayOpenGLView instance] movieTogglePause];
}

- (IBAction)movie_stop:(id)sender
{
  [[DisplayOpenGLView instance] movieStop];
}

- (IBAction)didaktik80_snap:(id)sender
{
  [[DisplayOpenGLView instance] didaktik80Snap];
}

- (IBAction)multiface_red_button:(id)sender
{
  [[DisplayOpenGLView instance] multifaceRedButton];
}

- (IBAction)quit:(id)sender
{
  [NSApp terminate:self];
}

- (IBAction)hide:(id)sender
{
  [NSApp hide:self];
}

- (IBAction)help:(id)sender
{
  [NSApp showHelp:self];
}

- (IBAction)cocoa_break:(id)sender
{
  if ( gdbserver_debugging_enabled ) {
    return;
  }

  /* Re-entry: if the Debugger is already showing, bring it forward and
     leave state alone. Calling debugger_activate: a second time would
     double-pause and unbalance the counter. */
  DebuggerController *dbg = [DebuggerController loadedSingleton];
  if ( dbg && [[dbg window] isVisible] ) {
    [[dbg window] makeKeyAndOrderFront:nil];
    return;
  }

  if ( paused ) {
    /* User-pause: release the user-pause contribution so the timer can
       run, then set HALTED. The emulator detects HALTED on the next
       instruction and ui_debugger_activate parks the thread on the
       semaphore via the proper path. The user-pause is not restored on
       dismissal — the user explicitly opened the Debugger from a paused
       state, and Continue means "resume." */
    paused = 0;
    [[DisplayOpenGLView instance] unpause];
  }
  /* The utility-window case (fuse_emulation_paused > 0 && !paused) is
     unreachable: validateMenuItem: disables the Debugger menu item in
     that state. */
  [[DisplayOpenGLView instance] cocoaBreak];
  [self setPauseState];
}

- (IBAction)showRollbackPane:(id)sender
{
  if( !rollbackController ) {
    rollbackController = [[RollbackController alloc] init];
  }
  [rollbackController showWindow:self];
}

- (IBAction)showTapeBrowserPane:(id)sender
{
  if( !tapeBrowserController ) {
    tapeBrowserController = [[TapeBrowserController alloc] init];
  }
  [tapeBrowserController showWindow:self];
}

- (IBAction)showKeyboardPane:(id)sender
{
  if( !keyboardController ) {
    keyboardController = [[KeyboardController alloc] init];
  }
  [keyboardController showCloseWindow:self];
}

- (IBAction)showLoadBinaryPane:(id)sender
{
  if( !loadBinaryController ) {
    loadBinaryController = [[LoadBinaryController alloc] init];
  }

  [loadBinaryController showWindow:self];
}

- (IBAction)showSaveBinaryPane:(id)sender
{
  if( !saveBinaryController ) {
    saveBinaryController = [[SaveBinaryController alloc] init];
  }
  [saveBinaryController showWindow:self];
}

- (IBAction)showPokeFinderPane:(id)sender
{
  if( !pokeFinderController ) {
    pokeFinderController = [[PokeFinderController alloc] init];
  }
  [pokeFinderController showWindow:self];
}

- (IBAction)showPokeMemoryPane:(id)sender
{
  if( !pokeMemoryController ) {
    pokeMemoryController = [[PokeMemoryController alloc] init];
  }
  [pokeMemoryController showWindow:self];
}

- (IBAction)showMemoryBrowserPane:(id)sender
{
  if( !memoryBrowserController ) {
    memoryBrowserController = [[MemoryBrowserController alloc] init];
  }
  [memoryBrowserController showWindow:self];
}

- (IBAction)showPreferencesPane:(id)sender;
{
  if( !preferencesController ) {
    preferencesController = [[PreferencesController alloc] init];
  }
  [preferencesController showWindow:self];
}

- (IBAction)saveFileTypeClicked:(id)sender;
{
  [sPanel setAllowedFileTypes:@[[saveFileType titleOfSelectedItem]]];
}

- savePanelAccessoryView
{
  if (savePanelAccessoryView == nil) {
    [NSBundle loadNibNamed:@"SavePanelAccessoryView.nib" owner:self];
    [savePanelAccessoryView retain];
  }
  return savePanelAccessoryView;
}

@synthesize saveFileType;

- (IBAction)resetUserDefaults:(id)sender
{
  int error;

  error = NSRunAlertPanel(@"Are you sure you want to reset all your preferences to the default settings?", @"Fuse will change all custom settings to the values set when the program was first installed.", @"Cancel", @"OK", nil);

  if( error != NSAlertAlternateReturn ) return;

  [[DisplayOpenGLView instance] settingsResetDefaults];
}

- (IBAction)checkForUpdates:(id)sender
{
  [sparkleUpdaterController checkForUpdates:sender];
}

- (void)dealloc
{
  [[NSNotificationCenter defaultCenter] removeObserver:self];

  [tapeBrowserController release];
  [keyboardController release];
  [saveBinaryController release];
  [loadBinaryController release];
  [pokeFinderController release];
  [preferencesController release];
  [rollbackController release];
  [savePanelAccessoryView release];
  [sparkleUpdaterController release];
  [super dealloc];
}

- (void)ui_menu_activate_media_cartridge:(NSNumber*)active
{
  [cartridge setEnabled:[active boolValue]];
}

- (void)ui_menu_activate_debugger:(NSNumber*)active
{
  debuggerEnabled = [active boolValue];
}

- (void)ui_menu_set_debugger_title:(NSString*)title
{
    [debugger setTitle:title];
}

- (void)ui_menu_activate_media_cartridge_dock:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_cartridge_dock_eject:(NSNumber*)active
{
  dockEject = [active boolValue];
}

- (void)ui_menu_activate_media_cartridge_if2:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_cartridge_if2_eject:(NSNumber*)active
{
  if2Eject = [active boolValue];
}

- (void)ui_menu_activate_media_disk:(NSNumber*)active
{
  [diskA setEnabled:[active boolValue]];
  [diskB setEnabled:[active boolValue]];
}

- (void)ui_menu_activate_media_disk_plus3:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_disk_plus3_a_eject:(NSNumber*)active
{
  diskPlus3EjectA = [active boolValue];
}

- (void)ui_menu_activate_media_disk_a_flip
{
  bool newValue = NO;

  if( !machine_current ) {
  } else if( machine_current->capabilities &
               LIBSPECTRUM_MACHINE_CAPABILITY_PLUS3_DISK ) {
    newValue = diskPlus3FlipA;
  } else if( is_beta_active() ) {
    newValue = diskTrDosFlipA;
  } else if( periph_is_active( PERIPH_TYPE_OPUS ) ) {
    newValue = diskOpusFlipA;
  } else if( periph_is_active( PERIPH_TYPE_DISCIPLE ) ) {
    newValue = diskDiscipleFlipA;
  } else if( periph_is_active( PERIPH_TYPE_DIDAKTIK80 ) ) {
    newValue = diskDidaktikFlipA;
  } else {
    newValue = diskPlusDFlipA;
  }

  [diskFlipA setState:!newValue];
}

- (void)ui_menu_activate_media_disk_b_flip
{
  bool newValue = NO;

  if( !machine_current ) {
  } else if( machine_current->capabilities &
               LIBSPECTRUM_MACHINE_CAPABILITY_PLUS3_DISK ) {
    newValue = diskPlus3FlipB;
  } else if( is_beta_active() ) {
    newValue = diskTrDosFlipB;
  } else if( periph_is_active( PERIPH_TYPE_OPUS ) ) {
    newValue = diskOpusFlipB;
  } else if( periph_is_active( PERIPH_TYPE_DISCIPLE ) ) {
    newValue = diskDiscipleFlipB;
  } else if( periph_is_active( PERIPH_TYPE_DIDAKTIK80 ) ) {
    newValue = diskDidaktikFlipB;
  } else {
    newValue = diskPlusDFlipB;
  }

  [diskFlipB setState:!newValue];
}

- (void)ui_menu_activate_media_disk_a_wp_set
{
  bool newValue = NO;

  if( !machine_current ) {
  } else if( machine_current->capabilities &
               LIBSPECTRUM_MACHINE_CAPABILITY_PLUS3_DISK ) {
    newValue = diskPlus3WpA;
  } else if( is_beta_active() ) {
    newValue = diskTrDosWpA;
  } else if( periph_is_active( PERIPH_TYPE_OPUS ) ) {
    newValue = diskOpusWpA;
  } else if( periph_is_active( PERIPH_TYPE_DISCIPLE ) ) {
    newValue = diskDiscipleWpA;
  } else if( periph_is_active( PERIPH_TYPE_DIDAKTIK80 ) ) {
    newValue = diskDidaktikWpA;
  } else {
    newValue = diskPlusDWpA;
  }

  [diskWpA setState:newValue];
}

- (void)ui_menu_activate_media_disk_b_wp_set
{
  bool newValue = NO;

  if( !machine_current ) {
  } else if( machine_current->capabilities &
               LIBSPECTRUM_MACHINE_CAPABILITY_PLUS3_DISK ) {
    newValue = diskPlus3WpB;
  } else if( is_beta_active() ) {
    newValue = diskTrDosWpB;
  } else if( periph_is_active( PERIPH_TYPE_OPUS ) ) {
    newValue = diskOpusWpB;
  } else if( periph_is_active( PERIPH_TYPE_DISCIPLE ) ) {
    newValue = diskDiscipleWpB;
  } else if( periph_is_active( PERIPH_TYPE_DIDAKTIK80 ) ) {
    newValue = diskDidaktikWpB;
  } else {
    newValue = diskPlusDWpB;
  }

  [diskWpB setState:newValue];
}

- (void)ui_menu_activate_media_disk_plus3_a_flip:(NSNumber*)active
{
  diskPlus3FlipA = [active boolValue];

  [self ui_menu_activate_media_disk_a_flip];
}

- (void)ui_menu_activate_media_disk_plus3_a_wp_set:(NSNumber*)active
{
  diskPlus3WpA = [active boolValue];

  [self ui_menu_activate_media_disk_a_wp_set];
}

- (void)ui_menu_activate_media_disk_plus3_b:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_disk_plus3_b_eject:(NSNumber*)active
{
  diskPlus3EjectB = [active boolValue];
}

- (void)ui_menu_activate_media_disk_plus3_b_flip:(NSNumber*)active
{
  diskPlus3FlipB = [active boolValue];

  [self ui_menu_activate_media_disk_b_flip];
}

- (void)ui_menu_activate_media_disk_plus3_b_wp_set:(NSNumber*)active
{
  diskPlus3WpB = [active boolValue];

  [self ui_menu_activate_media_disk_b_wp_set];
}

- (void)ui_menu_activate_media_disk_beta:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_disk_beta_a:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_disk_beta_a_eject:(NSNumber*)active
{
  diskTrdosEjectA = [active boolValue];
}

- (void)ui_menu_activate_media_disk_beta_a_flip:(NSNumber*)active
{
  diskTrDosFlipA = [active boolValue]; 
  [self ui_menu_activate_media_disk_a_flip];
}

- (void)ui_menu_activate_media_disk_beta_a_wp_set:(NSNumber*)active
{
  diskTrDosWpA = [active boolValue];

  [self ui_menu_activate_media_disk_a_wp_set];
}

- (void)ui_menu_activate_media_disk_beta_b:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_disk_beta_b_eject:(NSNumber*)active
{
  diskTrdosEjectB = [active boolValue];
}

- (void)ui_menu_activate_media_disk_beta_b_flip:(NSNumber*)active
{
  diskTrDosFlipB = [active boolValue];

  [self ui_menu_activate_media_disk_b_flip];
}

- (void)ui_menu_activate_media_disk_beta_b_wp_set:(NSNumber*)active
{
  diskTrDosWpB = [active boolValue];

  [self ui_menu_activate_media_disk_b_wp_set];
}

- (void)ui_menu_activate_media_disk_opus:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_disk_opus_a:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_disk_opus_a_eject:(NSNumber*)active
{
  diskOpusEjectA = [active boolValue];
}

- (void)ui_menu_activate_media_disk_opus_a_flip:(NSNumber*)active
{
  diskOpusFlipA = [active boolValue];

  [self ui_menu_activate_media_disk_a_flip];
}

- (void)ui_menu_activate_media_opus_a_wp_set:(NSNumber*)active
{
  diskOpusWpA = [active boolValue];

  [self ui_menu_activate_media_disk_a_wp_set];
}

- (void)ui_menu_activate_media_disk_opus_b:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_disk_opus_b_eject:(NSNumber*)active
{
  diskOpusEjectB = [active boolValue];
}

- (void)ui_menu_activate_media_disk_opus_b_flip:(NSNumber*)active
{
  diskOpusFlipB = [active boolValue];

  [self ui_menu_activate_media_disk_b_flip];
}

- (void)ui_menu_activate_media_opus_b_wp_set:(NSNumber*)active
{
  diskOpusWpB = [active boolValue];

  [self ui_menu_activate_media_disk_b_wp_set];
}

- (void)ui_menu_activate_media_disk_plusd:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_disk_plusd_a:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_disk_plusd_a_eject:(NSNumber*)active
{
  diskPlusDEjectA = [active boolValue];
}

- (void)ui_menu_activate_media_disk_plusd_a_flip:(NSNumber*)active
{
  diskPlusDFlipA = [active boolValue];

  [self ui_menu_activate_media_disk_a_flip];
}

- (void)ui_menu_activate_media_plusd_a_wp_set:(NSNumber*)active
{
  diskPlusDWpA = [active boolValue];

  [self ui_menu_activate_media_disk_a_wp_set];
}

- (void)ui_menu_activate_media_disk_plusd_b:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_disk_plusd_b_eject:(NSNumber*)active
{
  diskPlusDEjectB = [active boolValue];
}

- (void)ui_menu_activate_media_disk_plusd_b_flip:(NSNumber*)active
{
  diskPlusDFlipB = [active boolValue];

  [self ui_menu_activate_media_disk_b_flip];
}

- (void)ui_menu_activate_media_plusd_b_wp_set:(NSNumber*)active
{
  diskPlusDWpB = [active boolValue];

  [self ui_menu_activate_media_disk_b_wp_set];
}

- (void)ui_menu_activate_media_disk_disciple:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_disk_disciple_a:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_disk_disciple_a_eject:(NSNumber*)active
{
  diskDiscipleEjectA = [active boolValue];
}

- (void)ui_menu_activate_media_disk_disciple_a_flip:(NSNumber*)active
{
  diskDiscipleFlipA = [active boolValue];

  [self ui_menu_activate_media_disk_a_flip];
}

- (void)ui_menu_activate_media_disciple_a_wp_set:(NSNumber*)active
{
  diskDiscipleWpA = [active boolValue];

  [self ui_menu_activate_media_disk_a_wp_set];
}

- (void)ui_menu_activate_media_disk_disciple_b:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_disk_disciple_b_eject:(NSNumber*)active
{
  diskDiscipleEjectB = [active boolValue];
}

- (void)ui_menu_activate_media_disk_disciple_b_flip:(NSNumber*)active
{
  diskDiscipleFlipB = [active boolValue];

  [self ui_menu_activate_media_disk_b_flip];
}

- (void)ui_menu_activate_media_disciple_b_wp_set:(NSNumber*)active
{
  diskDiscipleWpB = [active boolValue];

  [self ui_menu_activate_media_disk_b_wp_set];
}

- (void)ui_menu_activate_media_disk_didaktik:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_disk_didaktik_a:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_disk_didaktik_a_eject:(NSNumber*)active
{
  diskDidaktikEjectA = [active boolValue];
}

- (void)ui_menu_activate_media_disk_didaktik_a_flip:(NSNumber*)active
{
  diskDidaktikFlipA = [active boolValue];
  
  [self ui_menu_activate_media_disk_a_flip];
}

- (void)ui_menu_activate_media_didaktik_a_wp_set:(NSNumber*)active
{
  diskDidaktikWpA = [active boolValue];
  
  [self ui_menu_activate_media_disk_a_wp_set];
}

- (void)ui_menu_activate_media_disk_didaktik_b:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_disk_didaktik_b_eject:(NSNumber*)active
{
  diskDidaktikEjectB = [active boolValue];
}

- (void)ui_menu_activate_media_disk_didaktik_b_flip:(NSNumber*)active
{
  diskDidaktikFlipB = [active boolValue];
  
  [self ui_menu_activate_media_disk_b_flip];
}

- (void)ui_menu_activate_media_didaktik_b_wp_set:(NSNumber*)active
{
  diskDidaktikWpB = [active boolValue];
  
  [self ui_menu_activate_media_disk_b_wp_set];
}

- (void)ui_menu_activate_media_ide:(NSNumber*)active
{
  [ideMaster setEnabled:[active boolValue]];
  [ideSlave setEnabled:NO];
  if( settings_current.simpleide_active || settings_current.zxatasp_active ||
      settings_current.divide_enabled )
    [ideSlave setEnabled:[active boolValue]];
}

- (void)ui_menu_activate_media_ide_divide:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_ide_divide_master_eject:(NSNumber*)active
{
  ideDivideEjectMaster = [active boolValue];
}

- (void)ui_menu_activate_media_ide_divide_slave_eject:(NSNumber*)active
{
  ideDivideEjectSlave = [active boolValue];
}

- (void)ui_menu_activate_media_ide_simple8bit:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_ide_simple8bit_master_eject:(NSNumber*)active
{
  ideSimple8BitEjectMaster = [active boolValue];
}

- (void)ui_menu_activate_media_ide_simple8bit_slave_eject:(NSNumber*)active
{
  ideSimple8BitEjectSlave = [active boolValue];
}

- (void)ui_menu_activate_media_ide_zxatasp:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_ide_zxatasp_master_eject:(NSNumber*)active
{
  ideZxataspEjectMaster = [active boolValue];
}

- (void)ui_menu_activate_media_ide_zxatasp_slave_eject:(NSNumber*)active
{
  ideZxataspEjectSlave = [active boolValue];
}

- (void)ui_menu_activate_media_ide_zxcf:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_ide_zxcf_eject:(NSNumber*)active
{
  ideZxcfEject = [active boolValue];
}

- (void)ui_menu_activate_media_ide_divmmc:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_ide_divmmc_eject:(NSNumber*)active
{
  ideDivmmcEject = [active boolValue];
}

- (void)ui_menu_activate_media_ide_zxmmc:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_ide_zxmmc_eject:(NSNumber*)active
{
  ideZxmmcEject = [active boolValue];
}

- (void)ui_menu_activate_media_if1:(NSNumber*)active
{
  [mdr1 setEnabled:[active boolValue]];
  [mdr2 setEnabled:[active boolValue]];
}

- (void)ui_menu_activate_media_if1_m1_eject:(NSNumber*)active
{
  if1M1Eject = [active boolValue];
}

- (void)ui_menu_activate_media_if1_m1_wp_set:(NSNumber*)active
{
  [if1Wp1 setState:[active boolValue] == YES ? NSOffState : NSOnState];
}

- (void)ui_menu_activate_media_if1_m2_eject:(NSNumber*)active
{
  if1M2Eject = [active boolValue];
}

- (void)ui_menu_activate_media_if1_m2_wp_set:(NSNumber*)active
{
  [if1Wp2 setState:[active boolValue] == YES ? NSOffState : NSOnState];
}

- (void)ui_menu_activate_media_if1_m3_eject:(NSNumber*)active
{
  if1M3Eject = [active boolValue];
}

- (void)ui_menu_activate_media_if1_m3_wp_set:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_if1_m4_eject:(NSNumber*)active
{
  if1M4Eject = [active boolValue];
}

- (void)ui_menu_activate_media_if1_m4_wp_set:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_if1_m5_eject:(NSNumber*)active
{
  if1M5Eject = [active boolValue];
}

- (void)ui_menu_activate_media_if1_m5_wp_set:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_if1_m6_eject:(NSNumber*)active
{
  if1M6Eject = [active boolValue];
}

- (void)ui_menu_activate_media_if1_m6_wp_set:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_if1_m7_eject:(NSNumber*)active
{
  if1M7Eject = [active boolValue];
}

- (void)ui_menu_activate_media_if1_m7_wp_set:(NSNumber*)active
{
}

- (void)ui_menu_activate_media_if1_m8_eject:(NSNumber*)active
{
  if1M8Eject = [active boolValue];
}

- (void)ui_menu_activate_media_if1_m8_wp_set:(NSNumber*)active
{
}

- (void)ui_menu_activate_recording:(NSNumber*)active
{
  record = recordFromSnapshot = play = ![active boolValue];
  stop = [active boolValue];
}

- (void)ui_menu_activate_movie_recording:(NSNumber*)active
{
  movieRecord = movieRecordFromSnapshot = ![active boolValue];
  movieStop = [active boolValue];
}

- (void)ui_menu_activate_movie_pause:(NSNumber*)active
{
  moviePause = [active boolValue];
}
                   
- (void)ui_menu_activate_recording_rollback:(NSNumber*)active
{
  insert_snap = rollback = rollback_to = [active boolValue];
}

- (void)ui_menu_activate_ay_logging:(NSNumber*)active
{
  recordPsg = ![active boolValue];
  stopPsg = [active boolValue];
}

- (void)ui_menu_activate_machine_profiler:(NSNumber*)active
{
  profileStart = ![active boolValue];
  profileStop = [active boolValue];
}

- (void)ui_menu_activate_tape_record:(NSNumber*)active
{
  playTape = ![active boolValue];
  [tapeRecord setTitle:[active boolValue] == NO ? @"Record" :
                                                  @"Stop Recording"];
}

- (void)ui_menu_activate_didaktik80_snap:(NSNumber*)active
{
  didaktik80Snap = [active boolValue];
}

- (void)ui_menu_activate_multiface:(NSNumber*)active
{
  multiface = [active boolValue];
}

- (BOOL)validateMenuItem:(NSMenuItem*)menuItem
{
  switch( [menuItem tag] ) {
  case 1:
    return recordPsg == 0 ? NO : YES;
    break;
  case 2:
    return dockEject == 0 ? NO : YES;
    break;
  case 3:
    return stopPsg == 0 ? NO : YES;
    break;
  case 6:
    return record == 0 ? NO : YES;
    break;
  case 7:
    return recordFromSnapshot == 0 ? NO : YES;
    break;
  case 8:
    return play == 0 ? NO : YES;
    break;
  case 9:
    return stop == 0 ? NO : YES;
    break;
  case 12:
    return if2Eject == 0 ? NO : YES;
    break;
  case 17:
    return insert_snap == 0 ? NO : YES;
    break;
  case 18:
    return rollback == 0 ? NO : YES;
    break;
  case 19:
    return rollback_to == 0 ? NO : YES;
    break;
  case 51:
  case 61:
    return ideSimple8BitEjectMaster || ideZxataspEjectMaster || ideZxcfEject ||
      ideDivideEjectMaster || ideZxmmcEject || ideDivmmcEject ? YES : NO;
    break;
  case 52:
  case 62:
    return ideSimple8BitEjectSlave || ideZxataspEjectSlave ||
      ideDivideEjectSlave ? YES : NO;
    break;
  case 70:
    return profileStart == 0 ? NO : YES;
    break;
  case 71:
    return profileStop == 0 ? NO : YES;
    break;
  case 72:
  case 73:
  case 74:
  case 75:
  case 76:
    return playTape == 0 ? NO : YES;
    break;
  case 32:
  case 33:
  case 34:
  case 35:
    return if1M1Eject == 0 ? NO : YES;
    break;
  case 82:
  case 83:
  case 84:
  case 85:
    return if1M2Eject == 0 ? NO : YES;
    break;
  case 92:
  case 93:
  case 94:
    return if1M3Eject == 0 ? NO : YES;
    break;
  case 102:
  case 103:
  case 104:
    return if1M4Eject == 0 ? NO : YES;
    break;
  case 112:
  case 113:
  case 114:
    return if1M5Eject == 0 ? NO : YES;
    break;
  case 122:
  case 123:
  case 124:
    return if1M6Eject == 0 ? NO : YES;
    break;
  case 132:
  case 133:
  case 134:
    return if1M7Eject == 0 ? NO : YES;
    break;
  case 142:
  case 143:
  case 144:
    return if1M8Eject == 0 ? NO : YES;
    break;
  case 150:
  case 151:
  case 152:
  case 153:
  case 154:
    return diskPlus3EjectA || diskTrdosEjectA || diskOpusEjectA ||
      diskPlusDEjectA || diskDiscipleEjectA || diskDidaktikEjectA ? YES : NO;
    break;
  case 160:
  case 161:
  case 162:
  case 163:
  case 164:
    return diskPlus3EjectB || diskTrdosEjectB || diskOpusEjectB ||
      diskPlusDEjectB || diskDiscipleEjectB || diskDidaktikEjectB ? YES : NO;
    break;
  case 170:
    return movieRecord == 0 ? NO : YES;
    break;
  case 171:
    return movieRecordFromSnapshot == 0 ? NO : YES;
    break;
  case 172:
    return moviePause == 0 ? NO : YES;
    break;
  case 173:
    return movieStop == 0 ? NO : YES;
    break;
  case 174:
    return didaktik80Snap == 0 ? NO : YES;
    break;
  case 175:
    return multiface == 0 ? NO : YES;
    break;
  case 176:
    if( !debuggerEnabled ) return NO;
    /* Disable Machine→Debugger when a utility window is holding the
       emulator paused and the Debugger itself is not already up. The
       menu cannot do anything useful in that state: cocoa_break: sets
       HALTED, but z80 is not executing because the utility window's
       pause stopped the timer, so no trap fires and the Debugger
       window never appears until the user closes the utility. The
       comparison must subtract the user-pause contribution so the
       guard fires regardless of whether Machine→Pause is also set —
       the utility-window holds are the part that breaks the trap
       path. */
    if( fuse_emulation_paused > (paused ? 1 : 0) ) {
      DebuggerController *dbg = [DebuggerController loadedSingleton];
      if( !dbg || ![[dbg window] isVisible] ) return NO;
    }
    return YES;
    break;
  default:
    return YES;
  }
}

- (void)openFile:(const char *)filename
{
  char *snapshot;
  __block utils_file file = {}; libspectrum_id_t type;
  libspectrum_class_t lsclass;
  libspectrum_error libspec_error;
  libspectrum_snap* snap;

  if( !filename ) return;
  
  AppSandboxFileAccess *fileAccess = [AppSandboxFileAccess fileAccess];
  NSString *fName = [NSString stringWithCString:filename
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1040
    encoding:NSWindowsCP1252StringEncoding
#endif
  ];
  [fileAccess persistPermissionPath:fName];
  
  BOOL accessAllowed = [fileAccess accessFilePath:fName persistPermission:YES withBlock: ^{
      (void)file;
      if( utils_read_file( filename, &file ) )
      {
        return;
      }
  }];

  if (!accessAllowed)
  {
    ui_error(UI_ERROR_ERROR, "Cannot obtain access to file %s", filename);
    return;
  }

  if( libspectrum_identify_file( &type, filename, file.buffer, file.length ) ) {
    utils_close_file( &file );
    fuse_abort();
  }

  if( libspectrum_identify_class( &lsclass, type ) ) fuse_abort();

  if( lsclass != LIBSPECTRUM_CLASS_RECORDING ) {
    utils_close_file( &file );
    [[DisplayOpenGLView instance] openFile:filename];
    return;
  }

  if( rzx_playback || rzx_recording ) return;

  rzx = libspectrum_rzx_alloc();

  libspec_error = libspectrum_rzx_read( rzx, file.buffer, file.length );
  utils_close_file( &file );
  if( libspec_error != LIBSPECTRUM_ERROR_NONE ) return;

  snap = rzx_get_initial_snapshot();
  if( !snap ) {
    /* We need to load an external snapshot. */
    snapshot = cocoaui_openpanel_get_filename( @"Load Replay Snapshot",
                                               snapFileTypes );
    if( !snapshot ) return;

    [[DisplayOpenGLView instance] snapOpen:snapshot];

    free( snapshot );
  }

  [self addRecentSnapshot:filename];

  libspectrum_rzx_free( rzx );

  [[DisplayOpenGLView instance] rzxStartPlayback:filename];

  if( rzx_playback ) ui_menu_activate( UI_MENU_ITEM_RECORDING, 1 );
}

- (void)openRecent:(id)fileMenu
{
  NSString *file = [settings_current.cocoa->recent_snapshots
                      objectAtIndex:[recentSnaps indexOfItem:fileMenu]];
  char *filename = strdup([file UTF8String]);

  if( filename == NULL ) return;
  
  [self addRecentSnapshot:filename];

  [[DisplayOpenGLView instance] pause];

  [self openFile:filename];

  [[DisplayOpenGLView instance] unpause];
}

- (void)generateUniqueLabels:(NSString *)filename
{
  NSEnumerator *enumerator = [recentSnapFileNames objectEnumerator];
  id object;
  /* Will contain the set of matches with the key being the index */
  NSMutableArray *matches = [NSMutableArray arrayWithCapacity:2];

  while( (object = [enumerator nextObject]) ) {
    if( [object isEqualToString:filename] ) {
        NSNumber *index = @([recentSnapFileNames indexOfObjectIdenticalTo:object]);
        NSString *file = [NSString stringWithString:filename];
        NSArray *match = @[index, file];
        [matches addObject:match];
    }
  }

  NSMutableString *commonPrefix = [NSMutableString
    stringWithString:[settings_current.cocoa->recent_snapshots
            objectAtIndex:[[[matches lastObject] objectAtIndex:1] intValue]]];

  /* Iterate through matches to find shortest common string as found by
     commonPrefixWithString */
  enumerator = [matches objectEnumerator];
  while( (object = [enumerator nextObject]) ) {
    /* Compare all strings with commonPrefixWithString and find
       the shortest prefix */
    id object2;
    NSString *file = [settings_current.cocoa->recent_snapshots
                          objectAtIndex:[[object objectAtIndex:1] intValue]];
    NSEnumerator *enumerator2 = [matches objectEnumerator];
    while( (object2 = [enumerator2 nextObject]) ) {
      if([object isEqual:object2]) continue;

      NSString *file2 = [settings_current.cocoa->recent_snapshots
                        objectAtIndex:[[object2 objectAtIndex:0] intValue]];
      NSString *prefix = [file commonPrefixWithString:file2 options:NSLiteralSearch];

      if( [prefix length] < [commonPrefix length] ) {
        [commonPrefix setString:prefix];
      }
    }
  }

  NSRange range = NSMakeRange(0, [commonPrefix length]);
  /* Iterate through matches, setting the title in recentSnaps with
     the full path with commonPrefix removed, and with snap name - .../
     prepended */
  for( object in matches ) {
    unsigned index = [[object objectAtIndex:0] intValue];
    NSString *file = [settings_current.cocoa->recent_snapshots
                        objectAtIndex:[[object objectAtIndex:0] intValue]];
    NSMutableString *label = [NSMutableString stringWithString:file];

    [label replaceCharactersInRange:range withString:@" - .../"];
    [label insertString:filename atIndex:0];

    [[recentSnaps itemAtIndex:index] setTitle:label];
  }
}

- (void)addRecentSnapshotWithString:(NSString*)file
{
  NSMenuItem *menuItem;
  NSFileManager *manager = [NSFileManager defaultManager];

  /* We only work with absolute paths */
  if([file characterAtIndex:0] != '/') return;

  if(![manager fileExistsAtPath:file]) return;

  /* If the snapname is already in the list, remove it to add it at the start */
  if([settings_current.cocoa->recent_snapshots containsObject:file] == YES) {
    unsigned index = [settings_current.cocoa->recent_snapshots
                        indexOfObject:file];
    [settings_current.cocoa->recent_snapshots removeObjectAtIndex:index];
    [recentSnaps removeItemAtIndex:index];
    [recentSnapFileNames removeObjectAtIndex:index];
  }

  NSArray *pathComponents = [file pathComponents];
  NSString *displayFile = [NSString stringWithString:[pathComponents lastObject]];

  BOOL foundFile = NO;

  if([recentSnapFileNames containsObject:displayFile] == YES) {
    foundFile = YES;
  }

  menuItem = [[NSMenuItem allocWithZone:[NSMenu menuZone]]
               initWithTitle:displayFile action:@selector(openRecent:)
               keyEquivalent:@""];
  [menuItem setTarget:self];
  [recentSnaps insertItem:menuItem atIndex:0];
  
  if([settings_current.cocoa->recent_snapshots count] == NUM_RECENT_ITEMS) {
    [settings_current.cocoa->recent_snapshots removeLastObject];
    [recentSnaps removeItemAtIndex:[recentSnaps numberOfItems]-1];
    [recentSnapFileNames removeLastObject];
  }
  
  [settings_current.cocoa->recent_snapshots insertObject:file atIndex:0];
  [recentSnapFileNames insertObject:displayFile atIndex:0];

  if(foundFile == YES) {
    [self generateUniqueLabels:displayFile];
  }
}

- (void)addRecentSnapshot:(const char *)filename
{
  [self addRecentSnapshotWithString:@(filename)];
}

- (void)clearRecentSnapshots
{
  int numMenuItems = [recentSnaps numberOfItems];
  int i;
  for ( i=0; i<numMenuItems; i++ ) {
    [recentSnaps removeItemAtIndex:0];
  }
  [settings_current.cocoa->recent_snapshots removeAllObjects];
  [recentSnapFileNames removeAllObjects];
}

- (void)setTitle:(NSString*)title
{
  [window setTitle:title];
}

- (void)newDisk:(int)drive
{
  [[DisplayOpenGLView instance] pause];
  [[DisplayOpenGLView instance] diskInsertNew:drive];
  [[DisplayOpenGLView instance] unpause];
}

- (void)openDisk:(int)drive
{
  char *filename = NULL;
  NSArray *fileTypes;
  NSString *message;
  ui_media_drive_info_t *drive_info = NULL;

  [[DisplayOpenGLView instance] pause];
 
  if( machine_current->capabilities &
               LIBSPECTRUM_MACHINE_CAPABILITY_PLUS3_DISK ) {
    fileTypes = plus3FileTypes;
  } else if( is_beta_active() ) {
    fileTypes = betaFileTypes;
  } else if( periph_is_active( PERIPH_TYPE_OPUS ) ) {
    fileTypes = opusFileTypes;
  } else if( periph_is_active( PERIPH_TYPE_DIDAKTIK80 ) ) {
    fileTypes = didaktikFileTypes;
  } else {
    fileTypes = plusdFileTypes;
  }

  drive_info = ui_media_drive_find( ui_media_active_disk_controller(), drive );
  if( !drive_info ) {
    [[DisplayOpenGLView instance] unpause];
    return;
  }
  message = [NSString stringWithFormat:@"Insert %s", drive_info->name];

  filename = cocoaui_openpanel_get_filename( message, fileTypes );

  if( !filename ) { [[DisplayOpenGLView instance] unpause]; return; }

  [self addRecentSnapshot:filename];

  [[DisplayOpenGLView instance] diskInsert:filename inDrive:drive];

  free( filename );

  [[DisplayOpenGLView instance] unpause];
}

- (void)setDiskState:(NSNumber*)state
{
}

- (void)setTapeState:(NSNumber*)state
{
  [tapePlay setTitle:[state unsignedCharValue] == UI_STATUSBAR_STATE_ACTIVE ? @"Pause" : @"Play"];
}

- (void)setMdrState:(NSNumber*)state
{
}

- (void)setPauseState
{
  [pause setTitle:paused ? @"Resume" : @"Pause"];
}

- (void)showAlertPanel:(NSString*)message
{
  NSRunAlertPanel(@"Fuse - Info", message, nil, nil, nil);
}

- (void)showCriticalAlertPanel:(NSString*)message
{
  NSRunCriticalAlertPanel(@"Fuse - Error", message, nil, nil, nil);
}

-(ui_confirm_save_t) confirmSave:(NSString*)theMessage
{
  int result;

  ui_confirm_save_t confirm;

  if( !settings_current.confirm_actions ) return UI_CONFIRM_SAVE_DONTSAVE;

  [[DisplayOpenGLView instance] pause];

  if( ui_mouse_grabbed ) ui_mouse_grabbed = ui_mouse_release( 1 );

  result = NSRunAlertPanel(@"Confirm", theMessage, @"Save",
                           @"Don't Save", @"Cancel");

  switch( result ) {
  case NSAlertDefaultReturn:
    confirm = UI_CONFIRM_SAVE_SAVE;
    break;
  case NSAlertAlternateReturn:
    confirm = UI_CONFIRM_SAVE_DONTSAVE;
    break;
  case NSAlertOtherReturn:
  default:
    confirm = UI_CONFIRM_SAVE_CANCEL;
  }

  [[DisplayOpenGLView instance] unpause];

  return confirm;
}

-(int) confirm:(NSString*)theMessage
{
  return cocoaui_confirm( [theMessage UTF8String] );
}

-(int) tapeWrite
{
  char *filename = NULL;

  [[DisplayOpenGLView instance] pause];

  filename = cocoaui_savepanel_get_filename( @"Write Tape As", @[@"tzx", @"tap", @"csw"] );

  if( !filename ) { [[DisplayOpenGLView instance] unpause]; return 1; }

  /* We will be calling this from the main thread while the emulator is
     paused */
  tape_write( filename );

  [self addRecentSnapshotWithString:@(filename)];

  free( filename );

  [[DisplayOpenGLView instance] unpause];

  return 0;
}

-(int) diskWrite:(int)which saveAs:(bool)saveas
{
  char *filename = NULL;
  NSArray *fileTypes;
  int err;
  ui_media_drive_info_t *drive_info = NULL;
  
  [[DisplayOpenGLView instance] pause];
  
  if( machine_current->capabilities &
               LIBSPECTRUM_MACHINE_CAPABILITY_PLUS3_DISK ) {
    fileTypes = @[@"dsk"];
  } else if( is_beta_active() ) {
    fileTypes = @[@"trd", @"scl", @"udi", @"fdi"];
  } else if( periph_is_active( PERIPH_TYPE_DIDAKTIK80 ) ) {
    fileTypes = @[@"d40", @"d80"];
  } else if( periph_is_active( PERIPH_TYPE_OPUS ) ) {
    fileTypes = @[@"opd", @"opu", @"dsk"];
  } else {
    fileTypes = @[@"mgt", @"img"];
  }
  
  drive_info = ui_media_drive_find( ui_media_active_disk_controller(), which );
  if( !drive_info )  { [[DisplayOpenGLView instance] unpause]; return 1; }

  if( drive_info->fdd->disk.filename == NULL )
    saveas = YES;
  
  if( saveas == YES ) {
    NSString *title = [NSString stringWithFormat:@"Write %s As", drive_info->name];
    filename = cocoaui_savepanel_get_filename( title, fileTypes );
    
    if( !filename ) { [[DisplayOpenGLView instance] unpause]; return 1; }
  }
  
  /* We will be calling this from the Emulator thread */
  err = ui_media_drive_save_with_filename( drive_info, filename );
  
  /* If we had a filename already it will only have been here - use the filename
     we have used to write the file */
  filename = drive_info->fdd->disk.filename;
  
  [self addRecentSnapshotWithString:@(filename)];
  
  if( saveas == YES ) free( filename );
  
  [[DisplayOpenGLView instance] unpause];
  
  return err;
}

-(int) if1MdrWrite:(int)which saveAs:(bool)saveas
{
  int err;
  char *filename = NULL;

  [[DisplayOpenGLView instance] pause];

  if( saveas == YES ) {
    NSString *title = [NSString stringWithFormat:@"Write Microdrive Cartridge %i As", which];
    filename = cocoaui_savepanel_get_filename( title, @[@"mdr"] );

    if( !filename ) { [[DisplayOpenGLView instance] unpause]; return 1; }
  }

  /* We will be calling this from the main thread with emulator paused */
  err = if1_mdr_write( which, filename );

  [self addRecentSnapshotWithString:@(filename)];

  if( saveas == YES ) free( filename );

  [[DisplayOpenGLView instance] unpause];

  return err;
}

-(ui_confirm_joystick_t) confirmJoystick:(libspectrum_joystick)type inputs:(int)theInputs
{
  char* joystick_name;
  char input_names[256] = "";
  NSString *alertString;
  NSAlert *alert;

  if( !settings_current.joy_prompt ) return UI_CONFIRM_JOYSTICK_NONE;
  
  alert = [[NSAlert alloc] init];
  [alert addButtonWithTitle:@"None"];
  [alert addButtonWithTitle:@"Joystick 2"];
  [alert addButtonWithTitle:@"Joystick 1"];
  [alert addButtonWithTitle:@"Keyboard"];
	
  [alert setMessageText:@"Configure joystick"];
  joystick_name = strdup( libspectrum_joystick_name( type ) );
  joystick_name[0] = tolower( joystick_name[0] );
  if( theInputs ) {
    int num_inputs = 0;
    int i = 0;
    const char* conn_names[3];

    for( i = 0; theInputs>>i; i++ ) {
      if( ( theInputs>> i ) & 0x01 ) {
        conn_names[num_inputs++] = connection_names[i];
      }
    }

    if( num_inputs >= 1 ) {
      sprintf(input_names, " connected to %s", conn_names[0]);
    }
    if( num_inputs >= 3 ) {
      sprintf(input_names, "%s, %s", input_names, conn_names[2]);
    }
    if( num_inputs >= 2 ) {
      sprintf(input_names, "%s and %s", input_names, conn_names[1]);
    }
  }
  alertString = [NSString stringWithFormat:@"The snapshot you are loading is configured for a %s joystick%s. Fuse is not currently configured for a %s joystick, would you like to connect the joystick to:", joystick_name, input_names, joystick_name ];
  free( joystick_name );

  [alert setInformativeText:alertString];
  [alert setAlertStyle:NSWarningAlertStyle];

  ui_confirm_joystick_t confirm;

  if( ui_mouse_grabbed ) ui_mouse_grabbed = ui_mouse_release( 1 );

  [[DisplayOpenGLView instance] pause];

  confirm = UI_CONFIRM_JOYSTICK_NONE;

  switch( [alert runModal] ) {
  case NSAlertFirstButtonReturn:
    confirm = UI_CONFIRM_JOYSTICK_NONE;
    break;
  case NSAlertSecondButtonReturn:
    confirm = UI_CONFIRM_JOYSTICK_JOYSTICK_2;
    break;
  case NSAlertThirdButtonReturn:
    confirm = UI_CONFIRM_JOYSTICK_JOYSTICK_1;
    break;
  case NSAlertThirdButtonReturn+1:
    confirm = UI_CONFIRM_JOYSTICK_KEYBOARD;
    break;
  } 

  [alert release];

  [[DisplayOpenGLView instance] unpause];

  return confirm;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
  /* The user-facing prompts live here, not in -windowShouldClose:, so Cmd+Q
     (-quit: → [NSApp terminate:]) and red-button / Cmd+W (windowShouldClose:
     → [NSApp terminate:]) share one path. AppKit calls close (not
     performClose:) on each window during termination, so windowShouldClose:
     would be bypassed on the Cmd+Q path otherwise. */
  if( !cocoaui_confirm( "Exit Fuse?" ) ) return NSTerminateCancel;
  if( [[DisplayOpenGLView instance] checkMediaChanged] ) return NSTerminateCancel;
  [[DisplayOpenGLView instance] displayLinkStop];
  return NSTerminateNow;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)theApplication
{
  return YES;
}

- (BOOL)application:(NSApplication *)theApplication openFile:(NSString *)filename
{
  utils_file file; libspectrum_id_t type;
  libspectrum_class_t lsclass;
  char fsrep[PATH_MAX+1];

  [filename getFileSystemRepresentation:fsrep maxLength:PATH_MAX];

  if ( display_ui_initialised ) {
    [[DisplayOpenGLView instance] pause];
    [self addRecentSnapshot:fsrep];
    [self openFile:fsrep];
    [[DisplayOpenGLView instance] unpause];
  } else {
    /* AppKit routes stray command-line arguments here on cold launch; an
       argument that is not a readable, identifiable file must be declined. */
    if( utils_read_file( fsrep, &file ) ) {
      fprintf( stderr, "%s: couldn't open `%s'\n", fuse_progname, fsrep );
      return NO;
    }

    if( libspectrum_identify_file( &type, fsrep, file.buffer, file.length ) ||
        libspectrum_identify_class( &lsclass, type ) ) {
      fprintf( stderr, "%s: couldn't identify `%s'\n", fuse_progname, fsrep );
      utils_close_file( &file );
      return NO;
    }

    switch( lsclass ) {

    case LIBSPECTRUM_CLASS_UNKNOWN:
      fprintf( stderr, "%s: couldn't identify `%s'\n", fuse_progname, fsrep );
    case LIBSPECTRUM_CLASS_SCREENSHOT:
      utils_close_file( &file );
      return NO;

    case LIBSPECTRUM_CLASS_RECORDING:
      settings_current.playback_file = strdup( fsrep );
      break;

    case LIBSPECTRUM_CLASS_SNAPSHOT:
      settings_current.snapshot = strdup( fsrep );
      break;

    case LIBSPECTRUM_CLASS_TAPE:
      settings_current.tape_file = strdup( fsrep );
      break;

    case LIBSPECTRUM_CLASS_DISK_PLUS3:
      settings_current.plus3disk_file = strdup( fsrep );
      break;

    case LIBSPECTRUM_CLASS_DISK_TRDOS:
    case LIBSPECTRUM_CLASS_DISK_GENERIC:
      settings_current.betadisk_file = strdup( fsrep );
      break;

    case LIBSPECTRUM_CLASS_DISK_OPUS:
      settings_current.opusdisk_file = strdup( fsrep );
      break;

    case LIBSPECTRUM_CLASS_DISK_PLUSD:
      settings_current.plusddisk_file = strdup( fsrep );
      break;

    case LIBSPECTRUM_CLASS_CARTRIDGE_TIMEX:
      settings_current.dck_file = strdup( fsrep );
      break;

    case LIBSPECTRUM_CLASS_CARTRIDGE_IF2:
      settings_current.if2_file = strdup( fsrep );
      break;

    case LIBSPECTRUM_CLASS_HARDDISK:
      if( settings_current.zxcf_active ) {
        settings_current.zxcf_pri_file = strdup( fsrep );
      } else if( settings_current.zxatasp_active ) {
        settings_current.zxatasp_master_file = strdup( fsrep );
      } else if( settings_current.simpleide_active ) {
        settings_current.simpleide_master_file = strdup( fsrep );
      } else if( settings_current.divmmc_enabled ) {
        settings_current.divmmc_file = strdup( fsrep );
      } else if( settings_current.divide_enabled ) {
        settings_current.divide_master_file = strdup( fsrep );
      } else if( settings_current.zxmmc_enabled ) {
        settings_current.zxmmc_file = strdup( fsrep );
      } else {
        /* No IDE interface active, so activate the ZXCF */
        settings_current.zxcf_active = 1;
        settings_current.zxcf_pri_file = strdup( fsrep );
      }
      break;

    default:
      fprintf( stderr, "%s: loadFile: unknown class %d!\n",
               fuse_progname, type );
    }

    utils_close_file( &file );
  }

  return YES;
}

- (void)setAcceptsMouseMovedEvents:(BOOL)flag
{
  [window setAcceptsMouseMovedEvents:flag];
}

@end

static char*
cocoaui_openpanel_get_filename( NSString *title, NSArray *fileTypes )
{
  char buffer[PATH_MAX+1];
  char *filename = NULL;
  int result;
  NSOpenPanel *oPanel = [NSOpenPanel openPanel];

  [oPanel setTitle:title];

  [oPanel setAllowedFileTypes:fileTypes];
  result = [oPanel runModal];
  if (result == NSOKButton) {
    NSArray *filesToOpen = [oPanel URLs];
    NSString *aFile = [[filesToOpen objectAtIndex:0] path];
    
    AppSandboxFileAccess *fileAccess = [AppSandboxFileAccess fileAccess];
    [fileAccess persistPermissionPath:aFile];
    
    [aFile getFileSystemRepresentation:buffer maxLength:PATH_MAX];
    filename = strdup ( buffer );
  }

  return filename;
}

static char*
cocoaui_savepanel_get_filename( NSString *title, NSArray *fileTypes )
{
  char buffer[PATH_MAX+1];
  char *filename = NULL;
  int result;
  NSView *accessoryView = nil;
  sPanel = [NSSavePanel savePanel];

  [sPanel setTitle:title];
  [sPanel setAllowedFileTypes:fileTypes];
  [sPanel setCanSelectHiddenExtension:YES];

  if( [fileTypes count] > 1 ) {
    FuseController *controller = [FuseController singleton];
    accessoryView = [controller savePanelAccessoryView];
    [[controller saveFileType] removeAllItems];
    [[controller saveFileType] addItemsWithTitles:fileTypes];
  }
  [sPanel setAccessoryView:accessoryView];

  result = [sPanel runModal];
  if (result == NSOKButton) {
    NSString *oFile = [[sPanel URL] path];
    [oFile getFileSystemRepresentation:buffer maxLength:PATH_MAX];
    filename = strdup ( buffer );
  }

  return filename;
}

static void
cocoaui_disk_eject( int drive )
{
  [[DisplayOpenGLView instance] pause];
  [[DisplayOpenGLView instance] diskEject:drive];
  [[DisplayOpenGLView instance] unpause];
}

static void
cocoaui_disk_save( int drive, int saveas )
{
  [[DisplayOpenGLView instance] pause];
  [[DisplayOpenGLView instance] diskSave:drive saveAs:saveas];
  [[DisplayOpenGLView instance] unpause];
}

static void
cocoaui_disk_flip( int drive, int flip )
{
  [[DisplayOpenGLView instance] pause];
  [[DisplayOpenGLView instance] diskFlip:drive side:flip];
  [[DisplayOpenGLView instance] unpause];
}

static void
cocoaui_disk_write_protect( int drive, int wrprot )
{
  [[DisplayOpenGLView instance] pause];
  [[DisplayOpenGLView instance] diskWriteProtect:drive protect:wrprot];
  [[DisplayOpenGLView instance] unpause];
}

static void
cocoaui_mdr_eject( int drive )
{
  [[DisplayOpenGLView instance] pause];
  [[DisplayOpenGLView instance] if1MdrCartEject:drive];
  [[DisplayOpenGLView instance] unpause];
}

static void 
cocoaui_mdr_save( int drive, int saveas )
{
  [[DisplayOpenGLView instance] pause];
  [[DisplayOpenGLView instance] if1MdrCartSave:drive saveAs:saveas];
  [[DisplayOpenGLView instance] unpause];
}

/* Runs in Emulator object context */
char *
ui_get_open_filename( const char *title )
{
  return cocoaui_openpanel_get_filename( @"Load Snapshot", snapFileTypes );
}

/* Function to (de)activate specific menu items */
int
ui_menu_activate( ui_menu_item item, int active )
{
  BOOL value = active ? YES : NO;
  NSNumber* activeBool = @(value);
  SEL method = nil;

  switch( item ) {
    
  case UI_MENU_ITEM_MEDIA_CARTRIDGE:
    method = @selector(ui_menu_activate_media_cartridge:);
    break;
    
  case UI_MENU_ITEM_MEDIA_CARTRIDGE_DOCK:
    method = @selector(ui_menu_activate_media_cartridge_dock:);
    break;
    
  case UI_MENU_ITEM_MEDIA_CARTRIDGE_DOCK_EJECT:
    method = @selector(ui_menu_activate_media_cartridge_dock_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_CARTRIDGE_IF2:
    method = @selector(ui_menu_activate_media_cartridge_if2:);
    break;
    
  case UI_MENU_ITEM_MEDIA_CARTRIDGE_IF2_EJECT:
    method = @selector(ui_menu_activate_media_cartridge_if2_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK:
    method = @selector(ui_menu_activate_media_disk:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_PLUS3:
    break;

  case UI_MENU_ITEM_MEDIA_DISK_PLUS3_A_EJECT:
    method = @selector(ui_menu_activate_media_disk_plus3_a_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_PLUS3_A_FLIP_SET:
    method = @selector(ui_menu_activate_media_disk_plus3_a_flip:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_PLUS3_A_WP_SET:
    method = @selector(ui_menu_activate_media_disk_plus3_a_wp_set:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_PLUS3_B:
    break;

  case UI_MENU_ITEM_MEDIA_DISK_PLUS3_B_EJECT:
    method = @selector(ui_menu_activate_media_disk_plus3_b_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_PLUS3_B_FLIP_SET:
    method = @selector(ui_menu_activate_media_disk_plus3_b_flip:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_PLUS3_B_WP_SET:
    method = @selector(ui_menu_activate_media_disk_plus3_b_wp_set:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_BETA:
    method = @selector(ui_menu_activate_media_disk_beta:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_BETA_A:
    method = @selector(ui_menu_activate_media_disk_beta_a:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_BETA_A_EJECT:
    method = @selector(ui_menu_activate_media_disk_beta_a_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_BETA_A_FLIP_SET:
    method = @selector(ui_menu_activate_media_disk_beta_a_flip:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_BETA_A_WP_SET:
    method = @selector(ui_menu_activate_media_disk_beta_a_wp_set:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_BETA_B:
    method = @selector(ui_menu_activate_media_disk_beta_b:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_BETA_B_EJECT:
    method = @selector(ui_menu_activate_media_disk_beta_b_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_BETA_B_FLIP_SET:
    method = @selector(ui_menu_activate_media_disk_beta_b_flip:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_BETA_B_WP_SET:
    method = @selector(ui_menu_activate_media_disk_beta_b_wp_set:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_BETA_C:
    break;

  case UI_MENU_ITEM_MEDIA_DISK_BETA_C_EJECT:
    break;

  case UI_MENU_ITEM_MEDIA_DISK_BETA_C_FLIP_SET:
    break;

  case UI_MENU_ITEM_MEDIA_DISK_BETA_C_WP_SET:
    break;

  case UI_MENU_ITEM_MEDIA_DISK_BETA_D:
    break;

  case UI_MENU_ITEM_MEDIA_DISK_BETA_D_EJECT:
    break;

  case UI_MENU_ITEM_MEDIA_DISK_BETA_D_FLIP_SET:
    break;

  case UI_MENU_ITEM_MEDIA_DISK_BETA_D_WP_SET:
    break;

  case UI_MENU_ITEM_MEDIA_DISK_OPUS:
    method = @selector(ui_menu_activate_media_disk_opus:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_OPUS_1:
    method = @selector(ui_menu_activate_media_disk_opus_a:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_OPUS_1_EJECT:
    method = @selector(ui_menu_activate_media_disk_opus_a_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_OPUS_1_FLIP_SET:
    method = @selector(ui_menu_activate_media_disk_opus_a_flip:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_OPUS_1_WP_SET:
    method = @selector(ui_menu_activate_media_opus_a_wp_set:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_OPUS_2:
    method = @selector(ui_menu_activate_media_disk_opus_b:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_OPUS_2_EJECT:
    method = @selector(ui_menu_activate_media_disk_opus_b_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_OPUS_2_FLIP_SET:
    method = @selector(ui_menu_activate_media_disk_opus_b_flip:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_OPUS_2_WP_SET:
    method = @selector(ui_menu_activate_media_opus_b_wp_set:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_PLUSD:
    method = @selector(ui_menu_activate_media_disk_plusd:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_PLUSD_1:
    method = @selector(ui_menu_activate_media_disk_plusd_a:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_PLUSD_1_EJECT:
    method = @selector(ui_menu_activate_media_disk_plusd_a_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_PLUSD_1_FLIP_SET:
    method = @selector(ui_menu_activate_media_disk_plusd_a_flip:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_PLUSD_1_WP_SET:
    method = @selector(ui_menu_activate_media_plusd_a_wp_set:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_PLUSD_2:
    method = @selector(ui_menu_activate_media_disk_plusd_b:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_PLUSD_2_EJECT:
    method = @selector(ui_menu_activate_media_disk_plusd_b_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_PLUSD_2_FLIP_SET:
    method = @selector(ui_menu_activate_media_disk_plusd_b_flip:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_PLUSD_2_WP_SET:
    method = @selector(ui_menu_activate_media_plusd_b_wp_set:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_DISCIPLE:
    method = @selector(ui_menu_activate_media_disk_disciple:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_DISCIPLE_1:
    method = @selector(ui_menu_activate_media_disk_disciple_a:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_DISCIPLE_1_EJECT:
    method = @selector(ui_menu_activate_media_disk_disciple_a_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_DISCIPLE_1_FLIP_SET:
    method = @selector(ui_menu_activate_media_disk_disciple_a_flip:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_DISCIPLE_1_WP_SET:
    method = @selector(ui_menu_activate_media_disciple_a_wp_set:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_DISCIPLE_2:
    method = @selector(ui_menu_activate_media_disk_disciple_b:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_DISCIPLE_2_EJECT:
    method = @selector(ui_menu_activate_media_disk_disciple_b_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_DISCIPLE_2_FLIP_SET:
    method = @selector(ui_menu_activate_media_disk_disciple_b_flip:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_DISCIPLE_2_WP_SET:
    method = @selector(ui_menu_activate_media_disciple_b_wp_set:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_DIDAKTIK:
    method = @selector(ui_menu_activate_media_disk_didaktik:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_DIDAKTIK_A:
    method = @selector(ui_menu_activate_media_disk_didaktik_a:);
    break;

  case UI_MENU_ITEM_MEDIA_DISK_DIDAKTIK_A_EJECT:
    method = @selector(ui_menu_activate_media_disk_didaktik_a_eject:);
    break;
      
  case UI_MENU_ITEM_MEDIA_DISK_DIDAKTIK_A_FLIP_SET:
    method = @selector(ui_menu_activate_media_disk_didaktik_a_flip:);
    break;
      
  case UI_MENU_ITEM_MEDIA_DISK_DIDAKTIK_A_WP_SET:
    method = @selector(ui_menu_activate_media_didaktik_a_wp_set:);
    break;
      
  case UI_MENU_ITEM_MEDIA_DISK_DIDAKTIK_B:
    method = @selector(ui_menu_activate_media_disk_didaktik_b:);
    break;
      
  case UI_MENU_ITEM_MEDIA_DISK_DIDAKTIK_B_EJECT:
    method = @selector(ui_menu_activate_media_disk_didaktik_b_eject:);
    break;
      
  case UI_MENU_ITEM_MEDIA_DISK_DIDAKTIK_B_FLIP_SET:
    method = @selector(ui_menu_activate_media_disk_didaktik_b_flip:);
    break;
      
  case UI_MENU_ITEM_MEDIA_DISK_DIDAKTIK_B_WP_SET:
    method = @selector(ui_menu_activate_media_didaktik_b_wp_set:);
    break;
      
  case UI_MENU_ITEM_RECORDING:
    method = @selector(ui_menu_activate_recording:);
    break;
  
  case UI_MENU_ITEM_FILE_MOVIE_RECORDING:
    method = @selector(ui_menu_activate_movie_recording:);
    break;
      
  case UI_MENU_ITEM_FILE_MOVIE_PAUSE:
    method = @selector(ui_menu_activate_movie_pause:);
    break;

  case UI_MENU_ITEM_RECORDING_ROLLBACK:
    method = @selector(ui_menu_activate_recording_rollback:);
    break;

  case UI_MENU_ITEM_AY_LOGGING:
    method = @selector(ui_menu_activate_ay_logging:);
    break;

  case UI_MENU_ITEM_MEDIA_IDE:
    method = @selector(ui_menu_activate_media_ide:);
    break;
    
  case UI_MENU_ITEM_MEDIA_IDE_SIMPLE8BIT:
    method = @selector(ui_menu_activate_media_ide_simple8bit:);
    break;

  case UI_MENU_ITEM_MEDIA_IDE_SIMPLE8BIT_MASTER_EJECT:
    method = @selector(ui_menu_activate_media_ide_simple8bit_master_eject:);
    break;
    
  case UI_MENU_ITEM_MEDIA_IDE_SIMPLE8BIT_SLAVE_EJECT:
    method = @selector(ui_menu_activate_media_ide_simple8bit_slave_eject:);
    break;
    
  case UI_MENU_ITEM_MEDIA_IDE_ZXATASP:
    method = @selector(ui_menu_activate_media_ide_zxatasp:);
    break;

  case UI_MENU_ITEM_MEDIA_IDE_ZXATASP_MASTER_EJECT:
    method = @selector(ui_menu_activate_media_ide_zxatasp_master_eject:);
    break;
    
  case UI_MENU_ITEM_MEDIA_IDE_ZXATASP_SLAVE_EJECT:
    method = @selector(ui_menu_activate_media_ide_zxatasp_slave_eject:);
    break;
    
  case UI_MENU_ITEM_MEDIA_IDE_ZXCF:
    method = @selector(ui_menu_activate_media_ide_zxcf:);
    break;

  case UI_MENU_ITEM_MEDIA_IDE_ZXCF_EJECT:
    method = @selector(ui_menu_activate_media_ide_zxcf_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_IDE_DIVMMC:
    method = @selector(ui_menu_activate_media_ide_divmmc:);
    break;

  case UI_MENU_ITEM_MEDIA_IDE_DIVMMC_EJECT:
    method = @selector(ui_menu_activate_media_ide_divmmc_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_IDE_ZXMMC:
    method = @selector(ui_menu_activate_media_ide_zxmmc:);
    break;

  case UI_MENU_ITEM_MEDIA_IDE_ZXMMC_EJECT:
    method = @selector(ui_menu_activate_media_ide_zxmmc_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_IF1:
    method = @selector(ui_menu_activate_media_if1:);
    break;

  case UI_MENU_ITEM_MEDIA_IF1_M1_EJECT:
    method = @selector(ui_menu_activate_media_if1_m1_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_IF1_M1_WP_SET:
    method = @selector(ui_menu_activate_media_if1_m1_wp_set:);
    break;

  case UI_MENU_ITEM_MEDIA_IF1_M2_EJECT:
    method = @selector(ui_menu_activate_media_if1_m2_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_IF1_M2_WP_SET:
    method = @selector(ui_menu_activate_media_if1_m2_wp_set:);
    break;

  case UI_MENU_ITEM_MEDIA_IF1_M3_EJECT:
    method = @selector(ui_menu_activate_media_if1_m3_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_IF1_M3_WP_SET:
    method = @selector(ui_menu_activate_media_if1_m3_wp_set:);
    break;

  case UI_MENU_ITEM_MEDIA_IF1_M4_EJECT:
    method = @selector(ui_menu_activate_media_if1_m4_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_IF1_M4_WP_SET:
    method = @selector(ui_menu_activate_media_if1_m4_wp_set:);
    break;

  case UI_MENU_ITEM_MEDIA_IF1_M5_EJECT:
    method = @selector(ui_menu_activate_media_if1_m5_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_IF1_M5_WP_SET:
    method = @selector(ui_menu_activate_media_if1_m5_wp_set:);
    break;

  case UI_MENU_ITEM_MEDIA_IF1_M6_EJECT:
    method = @selector(ui_menu_activate_media_if1_m6_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_IF1_M6_WP_SET:
    method = @selector(ui_menu_activate_media_if1_m6_wp_set:);
    break;

  case UI_MENU_ITEM_MEDIA_IF1_M7_EJECT:
    method = @selector(ui_menu_activate_media_if1_m7_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_IF1_M7_WP_SET:
    method = @selector(ui_menu_activate_media_if1_m7_wp_set:);
    break;

  case UI_MENU_ITEM_MEDIA_IF1_M8_EJECT:
    method = @selector(ui_menu_activate_media_if1_m8_eject:);
    break;

  case UI_MENU_ITEM_MEDIA_IF1_M8_WP_SET:
    method = @selector(ui_menu_activate_media_if1_m8_wp_set:);
    break;

  case UI_MENU_ITEM_MEDIA_IF1_RS232_UNPLUG_R:
  case UI_MENU_ITEM_MEDIA_IF1_RS232_UNPLUG_T:
  case UI_MENU_ITEM_MEDIA_IF1_SNET_UNPLUG:
    break;

  case UI_MENU_ITEM_MACHINE_PROFILER:
    method = @selector(ui_menu_activate_machine_profiler:);
    break;

  case UI_MENU_ITEM_MEDIA_IDE_DIVIDE:
    method = @selector(ui_menu_activate_media_ide_divide:);
    break;

  case UI_MENU_ITEM_MEDIA_IDE_DIVIDE_MASTER_EJECT:
    method = @selector(ui_menu_activate_media_ide_divide_master_eject:);
    break;
    
  case UI_MENU_ITEM_MEDIA_IDE_DIVIDE_SLAVE_EJECT:
    method = @selector(ui_menu_activate_media_ide_divide_slave_eject:);
    break;
    
  case UI_MENU_ITEM_TAPE_RECORDING:
    method = @selector(ui_menu_activate_tape_record:);
    break;
    
  case UI_MENU_ITEM_FILE_SVG_CAPTURE:
    break;

  case UI_MENU_ITEM_MACHINE_DIDAKTIK80_SNAP:
    method = @selector(ui_menu_activate_didaktik80_snap:);
    break;

  case UI_MENU_ITEM_MACHINE_MULTIFACE:
    method = @selector(ui_menu_activate_multiface:);
    break;
    
  case UI_MENU_ITEM_MACHINE_DEBUGGER:
    method = @selector(ui_menu_activate_debugger:);
    break;

  case UI_MENU_ITEM_MACHINE_DEBUG_LOG:
    break;
      
  default:
    ui_error( UI_ERROR_ERROR, "Attempt to activate unknown menu item %d",
              item );
    return 1;

  }

  if( method )
    [[FuseController singleton] 
          performSelectorOnMainThread:method
          withObject:activeBool
          waitUntilDone:NO
    ];

  return 0;
}

int
cocoaui_confirm( const char *message )
{
  int confirm = 0;
  int result;

  if( !settings_current.confirm_actions ) return 1;

  if( ui_mouse_grabbed ) ui_mouse_grabbed = ui_mouse_release( 1 );

  [[DisplayOpenGLView instance] pause];

  result = NSRunAlertPanel(@"Confirm", @(message), @"OK", @"Cancel", nil);

  if( result == NSAlertDefaultReturn ) confirm = 1;

  [[DisplayOpenGLView instance] unpause];

  return confirm;
}
