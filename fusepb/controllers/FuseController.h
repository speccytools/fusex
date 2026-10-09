/* FuseController.h: Routines for dealing with the Cocoa UI
   Copyright (c) 2002-2007 Fredrick Meunier

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

#import <Cocoa/Cocoa.h>

#include "specplus3.h"
#include "ui/ui.h"

@class DebuggerController;
@class KeyboardController;
@class LoadBinaryController;
@class MemoryBrowserController;
@class PreferencesController;
@class PokeFinderController;
@class PokeMemoryController;
@class RollbackController;
@class SaveBinaryController;
@class TapeBrowserController;
@class SPUStandardUpdaterController;

@interface FuseController : NSObject
{
  IBOutlet NSMenuItem *cartridge;
  IBOutlet NSMenuItem *mdr1;
  IBOutlet NSMenuItem *mdr2;
  IBOutlet NSMenuItem *if1Wp1;
  IBOutlet NSMenuItem *if1Wp2;
  IBOutlet NSMenuItem *diskA;
  IBOutlet NSMenuItem *diskB;
  IBOutlet NSMenuItem *diskFlipA;
  IBOutlet NSMenuItem *diskWpA;
  IBOutlet NSMenuItem *diskFlipB;
  IBOutlet NSMenuItem *diskWpB;
  IBOutlet NSMenuItem *ideMaster;
  IBOutlet NSMenuItem *ideSlave;
  IBOutlet NSMenuItem *simple8Bit;
  IBOutlet NSMenuItem *pause;
  IBOutlet NSMenuItem *tapePlay;
  IBOutlet NSMenuItem *tapeRecord;
  IBOutlet NSMenuItem *debugger;
  IBOutlet NSMenu *recentSnaps;
  IBOutlet NSWindow *window;

  IBOutlet id savePanelAccessoryView;
  IBOutlet NSPopUpButton* saveFileType;

  KeyboardController *keyboardController;
  LoadBinaryController *loadBinaryController;
  MemoryBrowserController *memoryBrowserController;
  PreferencesController *preferencesController;
  PokeFinderController *pokeFinderController;
  PokeMemoryController *pokeMemoryController;
  RollbackController *rollbackController;
  SaveBinaryController *saveBinaryController;
  TapeBrowserController *tapeBrowserController;
  SPUStandardUpdaterController *sparkleUpdaterController;

  BOOL emulatorConnected;
  NSMutableArray *pendingOpenFiles;
}
+ (FuseController *)singleton;

- (IBAction)cocoa_break:(id)sender;
- (IBAction)disk_eject_a:(id)sender;
- (IBAction)disk_eject_b:(id)sender;
- (IBAction)disk_save_a:(id)sender;
- (IBAction)disk_save_as_a:(id)sender;
- (IBAction)disk_save_b:(id)sender;
- (IBAction)disk_save_as_b:(id)sender;
- (IBAction)disk_new_a:(id)sender;
- (IBAction)disk_open_a:(id)sender;
- (IBAction)disk_new_b:(id)sender;
- (IBAction)disk_open_b:(id)sender;
- (IBAction)disk_write_protect_a:(id)sender;
- (IBAction)disk_write_protect_b:(id)sender;
- (IBAction)disk_flip_a:(id)sender;
- (IBAction)disk_flip_b:(id)sender;
- (IBAction)dock_eject:(id)sender;
- (IBAction)dock_open:(id)sender;
- (IBAction)cart_eject:(id)sender;
- (IBAction)cart_open:(id)sender;
- (IBAction)export_screen:(id)sender;
- (IBAction)hard_reset:(id)sender;
- (IBAction)ide_commit:(id)sender;
- (IBAction)ide_eject:(id)sender;
- (IBAction)ide_insert:(id)sender;
- (IBAction)if2_eject:(id)sender;
- (IBAction)if2_open:(id)sender;
- (IBAction)joystick_keyboard:(id)sender;
- (IBAction)keyboard_recreated:(id)sender;
- (IBAction)keyboard_arrows_shifted:(id)sender;
- (IBAction)mdr_eject:(id)sender;
- (IBAction)mdr_save:(id)sender;
- (IBAction)mdr_save_as:(id)sender;
- (IBAction)mdr_insert:(id)sender;
- (IBAction)mdr_insert_new:(id)sender;
- (IBAction)mdr_writep:(id)sender;
- (IBAction)nmi:(id)sender;
- (IBAction)open:(id)sender;
- (IBAction)openSpectranetRAMFSFolder:(id)sender;
- (IBAction)openSupplementaryROMsFolder:(id)sender;
- (IBAction)open_screen:(id)sender;
- (IBAction)pause:(id)sender;
- (IBAction)profiler_start:(id)sender;
- (IBAction)profiler_stop:(id)sender;
- (IBAction)psg_start:(id)sender;
- (IBAction)psg_stop:(id)sender;
- (IBAction)reset:(id)sender;
- (IBAction)resetUserDefaults:(id)sender;
- (IBAction)rzx_insert_snap:(id)sender;
- (IBAction)rzx_play:(id)sender;
- (IBAction)rzx_rollback:(id)sender;
- (IBAction)rzx_start:(id)sender;
- (IBAction)rzx_start_snap:(id)sender;
- (IBAction)rzx_stop:(id)sender;
- (IBAction)rzx_continue:(id)sender;
- (IBAction)rzx_finalise:(id)sender;
- (IBAction)save_as:(id)sender;
- (IBAction)save_options:(id)sender;
- (IBAction)save_screen:(id)sender;
- (IBAction)tape_clear:(id)sender;
- (IBAction)tape_open:(id)sender;
- (IBAction)tape_play:(id)sender;
- (IBAction)tape_rewind:(id)sender;
- (IBAction)tape_write:(id)sender;
- (IBAction)tape_record:(id)sender;
- (IBAction)movie_record:(id)sender;
- (IBAction)movie_record_from_rzx:(id)sender;
- (IBAction)movie_pause:(id)sender;
- (IBAction)movie_stop:(id)sender;
- (IBAction)didaktik80_snap:(id)sender;
- (IBAction)multiface_red_button:(id)sender;

- (IBAction)quit:(id)sender;
- (IBAction)hide:(id)sender;
- (IBAction)help:(id)sender;

- (IBAction)showRollbackPane:(id)sender;
- (IBAction)showTapeBrowserPane:(id)sender;
- (IBAction)showKeyboardPane:(id)sender;
- (IBAction)showLoadBinaryPane:(id)sender;
- (IBAction)showSaveBinaryPane:(id)sender;
- (IBAction)showPokeFinderPane:(id)sender;
- (IBAction)showPokeMemoryPane:(id)sender;
- (IBAction)showMemoryBrowserPane:(id)sender;
- (IBAction)showPreferencesPane:(id)sender;

- (IBAction)saveFileTypeClicked:(id)sender;

- savePanelAccessoryView;
@property (getter=saveFileType,readonly) NSPopUpButton* saveFileType;

- (void)ui_menu_activate_media_cartridge:(NSNumber*)active;
- (void)ui_menu_activate_media_cartridge_dock:(NSNumber*)active;
- (void)ui_menu_activate_media_cartridge_dock_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_cartridge_if2:(NSNumber*)active;
- (void)ui_menu_activate_media_cartridge_if2_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_disk:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_a_flip;
- (void)ui_menu_activate_media_disk_a_wp_set;
- (void)ui_menu_activate_media_disk_b_flip;
- (void)ui_menu_activate_media_disk_b_wp_set;
- (void)ui_menu_activate_media_disk_plus3:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_plus3_a_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_plus3_a_flip:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_plus3_a_wp_set:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_plus3_b:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_plus3_b_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_plus3_b_flip:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_plus3_b_wp_set:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_beta:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_beta_a:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_beta_a_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_beta_a_flip:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_beta_a_wp_set:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_beta_b:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_beta_b_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_beta_b_flip:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_beta_b_wp_set:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_opus:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_opus_a:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_opus_a_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_opus_a_flip:(NSNumber*)active;
- (void)ui_menu_activate_media_opus_a_wp_set:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_opus_b:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_opus_b_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_opus_b_flip:(NSNumber*)active;
- (void)ui_menu_activate_media_opus_b_wp_set:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_plusd:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_plusd_a:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_plusd_a_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_plusd_a_flip:(NSNumber*)active;
- (void)ui_menu_activate_media_plusd_a_wp_set:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_plusd_b:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_plusd_b_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_plusd_b_flip:(NSNumber*)active;
- (void)ui_menu_activate_media_plusd_b_wp_set:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_disciple:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_disciple_a:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_disciple_a_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_disciple_a_flip:(NSNumber*)active;
- (void)ui_menu_activate_media_disciple_a_wp_set:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_disciple_b:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_disciple_b_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_disk_disciple_b_flip:(NSNumber*)active;
- (void)ui_menu_activate_media_disciple_b_wp_set:(NSNumber*)active;
- (void)ui_menu_activate_media_if1:(NSNumber*)active;
- (void)ui_menu_activate_media_if1_m1_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_if1_m1_wp_set:(NSNumber*)active;
- (void)ui_menu_activate_media_if1_m2_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_if1_m2_wp_set:(NSNumber*)active;
- (void)ui_menu_activate_media_if1_m3_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_if1_m3_wp_set:(NSNumber*)active;
- (void)ui_menu_activate_media_if1_m4_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_if1_m4_wp_set:(NSNumber*)active;
- (void)ui_menu_activate_media_if1_m5_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_if1_m5_wp_set:(NSNumber*)active;
- (void)ui_menu_activate_media_if1_m6_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_if1_m6_wp_set:(NSNumber*)active;
- (void)ui_menu_activate_media_if1_m7_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_if1_m7_wp_set:(NSNumber*)active;
- (void)ui_menu_activate_media_if1_m8_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_if1_m8_wp_set:(NSNumber*)active;
- (void)ui_menu_activate_media_ide:(NSNumber*)active;
- (void)ui_menu_activate_media_ide_divide:(NSNumber*)active;
- (void)ui_menu_activate_media_ide_divide_master_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_ide_divide_slave_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_ide_simple8bit:(NSNumber*)active;
- (void)ui_menu_activate_media_ide_simple8bit_master_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_ide_simple8bit_slave_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_ide_zxatasp:(NSNumber*)active;
- (void)ui_menu_activate_media_ide_zxatasp_master_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_ide_zxatasp_slave_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_ide_zxcf:(NSNumber*)active;
- (void)ui_menu_activate_media_ide_zxcf_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_ide_divmmc:(NSNumber*)active;
- (void)ui_menu_activate_media_ide_divmmc_eject:(NSNumber*)active;
- (void)ui_menu_activate_media_ide_zxmmc:(NSNumber*)active;
- (void)ui_menu_activate_media_ide_zxmmc_eject:(NSNumber*)active;
- (void)ui_menu_activate_recording:(NSNumber*)active;
- (void)ui_menu_activate_recording_rollback:(NSNumber*)active;
- (void)ui_menu_activate_ay_logging:(NSNumber*)active;
- (void)ui_menu_activate_machine_profiler:(NSNumber*)active;
- (void)ui_menu_activate_tape_record:(NSNumber*)active;

- (void)openFile:(const char *)filename;
- (void)openRecent:(id)sender;
- (void)addRecentSnapshotWithString:(NSString*)file;
- (void)addRecentSnapshot:(const char *)filename;
- (void)clearRecentSnapshots;
- (void)setTitle:(NSString*)title;

- (void)newDisk:(int)drive;
- (void)openDisk:(int)drive;

- (void)setDiskState:(NSNumber*)state;
- (void)setTapeState:(NSNumber*)state;
- (void)setMdrState:(NSNumber*)state;
- (void)setPauseState;

- (void)showAlertPanel:(NSString*)message;
- (void)showCriticalAlertPanel:(NSString*)message;

-(ui_confirm_save_t) confirmSave:(NSString*)theMessage;
-(int) confirm:(NSString*)theMessage;
-(int) tapeWrite;
-(int) diskWrite:(int)which saveAs:(bool)saveas;
-(int) if1MdrWrite:(int)which saveAs:(bool)saveas;
-(ui_confirm_joystick_t) confirmJoystick:(libspectrum_joystick)type inputs:(int)theInputs;

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)theApplication;
- (BOOL)application:(NSApplication *)theApplication openFile:(NSString *)filename;
- (void)emulatorDidConnect;

- (void)setAcceptsMouseMovedEvents:(BOOL)flag;

@end
