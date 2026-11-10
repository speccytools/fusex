/* TapeBrowserController.m: Routines for dealing with the Tape Browser Panel
   Copyright (c) 2002-2004 Fredrick Meunier

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

#include <config.h>

#import "DisplayOpenGLView.h"
#import "Emulator.h"
#import "TapeBrowserController.h"

#include "tape.h"

static void
add_block_details( libspectrum_tape_block *block, void *user_data );

static int dialog_created;      /* Have we created the dialog box yet? */

@implementation TapeBrowserController

static TapeBrowserController *singleton = nil;
   
+ (TapeBrowserController *)singleton 
{
  return singleton ? singleton : [[self alloc] init];
}

- (id)init
{
  if ( singleton ) {
    [self dealloc];
  } else {
    self = [super initWithWindowNibName:@"TapeBrowser"];
    singleton = self;

    [self setWindowFrameAutosaveName:@"TapeBrowserWindow"];

    dialog_created = 1;
	
    initialising = NO;
  }

  return singleton;
}

- (void)tableViewSelectionDidChange:(NSNotification *)aNotification
{
  int row, current_block;
  
  row = [tapeBrowser selectedRow];

  if( row < 0 || row >= [tapeBrowser numberOfRows] ) return;

  /* Don't do anything if the current block was clicked on */
  current_block = tape_get_current_block();
  if( initialising == YES || row == current_block ) return;

  /* Otherwise, select the new block */
  tape_select_block_no_update( row ); 
}

- (void)dealloc
{
  dialog_created = 0;
      
  [super dealloc];
}

- (IBAction)apply:(id)sender
{
  [[self window] close];
}

- (void)showWindow:(id)sender
{
  [super showWindow:sender];

  [[DisplayOpenGLView instance] tapeWindowInitialise];
}

- (void)clearContents
{
  [tapeController removeObjects:[tapeController arrangedObjects]];
  [infoController removeObjects:[infoController arrangedObjects]];
}

- (void)addObjectToTapeContents:(NSDictionary*)info
{
  [tapeController addObject:info];
}

- (void)addObjectToInfoContents:(NSDictionary*)info
{
  [infoController addObject:info];
}

- (void)setTapeIndex:(NSNumber*)index
{
  NSUInteger idx = [index unsignedIntValue];
  [tapeController setSelectionIndex:idx];
  [tapeBrowser scrollRowToVisible:(NSInteger)idx];
}

- (void)setInitialising:(NSNumber*)value
{
  initialising = [value boolValue];
}

- (void)setDocumentEditedFlag:(NSNumber*)flag
{
  [[self window] setDocumentEdited:[flag boolValue]];
}

/* Runs on the emulator thread to serialise the tape_foreach walk with
   save-trap / RZX block appends. */
- (void)populateFromTape
{
  int current_block;

  [self performSelectorOnMainThread:@selector(clearContents)
                          withObject:nil
                       waitUntilDone:NO];
  [self performSelectorOnMainThread:@selector(setInitialising:)
                          withObject:@(YES)
                       waitUntilDone:NO];
  tape_foreach( add_block_details, self );
  [self performSelectorOnMainThread:@selector(setInitialising:)
                          withObject:@(NO)
                       waitUntilDone:NO];

  current_block = tape_get_current_block();
  if( current_block >= 0 ) {
    [self performSelectorOnMainThread:@selector(setTapeIndex:)
                            withObject:@((unsigned int)current_block)
                         waitUntilDone:NO];
  }
}

@synthesize tapeController;
@synthesize infoController;
@end

static void
add_block_details( libspectrum_tape_block *block, void *user_data )
{
  TapeBrowserController *tapeBrowserController = (TapeBrowserController*)user_data;
  NSString *type, *data;
  char buffer[256], *details;
  NSArray *keys = @[@"type", @"data"];
  NSArray *values;

  libspectrum_tape_block_description( buffer, 256, block );
  type = @(buffer);
  if( tape_block_details( &details, block ) ) return;
  data = @(details);
  libspectrum_free( details );
  values = @[type, data];
  [tapeBrowserController
        performSelectorOnMainThread:@selector(addObjectToTapeContents:)
        withObject:[NSDictionary dictionaryWithObjects:values forKeys:keys]
        waitUntilDone:NO
  ];
  
  if( libspectrum_tape_block_type( block ) == LIBSPECTRUM_TAPE_BLOCK_ARCHIVE_INFO ) {
    int i;
    for( i = 0; i < libspectrum_tape_block_count( block ); i++ ) {
      NSString *info;

      switch( libspectrum_tape_block_ids( block, i ) ) {
      case   0:
        values = @[@"Title",
                    [NSString stringWithCString:(const char *)libspectrum_tape_block_texts( block, i )
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1040
                      encoding:NSWindowsCP1252StringEncoding
#endif
                      ]];
        [tapeBrowserController
              performSelectorOnMainThread:@selector(addObjectToInfoContents:)
              withObject:[NSDictionary dictionaryWithObjects:values forKeys:keys]
              waitUntilDone:NO
        ];
        break;          
      case   1:
        info = [NSString stringWithCString:
                  (const char *)libspectrum_tape_block_texts( block, i )
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1040
                  encoding:NSWindowsCP1252StringEncoding
#endif
                  ];
        values = @[@"Publishers",
                    [[info componentsSeparatedByString:@"\n"] componentsJoinedByString:@", "]];
        [tapeBrowserController
              performSelectorOnMainThread:@selector(addObjectToInfoContents:)
              withObject:[NSDictionary dictionaryWithObjects:values forKeys:keys]
              waitUntilDone:NO
        ];
        break;
      case   2:
        info = [NSString stringWithCString:
        (const char *)libspectrum_tape_block_texts( block, i )
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1040
        encoding:NSWindowsCP1252StringEncoding
#endif
        ];
        values = @[@"Authors",
                    [[info componentsSeparatedByString:@"\n"] componentsJoinedByString:@", "]];
        [tapeBrowserController
              performSelectorOnMainThread:@selector(addObjectToInfoContents:)
              withObject:[NSDictionary dictionaryWithObjects:values forKeys:keys]
              waitUntilDone:NO
        ];
        break;
      case   3:
        values = @[@"Year",
                    @([[NSString stringWithCString:
                        (const char *)libspectrum_tape_block_texts( block, i )
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1040
                        encoding:NSWindowsCP1252StringEncoding
#endif
                        ] intValue])];
        [tapeBrowserController
              performSelectorOnMainThread:@selector(addObjectToInfoContents:)
              withObject:[NSDictionary dictionaryWithObjects:values forKeys:keys]
              waitUntilDone:NO
        ];
        break;
      case   4:
        info = [NSString stringWithCString:
                  (const char *)libspectrum_tape_block_texts( block, i )
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1040
                  encoding:NSWindowsCP1252StringEncoding
#endif
                  ];
        values = @[@"Languages",
                    [[info componentsSeparatedByString:@"\n"] componentsJoinedByString:@", "]];
        [tapeBrowserController
              performSelectorOnMainThread:@selector(addObjectToInfoContents:)
              withObject:[NSDictionary dictionaryWithObjects:values forKeys:keys]
              waitUntilDone:NO
        ];
        break;
      case   5:
        values = @[@"Category",
                    [NSString stringWithCString:(const char *)libspectrum_tape_block_texts( block, i )
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1040
                      encoding:NSWindowsCP1252StringEncoding
#endif
                      ]];
        [tapeBrowserController
              performSelectorOnMainThread:@selector(addObjectToInfoContents:)
              withObject:[NSDictionary dictionaryWithObjects:values forKeys:keys]
              waitUntilDone:NO
        ];
        break;
      case   6:
        {
        const char *infoString =
          libspectrum_tape_block_texts( block, i );
        NSMutableString *priceString =
          [NSMutableString stringWithCString:infoString 
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1040
                encoding:NSWindowsCP1252StringEncoding
#endif
                ];
        // WoS Infoseek has been putting HTML-style "&euro;" in for the
        // Euro symbol which isn't in the ISO Latin 1 string encoding.
        // Martijn has agreed to use CP1252 (a superset of Latin 1)
        // instead.
        // In case of encountering some old blocks we support
        // translating "&euro;" to the correect sign as well as
        // supporting CP1252 encoding on import replace it with the
        // standard euro sign
        [priceString replaceOccurrencesOfString:@"&euro;"
            withString:@"€"
               options:NSCaseInsensitiveSearch
                 range:NSMakeRange(0, [priceString length])];
        values = @[@"Price", priceString];
        [tapeBrowserController
              performSelectorOnMainThread:@selector(addObjectToInfoContents:)
              withObject:[NSDictionary dictionaryWithObjects:values forKeys:keys]
              waitUntilDone:NO
        ];
        }
        break;
      case   7:
        values = @[@"Loader",
                    [NSString stringWithCString:(const char *)libspectrum_tape_block_texts( block, i )
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1040
                      encoding:NSWindowsCP1252StringEncoding
#endif
                      ]];
        [tapeBrowserController
              performSelectorOnMainThread:@selector(addObjectToInfoContents:)
              withObject:[NSDictionary dictionaryWithObjects:values forKeys:keys]
              waitUntilDone:NO
        ];
        break;
      case   8:
        values = @[@"Origin",
                    [NSString stringWithCString:(const char *)libspectrum_tape_block_texts( block, i )
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1040
                      encoding:NSWindowsCP1252StringEncoding
#endif
                      ]];
        [tapeBrowserController
              performSelectorOnMainThread:@selector(addObjectToInfoContents:)
              withObject:[NSDictionary dictionaryWithObjects:values forKeys:keys]
              waitUntilDone:NO
        ];
        break;
      case 255:
        values = @[@"Comment",
                    [NSString stringWithCString:(const char *)libspectrum_tape_block_texts( block, i )
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1040
                      encoding:NSWindowsCP1252StringEncoding
#endif
                      ]];
        [tapeBrowserController
              performSelectorOnMainThread:@selector(addObjectToInfoContents:)
              withObject:[NSDictionary dictionaryWithObjects:values forKeys:keys]
              waitUntilDone:NO
        ];
        break; 
      default: NSLog(@"(Unknown string): %s",
        (const char *)libspectrum_tape_block_texts( block, i ));
        break;
      }
    }
  }
}

int
ui_tape_browser_update( ui_tape_browser_update_type change,
                        libspectrum_tape_block *block )
{
  TapeBrowserController* tapeBrowserController;

  if( !dialog_created ) return 0;

  tapeBrowserController = [TapeBrowserController singleton];

  if( change == UI_TAPE_BROWSER_NEW_TAPE ) {
    NSThread *emulThread = [[Emulator instance] emulatorThread];
    if( emulThread == nil || [NSThread currentThread] == emulThread ) {
      [tapeBrowserController populateFromTape];
    } else {
      [tapeBrowserController
            performSelector:@selector(populateFromTape)
            onThread:emulThread
            withObject:nil
            waitUntilDone:NO
      ];
    }
  }

  if( change == UI_TAPE_BROWSER_SELECT_BLOCK ) {
    int current_block = tape_get_current_block();
    if(current_block >= 0) {
      [tapeBrowserController
            performSelectorOnMainThread:@selector(setTapeIndex:)
            withObject:@((unsigned int)current_block)
            waitUntilDone:NO
      ];
    }
  }

  if( change == UI_TAPE_BROWSER_NEW_BLOCK && block ) {
    add_block_details( block, tapeBrowserController );
  }

  [tapeBrowserController
        performSelectorOnMainThread:@selector(setDocumentEditedFlag:)
        withObject:@(tape_modified ? YES : NO)
        waitUntilDone:NO
  ];

  return 0;
}
