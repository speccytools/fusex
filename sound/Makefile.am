## Process this file with automake to produce Makefile.in
## Copyright (c) 2004 Philip Kendall
## Copyright (c) 2015 Sergio Baldoví

## This program is free software; you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation; either version 2 of the License, or
## (at your option) any later version.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License along
## with this program; if not, write to the Free Software Foundation, Inc.,
## 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
##
## Author contact information:
##
## E-mail: philip-fuse@shadowmagic.org.uk

fuse_SOURCES += sound/ay_engine.c sound/blipbuffer.c sound/dc_filter.c \
                sound/output_mixer.c sound/source_synths.c \
                sound/speaker_filter.c sound/tv_filter.c sound/ula_filter.c

EXTRA_fuse_SOURCES += \
                      sound/alsasound.c sound/alsa_config.c \
                      sound/aosound.c \
                      sound/coreaudiosound.c \
                      sound/dxsound.c \
                      sound/hpsound.c \
                      sound/nullsound.c \
                      sound/osssound.c \
                      sound/pulsesound.c \
                      sound/sdl2sound.c \
                      sound/sfifo.c \
                      sound/sunsound.c \
                      sound/wiisound.c \
                      sound/win32sound.c

noinst_HEADERS += \
                  sound/ay_engine.h \
                  sound/alsa_pacing.h \
                  sound/alsa_config.h \
                  sound/audio_timeline.h \
                  sound/audio_controller.h \
                  sound/audio_pacing.h \
                  sound/audio_progress.h \
                  sound/blipbuffer.h \
                  sound/dc_filter.h \
                  sound/output_mixer.h \
                  sound/silence_noise.h \
                  sound/pcm_fifo.h \
                  sound/speaker_filter.h \
                  sound/sfifo.h \
                  sound/source_synths.h \
                  sound/tv_filter.h \
                  sound/ula_filter.h

fuse_DEPENDENCIES += $(SOUND_LIBADD)
fuse_LDADD += $(SOUND_LIBS) $(SOUND_LIBADD)
