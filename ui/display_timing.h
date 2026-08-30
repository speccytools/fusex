/* display_timing.h: optional display timing instrumentation */

#ifndef FUSE_DISPLAY_TIMING_H
#define FUSE_DISPLAY_TIMING_H

void display_timing_init( const char *backend );
void display_timing_input_begin( void );
void display_timing_input_end( void );
void display_timing_area( int width, int height );
void display_timing_scaler_begin( void );
void display_timing_scaler_end( void );
void display_timing_presentation_begin( void );
void display_timing_presentation_end( void );
void display_timing_paint_begin( void );
void display_timing_paint_end( void );
void display_timing_frame_end( void );

#endif
