/* DisplayOpenGLView.m: Implementation for the DisplayOpenGLView class
   Copyright (c) 2006-2007 Fredrick Meunier

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

   E-mail: fredm@spamcop.net

*/

#import "DisplayOpenGLView.h"
#import "Emulator.h"
#import "FuseController.h"
#import "DebuggerController.h"
#import "Texture.h"

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include "fuse.h"
#include "fusepb/main.h"
#include "settings.h"
#include "ui/cocoa/cocoaui.h"
#include "ui/cocoa/dirty.h"

#define QZ_f 0x03
#define QZ_0 0x1D
#define QZ_1 0x12
#define QZ_2 0x13
#define QZ_3 0x14
#define QZ_4 0x15
#define QZ_5 0x16
#define QZ_m 0x2E

/* Vertex layout shared with DisplayShaders.metal. Position is in clip
   space (-1..1); texture coordinate is normalized (0..1). */
typedef struct {
  float position[2];
  float tex_coord[2];
} DisplayVertex;

/* Quad as two triangles, six vertices, top-left -> bottom-right. */
static void
fill_quad( DisplayVertex *out,
           float x0, float y0, float x1, float y1,
           float u0, float v0, float u1, float v1 )
{
  out[0] = (DisplayVertex){ { x0, y0 }, { u0, v0 } };
  out[1] = (DisplayVertex){ { x1, y0 }, { u1, v0 } };
  out[2] = (DisplayVertex){ { x0, y1 }, { u0, v1 } };
  out[3] = (DisplayVertex){ { x1, y0 }, { u1, v0 } };
  out[4] = (DisplayVertex){ { x1, y1 }, { u1, v1 } };
  out[5] = (DisplayVertex){ { x0, y1 }, { u0, v1 } };
}

static float
map_clip_coordinate( float value, float min, float max )
{
  return min + ( value + 1.0f ) * 0.5f * ( max - min );
}

/* Convert a full frame of 16-bit BGR5A1 pixels (Spectrum framebuffer
   format) to 32-bit BGRA8 packed in little-endian word order, suitable
   for direct upload to MTLPixelFormatBGRA8Unorm via -replaceRegion:. */
static void
convert_bgr5a1_to_bgra8( const uint16_t *src, int src_pitch_words,
                         uint32_t *dst, int w, int h )
{
  for( int row = 0; row < h; row++ ) {
    const uint16_t *srow = src + row * src_pitch_words;
    uint32_t *drow = dst + row * w;
    for( int col = 0; col < w; col++ ) {
      uint16_t p = srow[col];
      uint32_t b5 = p & 0x1F;
      uint32_t g5 = (p >> 5) & 0x1F;
      uint32_t r5 = (p >> 10) & 0x1F;
      uint32_t b8 = (b5 << 3) | (b5 >> 2);
      uint32_t g8 = (g5 << 3) | (g5 >> 2);
      uint32_t r8 = (r5 << 3) | (r5 >> 2);
      drow[col] = (0xFFu << 24) | (r8 << 16) | (g8 << 8) | b8;
    }
  }
}

static int
get_offset( int window_width, int window_height,
            int image_width, int image_height, float* width_adjustment ) {
  static const float FULL_IMAGE_RATIO = 4./3.;
  static const float TOP_BORDER_HAIRCUT = 24.;
  static const float MINIMAL_HEIGHT = 240.-TOP_BORDER_HAIRCUT*2.;
  static const float MINIMAL_TOP_BOTTOM_RATIO = 320./MINIMAL_HEIGHT;
  static const float SIDE_BORDER_HAIRCUT = 31.;
  static const float MINIMAL_WIDTH = 320.-SIDE_BORDER_HAIRCUT*2.;
  static const float MINIMAL_LEFT_RIGHT_RATIO = MINIMAL_WIDTH/240.;
  float ratio = window_width/(float)window_height;
  *width_adjustment = 0.;
  if( fabs( ratio - FULL_IMAGE_RATIO ) < 0.01f ) {
    /* no change, we are already at 4:3 */
    return 0;
  }

  if( !settings_current.full_screen_panorama ) {
    // if wider then desirable then use max height and have black bars on left
    // and right borders
    if( ratio > FULL_IMAGE_RATIO ) {
      float height_scale = window_height / (float)image_height;
      float height_ratio = window_height * FULL_IMAGE_RATIO;
      *width_adjustment = (window_width - height_ratio)/(2.*height_scale);
      return 0;
    }
    // if narrower than desirable then use max width and have black bars on top
    // and bottom of frame
    float width_scale = window_width / (float)image_width;
    float width_ratio = window_width / FULL_IMAGE_RATIO;
    return (width_ratio - window_height)/(2.*width_scale);
  }
  if( ratio > MINIMAL_TOP_BOTTOM_RATIO ) {
    // Truncate the top and bottom borders as much as possible and put black
    // bars on the left and right borders to cover the remainder
    float height_scale = window_height / MINIMAL_HEIGHT;
    float height_ratio = window_height * MINIMAL_TOP_BOTTOM_RATIO;
    *width_adjustment = (window_width - height_ratio)/(2.*height_scale);
    return TOP_BORDER_HAIRCUT;
  } else if( ratio > FULL_IMAGE_RATIO ) {
    // Similar to above but need to appropriately scale the amount of border
    // truncation assuming that full width will be used
    float width_scale = window_width / (float)image_width;
    float height_pixels = window_width / FULL_IMAGE_RATIO;
    return (height_pixels - window_height) / (2.*width_scale);
  } else if( ratio < MINIMAL_LEFT_RIGHT_RATIO ) {
    // here we have maximum border truncation and black bars top and bottom
    float width_scale = window_width / MINIMAL_WIDTH;
    float width_ratio = window_width / MINIMAL_LEFT_RIGHT_RATIO;
    *width_adjustment = -SIDE_BORDER_HAIRCUT;
    return (width_ratio - window_height)/(2.*width_scale);
  }
  // here we have a little bit of truncation of the left and right borders and
  // leave the top and bottom borders alone
  float height_scale = window_height / (float)image_height;
  float width_pixels = window_height * FULL_IMAGE_RATIO;
  *width_adjustment = (window_width - width_pixels) / (2.*height_scale);
  return 0;
}

@implementation DisplayOpenGLView

static DisplayOpenGLView *instance = nil;

+(DisplayOpenGLView *) instance
{
  return instance;
}

-(IBAction) zoom:(id)sender
{
  NSSize size;

  switch( [sender tag] ) {
  case 1: /* 320x240 */
    size.width = 320;
    size.height = 240;
    break;
  case 2: /* 640x480 */
    size.width = 640;
    size.height = 480;
    break;
  case 3: /* 960x720 */
    size.width = 960;
    size.height = 720;
    break;
  case 4: /* 1280x960 */
    size.width = 1280;
    size.height = 960;
    break;
  case 5: /* 1600x1200 */
    size.width = 1600;
    size.height = 1200;
    break;
  case 0:
  default: /* Actual size */
    size.width = screenTex[0].image_width;
    size.height = screenTex[0].image_height;
  }

  [[self window] setContentSize:size];
}

-(void)setServer:(id)anObject
{
  proxy_emulator = [anObject retain];
  [[FuseController singleton] emulatorDidConnect];
}

-(id) initWithFrame:(NSRect)frameRect
{
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if( !device ) {
    NSLog( @"Metal is not supported on this system -- exiting" );
    exit( 1 );
  }

  if ( instance ) {
    /* Defensive: this branch only triggers if something tries to construct
       a second DisplayOpenGLView. The XIB instantiates this class once via
       -initWithCoder:, so it should not be reached in normal use. */
    [self dealloc];
    [device release];
    return instance;
  }

  self = [super initWithFrame:frameRect device:device];
  instance = self;

  buffered_screen_lock = [[NSLock alloc] init];
  real_emulator = [[Emulator alloc] init];

  /* MTKView retains its device internally; keep an unretained ivar
     for convenience. Balance MTLCreateSystemDefaultDevice's +1. */
  mtlDevice = device;
  [device release];

  self.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
  self.clearColor = MTLClearColorMake( 0.0, 0.0, 0.0, 1.0 );
  /* Use MTKView's built-in draw loop (a CADisplayLink under the hood)
     so the view redraws automatically every vsync, including during
     window resize and the fullscreen transition animation. Start paused
     so -drawInMTKView: cannot fire before -createTexture: has set up
     the screen MTLTextures; -displayLinkStart wakes the loop. */
  self.paused = YES;
  self.enableSetNeedsDisplay = NO;
  self.delegate = self;

  commandQueue = [mtlDevice newCommandQueue];

  id<MTLLibrary> library = [mtlDevice newDefaultLibrary];
  id<MTLFunction> vertFunc = [library newFunctionWithName:@"display_vertex"];
  id<MTLFunction> fragFunc = [library newFunctionWithName:@"display_fragment"];

  MTLRenderPipelineDescriptor *pipeDesc =
    [[MTLRenderPipelineDescriptor alloc] init];
  pipeDesc.vertexFunction = vertFunc;
  pipeDesc.fragmentFunction = fragFunc;
  pipeDesc.colorAttachments[0].pixelFormat = self.colorPixelFormat;

  NSError *pipeErr = nil;
  pipelineState = [mtlDevice newRenderPipelineStateWithDescriptor:pipeDesc
                                                            error:&pipeErr];
  [pipeDesc release];
  [vertFunc release];
  [fragFunc release];
  [library release];
  if( !pipelineState ) {
    NSLog( @"Failed to create Metal pipeline state: %@", pipeErr );
    exit( 1 );
  }

  greenCassette = [Texture alloc];
  redCassette = [Texture alloc];
  [self loadPicture: @"cassette" greenTex:greenCassette
                                   redTex:redCassette
                                  xOrigin:285
                                  yOrigin:220];
  greenMdr = [Texture alloc];
  redMdr = [Texture alloc];
  [self loadPicture: @"microdrive" greenTex:greenMdr
                                     redTex:redMdr
                                    xOrigin:264
                                    yOrigin:218];
  greenDisk = [Texture alloc];
  redDisk = [Texture alloc];
  [self loadPicture: @"plus3disk" greenTex:greenDisk
                                    redTex:redDisk
                                   xOrigin:243
                                   yOrigin:218];
  screenTexInitialised = NO;

  target_ratio = 4.0f/3.0f;

  NSPort *port1;
  NSPort *port2;
  NSArray *portArray;

  port1 = [NSPort port];
  port2 = [NSPort port];

  kitConnection = [[NSConnection alloc] initWithReceivePort:port1 sendPort:port2];
  [kitConnection setRootObject:self];

  [kitConnection enableMultipleThreads];

  /* Ports switched here */
  portArray = @[port2, port1];

  [NSThread detachNewThreadSelector:@selector(connectWithPorts:)
            toTarget:real_emulator withObject:portArray]; 

  currentScreenTex = 0;

  [self registerForDraggedTypes:[NSArray arrayWithObjects: NSFilenamesPboardType, nil]];

  return self;
}

-(void)dealloc
{
  if (view_lock)
    [view_lock release];
  view_lock = nil;

  if (buffered_screen_lock)
    [buffered_screen_lock release];
  buffered_screen_lock = nil;

  [pipelineState release];
  [commandQueue release];
  if( conversionBuffer ) {
    free( conversionBuffer );
    conversionBuffer = NULL;
    conversionBufferPixels = 0;
  }

  [super dealloc];
}

- (NSDragOperation)draggingEntered:(id <NSDraggingInfo>)sender {
  return NSDragOperationCopy;
}

- (BOOL)performDragOperation:(id <NSDraggingInfo>)sender {
  NSPasteboard *pboard;
  
  pboard = [sender draggingPasteboard];

  if ( [[pboard types] containsObject:NSFilenamesPboardType] ) {
    NSString *fileURL = [[NSURL URLFromPasteboard:pboard] path];
    [[FuseController singleton] openFile:[fileURL UTF8String]];
  }
  return YES;
}

-(void) awakeFromNib
{
  /* keep the window in the standard aspect ratio if the user resizes */
  [[self window] setContentAspectRatio:NSMakeSize(4.0,3.0)];

  /* Opt into AppKit native fullscreen so the green traffic-light
     button acts as a fullscreen toggle (the standard arrows icon)
     rather than a zoom. */
  [[self window] setCollectionBehavior:
    [[self window] collectionBehavior] |
    NSWindowCollectionBehaviorFullScreenPrimary];

  view_lock = [[NSLock alloc] init];
}

- (void)windowWillClose:(NSNotification *)notification
{
  [[self window] setDelegate:nil];
  [proxy_emulator stop];
  [proxy_emulator release];
  proxy_emulator = nil;
  [real_emulator release];
  real_emulator = nil;

  [redCassette release];
  redCassette = nil;
  [greenCassette release];
  greenCassette = nil;
  [redMdr release];
  redMdr = nil;
  [greenMdr release];
  greenMdr = nil;
  [redDisk release];
  redDisk = nil;
  [greenDisk release];
  greenDisk = nil;

  [self release];
}

- (void)windowDidResignKey:(NSNotification *)notification
{
  [proxy_emulator keyboardReleaseAll];
}

- (void)windowDidExitFullScreen:(NSNotification *)notification
{
  /* Release the emulator's mouse grab on fullscreen exit so a user who
     entered fullscreen with the cursor grabbed regains it. The entry side
     is deliberately not symmetric: auto-grabbing would hide the cursor
     and block the menu-bar-on-hover reveal that exposes Open, Preferences,
     and the rest of the menu in fullscreen. */
  if( ui_mouse_grabbed ) ui_mouse_grabbed = ui_mouse_release( 0 );
}

- (void)windowDidEnterFullScreen:(NSNotification *)notification
{
  [[self window] makeFirstResponder:self];
}

-(void) loadPicture: (NSString *) name
                      greenTex:(Texture*) greenTexture
                      redTex:(Texture*) redTexture
                      xOrigin:(int) x
                      yOrigin:(int) y
{
  NSString *filename;

  filename = [NSString stringWithFormat:@"%@_green", name];

  /* Colour first image green */
  (void)[greenTexture initWithImageFile:filename withXOrigin:x
                          withYOrigin:y device:mtlDevice];

  filename = [NSString stringWithFormat:@"%@_red", name];

  /* Colour second image red */
  (void)[redTexture initWithImageFile:filename withXOrigin:x
                        withYOrigin:y device:mtlDevice];
}

-(void) encodeIcon:(Texture*)iconTexture
           encoder:(id<MTLRenderCommandEncoder>)encoder
         contentX0:(float)contentX0
         contentY0:(float)contentY0
         contentX1:(float)contentX1
         contentY1:(float)contentY1
{
  Cocoa_Texture* texture = [iconTexture getTexture];

  /* Map pixel icon position to appropriate position on -1.0 to 1.0 canvas */
  float target_x1 = texture->image_xoffset * 2.0f / (float)DISPLAY_ASPECT_WIDTH
                    - 1.0f;
  float target_x2 = ( ( texture->image_xoffset + texture->image_width ) * 2.0f
                      / (float)DISPLAY_ASPECT_WIDTH ) - 1.0f;
  float target_y1 = 1.0f - texture->image_yoffset * 2.0f /
                    (float)DISPLAY_SCREEN_HEIGHT;
  float target_y2 = 1.0f - ( texture->image_yoffset + texture->image_height )
                    * 2.0f / (float)DISPLAY_SCREEN_HEIGHT;

  target_x1 = map_clip_coordinate( target_x1, contentX0, contentX1 );
  target_x2 = map_clip_coordinate( target_x2, contentX0, contentX1 );
  target_y1 = map_clip_coordinate( target_y1, contentY1, contentY0 );
  target_y2 = map_clip_coordinate( target_y2, contentY1, contentY0 );

  DisplayVertex verts[6];
  fill_quad( verts,
             target_x1, target_y1, target_x2, target_y2,
             0.0f, 0.0f, 1.0f, 1.0f );

  [encoder setVertexBytes:verts length:sizeof(verts) atIndex:0];
  [encoder setFragmentTexture:[iconTexture mtlTexture] atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
}

-(void) encodeIconOverlay:(id<MTLRenderCommandEncoder>)encoder
                contentX0:(float)contentX0
                contentY0:(float)contentY0
                contentX1:(float)contentX1
                contentY1:(float)contentY1
{
  switch( disk_state ) {
  case UI_STATUSBAR_STATE_ACTIVE:
    [self encodeIcon:greenDisk encoder:encoder contentX0:contentX0
           contentY0:contentY0 contentX1:contentX1 contentY1:contentY1];
    break;
  case UI_STATUSBAR_STATE_INACTIVE:
    [self encodeIcon:redDisk encoder:encoder contentX0:contentX0
           contentY0:contentY0 contentX1:contentX1 contentY1:contentY1];
    break;
  case UI_STATUSBAR_STATE_NOT_AVAILABLE:
    break;
  }

  switch( mdr_state ) {
  case UI_STATUSBAR_STATE_ACTIVE:
    [self encodeIcon:greenMdr encoder:encoder contentX0:contentX0
           contentY0:contentY0 contentX1:contentX1 contentY1:contentY1];
    break;
  case UI_STATUSBAR_STATE_INACTIVE:
    [self encodeIcon:redMdr encoder:encoder contentX0:contentX0
           contentY0:contentY0 contentX1:contentX1 contentY1:contentY1];
    break;
  case UI_STATUSBAR_STATE_NOT_AVAILABLE:
    break;
  }

  switch( tape_state ) {
  case UI_STATUSBAR_STATE_ACTIVE:
    [self encodeIcon:greenCassette encoder:encoder contentX0:contentX0
           contentY0:contentY0 contentX1:contentX1 contentY1:contentY1];
    break;
  case UI_STATUSBAR_STATE_INACTIVE:
  case UI_STATUSBAR_STATE_NOT_AVAILABLE:
    [self encodeIcon:redCassette encoder:encoder contentX0:contentX0
           contentY0:contentY0 contentX1:contentX1 contentY1:contentY1];
    break;
  }
}

/* MTKViewDelegate method. Called by MTKView's built-in CADisplayLink at
   the display's vsync. Uploads any pending dirty pixels, then encodes
   the render pass for the most-recently uploaded screen MTLTexture
   plus the activity icons if the statusbar is on, and presents. */
-(void) drawInMTKView:(MTKView *)view
{
  [self uploadDirtyToTexture];

  [view_lock lock];

  if( !screenTexInitialised ) {
    [view_lock unlock];
    return;
  }

  MTLRenderPassDescriptor *passDesc = view.currentRenderPassDescriptor;
  id<CAMetalDrawable> drawable = view.currentDrawable;
  if( !passDesc || !drawable ) {
    [view_lock unlock];
    return;
  }

  /* Letterbox the emulated 4:3 image inside the view's current bounds.
     In windowed mode the window's contentAspectRatio constraint keeps
     the view 4:3 and get_offset returns zero margins (no-op). In any
     fullscreen mode the view fills the display and get_offset produces
     pillarbox or letterbox margins as appropriate. get_offset returns
     the vertical (top/bottom) margin in pixels and writes the horizontal
     (left/right) margin into its out-parameter. */
  NSRect rect = [self bounds];
  float horizontal_margin = 0.0f;
  int vertical_margin =
    get_offset( rect.size.width, rect.size.height,
                screenTex[currentScreenTex].image_width,
                screenTex[currentScreenTex].image_height,
                &horizontal_margin );

  Cocoa_Texture *cur = &screenTex[currentScreenTex];
  float contentX0 = -1.0f;
  float contentY0 = 1.0f;
  float contentX1 = 1.0f;
  float contentY1 = -1.0f;

  if( !settings_current.full_screen_panorama ) {
    static const float FULL_IMAGE_RATIO = 4.0f / 3.0f;
    float viewRatio = rect.size.width / (float)rect.size.height;

    horizontal_margin = 0.0f;
    vertical_margin = 0;

    if( viewRatio > FULL_IMAGE_RATIO ) {
      float widthScale = FULL_IMAGE_RATIO / viewRatio;
      contentX0 = -widthScale;
      contentX1 = widthScale;
    } else if( viewRatio < FULL_IMAGE_RATIO ) {
      float heightScale = viewRatio / FULL_IMAGE_RATIO;
      contentY0 = heightScale;
      contentY1 = -heightScale;
    }
  }

  float fw = (float)cur->full_width;
  float fh = (float)cur->full_height;
  float u0 = (cur->image_xoffset - horizontal_margin) / fw;
  float u1 = (cur->image_width + cur->image_xoffset + horizontal_margin) / fw;
  float v0 = (cur->image_yoffset + vertical_margin) / fh;
  float v1 = (cur->image_height + cur->image_yoffset - vertical_margin) / fh;

  DisplayVertex mainVerts[6];
  fill_quad( mainVerts, contentX0, contentY0, contentX1, contentY1,
             u0, v0, u1, v1 );

  id<MTLCommandBuffer> cmdBuf = [commandQueue commandBuffer];
  id<MTLRenderCommandEncoder> encoder =
    [cmdBuf renderCommandEncoderWithDescriptor:passDesc];
  [encoder setRenderPipelineState:pipelineState];

  [encoder setVertexBytes:mainVerts length:sizeof(mainVerts) atIndex:0];
  [encoder setFragmentTexture:screenTexMTL[currentScreenTex] atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];

  if( settings_current.statusbar )
    [self encodeIconOverlay:encoder contentX0:contentX0 contentY0:contentY0
                  contentX1:contentX1 contentY1:contentY1];

  [encoder endEncoding];
  [cmdBuf presentDrawable:drawable];
  [cmdBuf commit];

  [view_lock unlock];
}

-(void) mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size
{
  /* The drawable resize is handled by MTKView itself; nothing to do
     beyond accepting the new size. The pixel art is sampled at whatever
     drawable resolution the view reports. */
}

-(void) destroyTexture
{
  if (!screenTexInitialised)
    return;

  [view_lock lock];

  [self displayLinkStop];

  for( unsigned int i = 0; i < MAX_SCREEN_BUFFERS; i++ )
  {
    [screenTexMTL[i] release];
    screenTexMTL[i] = nil;
    free( screenTex[i].pixels );
    screenTex[i].pixels = NULL;
    if( screenTex[i].dirty )
      pig_dirty_close( screenTex[i].dirty );
    screenTex[i].dirty = NULL;
  }
  screenTexInitialised = NO;
  [view_lock unlock];
}

-(void) createTexture:(Cocoa_Texture*)newScreen
{
  [view_lock lock];

  for( unsigned int i = 0; i < MAX_SCREEN_BUFFERS; i++ )
  {
    screenTex[i].full_width = newScreen->full_width;
    screenTex[i].full_height = newScreen->full_height;
    screenTex[i].image_width = newScreen->image_width;
    screenTex[i].image_height = newScreen->image_height;
    screenTex[i].image_xoffset = newScreen->image_xoffset;
    screenTex[i].image_yoffset = newScreen->image_yoffset;
    screenTex[i].pixels = calloc( screenTex[i].full_width * screenTex[i].full_height,
                                  sizeof(uint16_t) );
    if( !screenTex[i].pixels ) {
      NSLog( @"%s: couldn't create screenTex[%ud].pixels\n", fuse_progname,
             (unsigned int)i );
      [view_lock unlock];
      return;
    }
    screenTex[i].pitch = screenTex[i].full_width * sizeof(uint16_t);

    MTLTextureDescriptor *desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                   width:screenTex[i].full_width
                                  height:screenTex[i].full_height
                               mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageShaderRead;
    screenTexMTL[i] = [mtlDevice newTextureWithDescriptor:desc];
    if( !screenTexMTL[i] ) {
      NSLog( @"%s: couldn't create screenTexMTL[%ud]\n", fuse_progname,
             (unsigned int)i );
      [view_lock unlock];
      return;
    }
  }

  /* Reusable conversion buffer sized for one full frame. */
  size_t needed_pixels = (size_t)newScreen->full_width * newScreen->full_height;
  if( conversionBufferPixels < needed_pixels ) {
    free( conversionBuffer );
    conversionBuffer = malloc( needed_pixels * sizeof(uint32_t) );
    conversionBufferPixels = conversionBuffer ? needed_pixels : 0;
  }

  screenTexInitialised = YES;

  [self displayLinkStart];

  [view_lock unlock];
}

-(void) openFile:(const char *)filename
{
  [proxy_emulator openFile:filename];
}

-(void) snapOpen:(const char *)filename
{
  [proxy_emulator snapOpen:filename];
}

-(void) tapeOpen:(const char *)filename
{
  [view_lock lock];
  [proxy_emulator tapeOpen:filename];
  [view_lock unlock];
}

-(void) tapeWrite:(const char *)filename;
{
  [proxy_emulator tapeWrite:filename];
}

-(void) tapeTogglePlay
{
  [proxy_emulator tapeTogglePlay];
}

-(void) tapeToggleRecord
{
  [proxy_emulator tapeToggleRecord];
}

-(void) tapeRewind
{
  [proxy_emulator tapeRewind];
}

-(void) tapeClear
{
  [proxy_emulator tapeClear];
}

-(int) tapeClose
{
  return [proxy_emulator tapeClose];
}

-(void) tapeWindowInitialise
{
  [proxy_emulator tapeWindowInitialise];
}

-(void) cocoaBreak
{
  [proxy_emulator cocoaBreak];
}

-(void) pause
{
  [proxy_emulator pause];

  [self displayLinkStop];

  /* FIXME: Show paused status somehow */
}

-(void) unpause
{
  [proxy_emulator unpause];

  [self displayLinkStart];
}

-(void) reset
{
  [proxy_emulator reset];
}

-(void) hard_reset
{
  [proxy_emulator hard_reset];
}

-(void) nmi
{
  [proxy_emulator nmi];
}

-(int) checkMediaChanged
{
  return [proxy_emulator checkMediaChanged];
}

-(void) diskInsertNew:(int)which
{
  [proxy_emulator diskInsertNew:which];
}

-(void) diskInsert:(const char *)filename inDrive:(int)which
{
  [proxy_emulator diskInsert:filename inDrive:which];
}

-(void) diskEject:(int)drive
{
  [proxy_emulator diskEject:drive];
}

-(void) diskSave:(int)drive saveAs:(bool)saveas
{
  [proxy_emulator diskSave:drive saveAs:saveas];
}

//-(int) diskWrite:(int)drive saveAs:(bool)saveas
//{
//  [proxy_emulator diskWrite:drive saveAs:saveas];
//}

-(void) diskFlip:(int)which side:(int)flip
{
  [proxy_emulator diskFlip:which side:flip];
}

-(void) diskWriteProtect:(int)which protect:(int)write
{
  [proxy_emulator diskWriteProtect:which protect:write];
}

-(void) snapshotWrite:(const char *)filename
{
  [proxy_emulator snapshotWrite:filename];
}

-(void) screenshotScrRead:(const char *)filename
{
  [proxy_emulator screenshotScrRead:filename];
}

-(void) screenshotScrWrite:(const char *)filename
{
  [proxy_emulator screenshotScrWrite:filename];
}

-(void) screenshotWrite:(const char *)filename
{
  [proxy_emulator screenshotWrite:filename];
}

-(void) profileStart
{
  [proxy_emulator profileStart];
}

-(void) profileFinish:(const char *)filename
{
  [proxy_emulator profileFinish:filename];
}

-(void) settingsSave
{
  [proxy_emulator settingsSave];
}

-(void) settingsResetDefaults
{
  [proxy_emulator settingsResetDefaults];
}

-(void) joystickToggleKeyboard
{
  [proxy_emulator joystickToggleKeyboard];
}

-(void) keyboardToggleRecreatedZXSpectrum
{
  [proxy_emulator keyboardToggleRecreatedZXSpectrum];
}

-(void) keyboardToggleArrowsShifted
{
  [proxy_emulator keyboardToggleArrowsShifted];
}

-(int) rzxStartPlayback:(const char *)filename
{
  return [proxy_emulator rzxStartPlayback:filename];
}

-(void) rzxInsertSnap
{
  [proxy_emulator rzxInsertSnap];
}

-(void) rzxRollback
{
  [proxy_emulator rzxRollback];
}

-(int) rzxStartRecording:(const char *)filename embedSnapshot:(int)flag
{
  return [proxy_emulator rzxStartRecording:filename embedSnapshot:flag];
}

-(void) rzxStop
{
  [proxy_emulator rzxStop];
}

-(int) rzxContinueRecording:(const char *)filename
{
  return [proxy_emulator rzxContinueRecording:filename];
}

-(int) rzxFinaliseRecording:(const char *)filename
{
  return [proxy_emulator rzxFinaliseRecording:filename];
}

-(void)movieStartRecording:(const char *)filename
{
  [proxy_emulator movieStartRecording:filename];
}

-(void)movieTogglePause
{
  [proxy_emulator movieTogglePause];
}

-(void)movieStop
{
  [proxy_emulator movieStop];
}

-(void) didaktik80Snap
{
  [proxy_emulator didaktik80Snap];
}

-(void) multifaceRedButton
{
  [proxy_emulator multifaceRedButton];
}

-(void) if1MdrNew:(int)drive
{
  [proxy_emulator if1MdrNew:drive];
}

-(void) if1MdrInsert:(const char *)filename inDrive:(int)drive
{
  [proxy_emulator if1MdrInsert:filename inDrive:drive];
}

-(void) if1MdrCartEject:(int)drive
{
  [proxy_emulator if1MdrCartEject:drive];
}

-(void) if1MdrCartSave:(int)drive saveAs:(bool)saveas
{
  [proxy_emulator if1MdrCartSave:drive saveAs:saveas];
}

-(void) if1MdrWriteProtect:(int)w inDrive:(int)drive
{
  [proxy_emulator if1MdrWriteProtect:w inDrive:drive];
}

-(int) if2Insert:(const char *)filename
{
  return [proxy_emulator if2Insert:filename];
}

-(void) if2Eject
{
  [proxy_emulator if2Eject];
}

-(int) dckInsert:(const char *)filename
{
  return [proxy_emulator dckInsert:filename];
}

-(void) dckEject
{
  [proxy_emulator dckEject];
}

-(void) psgStart:(const char *)psgfile
{
  [proxy_emulator psgStart:psgfile];
}

-(void) psgStop
{
  [proxy_emulator psgStop];
}

-(int) simpleideInsert:(const char *)filename inUnit:(libspectrum_ide_unit)unit
{
  return [proxy_emulator simpleideInsert:filename inUnit:unit];
}

-(int) simpleideCommit:(libspectrum_ide_unit)unit
{
  return [proxy_emulator simpleideCommit:unit];
}

-(int) simpleideEject:(libspectrum_ide_unit)unit
{
  return [proxy_emulator simpleideEject:unit];
}

-(int) zxataspInsert:(const char *)filename inUnit:(libspectrum_ide_unit)unit
{
  return [proxy_emulator zxataspInsert:filename inUnit:unit];
}

-(int) zxataspCommit:(libspectrum_ide_unit)unit
{
  return [proxy_emulator zxataspCommit:unit];
}

-(int) zxataspEject:(libspectrum_ide_unit)unit
{
  return [proxy_emulator zxataspEject:unit];
}

-(int) zxcfInsert:(const char *)filename
{
  return [proxy_emulator zxcfInsert:filename];
}

-(int) zxcfCommit
{
  return [proxy_emulator zxcfCommit];
}

-(int) zxcfEject
{
  return [proxy_emulator zxcfEject];
}

-(int) divideInsert:(const char *)filename inUnit:(libspectrum_ide_unit)unit
{
  return [proxy_emulator divideInsert:filename inUnit:unit];
}

-(int) divideCommit:(libspectrum_ide_unit)unit
{
  return [proxy_emulator divideCommit:unit];
}

-(int) divideEject:(libspectrum_ide_unit)unit
{
  return [proxy_emulator divideEject:unit];
}

-(int) divmmcInsert:(const char *)filename
{
  return [proxy_emulator divmmcInsert:filename];
}

-(int) divmmcCommit
{
  return [proxy_emulator divmmcCommit];
}

-(int) divmmcEject
{
  return [proxy_emulator divmmcEject];
}

-(int) zxmmcInsert:(const char *)filename
{
  return [proxy_emulator zxmmcInsert:filename];
}

-(int) zxmmcCommit
{
  return [proxy_emulator zxmmcCommit];
}

-(int) zxmmcEject
{
  return [proxy_emulator zxmmcEject];
}

-(void) setDiskState:(NSNumber*)state
{
  disk_state = [state unsignedCharValue];
  [[FuseController singleton] setDiskState:state];
}

-(void) setTapeState:(NSNumber*)state
{
  tape_state = [state unsignedCharValue];
  [[FuseController singleton] setTapeState:state];
}

-(void) setMdrState:(NSNumber*)state
{
  mdr_state = [state unsignedCharValue];
  [[FuseController singleton] setMdrState:state];
}

-(ui_confirm_save_t) confirmSave:(NSString*)theMessage
{
  return [[FuseController singleton] confirmSave:theMessage];
}

-(int) confirm:(NSString*)theMessage
{
  return [[FuseController singleton] confirm:theMessage];
}

-(int) tapeWrite
{
  return [[FuseController singleton] tapeWrite];
}

-(int) diskWrite:(int)which saveAs:(bool)saveas
{
  return [[FuseController singleton] diskWrite:which saveAs:saveas];
}

-(int) if1MdrWrite:(int)which saveAs:(bool)saveas
{
  return [[FuseController singleton] if1MdrWrite:which saveAs:saveas];
}

-(ui_confirm_joystick_t) confirmJoystick:(libspectrum_joystick)type inputs:(int)theInputs
{
  return [[FuseController singleton] confirmJoystick:type inputs:theInputs];
}

-(void) debuggerActivate
{
  [[DebuggerController singleton] debugger_activate:nil];
}

-(void) mouseMoved:(NSEvent *)theEvent
{
  [proxy_emulator mouseMoved:theEvent];
}

-(void) mouseDown:(NSEvent *)theEvent
{
  [[self window] makeFirstResponder:self];
  [proxy_emulator mouseDown:theEvent];
}

-(void) mouseUp:(NSEvent *)theEvent
{
  [proxy_emulator mouseUp:theEvent];
}

-(void) rightMouseDown:(NSEvent *)theEvent
{
  [proxy_emulator rightMouseDown:theEvent];
}

-(void) rightMouseUp:(NSEvent *)theEvent
{
  [proxy_emulator rightMouseUp:theEvent];
}

-(void) otherMouseDown:(NSEvent *)theEvent
{
  [proxy_emulator otherMouseDown:theEvent];
}

-(void) otherMouseUp:(NSEvent *)theEvent
{
  [proxy_emulator otherMouseUp:theEvent];
}

-(void) flagsChanged:(NSEvent *)theEvent
{
  [proxy_emulator flagsChanged:theEvent];
}

-(void) keyDown:(NSEvent *)theEvent
{
  [proxy_emulator keyDown:theEvent];
}

-(void) keyUp:(NSEvent *)theEvent
{
  [proxy_emulator keyUp:theEvent];
}

-(BOOL) acceptsFirstResponder
{
  return YES;
}

-(BOOL) becomeFirstResponder
{
  return YES;
}

-(BOOL) resignFirstResponder
{
  return YES;
}

-(BOOL) isFlipped
{
  return YES;
}

-(BOOL) windowShouldClose:(id)window
{
  /* Funnel the red-button / Cmd+W close through the same exit path as Cmd+Q
     so the "Exit Fuse?" confirm and the unsaved-media check live in one
     place (FuseController -applicationShouldTerminate:). Returning NO lets
     terminate: take over: if the user confirms, AppKit closes every window
     itself; if the user cancels, the window stays open. */
  [NSApp terminate:self];
  return NO;
}

-(void) uploadDirtyToTexture
{
  int i;
  PIG_dirtytable *workdirty = NULL;

  // Is it possible that while waiting for a lock the emulator is stopped?
  // or already holds the lock? If so give up on updating the frame rather
  // than deadlock on getting the lock - may mean that we miss some screen
  // updates if we are invoked while the buffered screen is being updated
  if( !buffered_screen_lock || [buffered_screen_lock tryLock] == NO ) {
    return;
  }

  /* Check if buffered_screen.dirty is initialized - it might be NULL during initialization or cleanup */
  if( !buffered_screen.dirty || buffered_screen.dirty->count == 0 ) {
    [buffered_screen_lock unlock];
    return;
  }

  /* Cover the screen-texture swap and the GPU upload. */
  [view_lock lock];

  if( !screenTexInitialised ) {
    [view_lock unlock];
    [buffered_screen_lock unlock];
    return;
  }

  if( screenTex[currentScreenTex].dirty )
    pig_dirty_copy( &workdirty, screenTex[currentScreenTex].dirty );

  currentScreenTex = !currentScreenTex;

  pig_dirty_copy( &screenTex[currentScreenTex].dirty, buffered_screen.dirty );

  if( workdirty )
    pig_dirty_merge( workdirty, screenTex[currentScreenTex].dirty );
  else
    pig_dirty_copy( &workdirty, screenTex[currentScreenTex].dirty );

  /* Sync the CPU shadow buffer against the source for every rect that
     may have changed since this buffer was last current. */
  for( i = 0; i < workdirty->count; ++i )
    copy_area( &screenTex[currentScreenTex], &buffered_screen,
               workdirty->rects + i );

  buffered_screen.dirty->count = 0;

  pig_dirty_close( workdirty );

  /* Convert the entire CPU-side buffer from 16-bit BGR5A1 to 32-bit
     BGRA8 in the reusable conversion buffer, then upload the full
     texture in one call. This matches the legacy OpenGL path
     (`glTexSubImage2D` over `full_width` x `full_height` every frame)
     and keeps the GPU side in sync regardless of which regions the
     emulator core flagged as dirty -- the dirty list in `dirty.c` is
     used for CPU-side copy_area accounting, not for tracking what
     the GPU texture needs uploaded. */
  Cocoa_Texture *cur = &screenTex[currentScreenTex];
  int src_pitch_words = cur->pitch / (int)sizeof(uint16_t);
  size_t total_pixels = (size_t)cur->full_width * (size_t)cur->full_height;
  if( total_pixels > conversionBufferPixels ) {
    free( conversionBuffer );
    conversionBuffer = malloc( total_pixels * sizeof(uint32_t) );
    conversionBufferPixels = conversionBuffer ? total_pixels : 0;
  }
  if( conversionBuffer ) {
    convert_bgr5a1_to_bgra8( (const uint16_t *)cur->pixels, src_pitch_words,
                             conversionBuffer,
                             cur->full_width, cur->full_height );

    MTLRegion region =
      MTLRegionMake2D( 0, 0, cur->full_width, cur->full_height );
    [screenTexMTL[currentScreenTex]
      replaceRegion:region
        mipmapLevel:0
          withBytes:conversionBuffer
        bytesPerRow:(NSUInteger)cur->full_width * sizeof(uint32_t)];
  }

  [view_lock unlock];
  [buffered_screen_lock unlock];
}

-(void) windowChangedScreen:(NSNotification*)inNotification
{
  /* CADisplayLink follows the view's screen automatically. Kept as a
     stub so any existing notification wiring continues to work. */
}

/* -[MTKView setPaused:] mutates AppKit/MetalKit state that lives on the
   main thread. -createTexture: and -destroyTexture run on the emulator
   worker thread via uidisplay_init/uidisplay_end, so hop to main before
   toggling self.paused. -performSelectorOnMainThread: retains the
   receiver until the selector runs, which avoids a use-after-free if the
   view is dealloc'd before a pending hop fires. Direct invocation when
   already on main keeps the common case (UI-driven pause/unpause)
   synchronous. */
-(void) _setPausedMain:(NSNumber *)paused
{
  self.paused = [paused boolValue];
}

-(void) displayLinkStop
{
  if( [NSThread isMainThread] ) {
    self.paused = YES;
  } else {
    [self performSelectorOnMainThread:@selector(_setPausedMain:)
                           withObject:@YES
                        waitUntilDone:NO];
  }
}

-(void) displayLinkStart
{
  if( [NSThread isMainThread] ) {
    self.paused = NO;
  } else {
    [self performSelectorOnMainThread:@selector(_setPausedMain:)
                           withObject:@NO
                        waitUntilDone:NO];
  }
}

@end

@implementation NSWindow (FuseChildPinning)

- (void)pinAsChildOf:(NSWindow *)parent
{
  if( !parent || parent == self ) return;
  /* Without FullScreenAuxiliary, adding self as a child of a fullscreen
     parent forces AppKit to exit fullscreen so the two can coexist on
     the regular desktop. With it, both windows share the parent's
     fullscreen Space. Harmless when the parent is windowed. */
  [self setCollectionBehavior:
    [self collectionBehavior] |
    NSWindowCollectionBehaviorFullScreenAuxiliary];
  NSWindow *currentParent = [self parentWindow];
  if( currentParent == parent ) return;
  if( currentParent ) {
    [currentParent removeChildWindow:self];
  }
  [parent addChildWindow:self ordered:NSWindowAbove];
}

- (void)unpinFromParent
{
  NSWindow *parent = [self parentWindow];
  if( parent ) {
    [parent removeChildWindow:self];
  }
}

@end
