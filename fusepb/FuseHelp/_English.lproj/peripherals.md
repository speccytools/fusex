---
title: Peripherals Preferences
description: This section describes the Fuse peripherals preferences dialog.
order: 30
group: Fuse Preferences
---

The Peripherals pane of Fuse preferences lets you configure the peripherals
which Fuse will consider to be attached to the emulated machines.

## Mass storage/ROM interface options

OPTION | DESCRIPTION
:--- | :---
*None* | If this option is selected, Fuse will not emulate any mass storage device (except for the integrated disk devices on the +3, Pentagon and Scorpion).
*Beta 128* | If this option is selected, Fuse will emulate the Beta 128 interface, which provides a floppy drive interface. See the [Beta 128 Emulation](trdos.html) section for more details. Beta 128 emulation is enabled for the Pentagon and Scorpion machines regardless of this option.
*Currah µSource* | If this option is selected, Fuse will emulate a Currah µSource interface. See the [World of Spectrum Infoseek web page](http://www.worldofspectrum.org/infoseekid.cgi?id=1000080) for the manual. The required ROM file is not supplied with Fuse for macOSX and so must be installed before the interface can be used. The expected file is an 8KB dump of the interface ROM named usource.rom and placed in the Fuse packages Contents/Resources directory.
*DISCiPLE* | If this option is selected, Fuse will emulate the DISCiPLE interface, which provides both a floppy drive and a printer interface. See the [DISCiPLE Emulation](disciple.html) section for more details.
*Didaktik 80 interface* | If this option is selected, Fuse will emulate the Didaktik 80 (or Didaktik 40) interface.  See the [Didaktik 80 Emulation](didaktik80.html) section for more details.
*DivIDE interface* | If this option is selected, Fuse will emulate the DivIDE interface, which provides both additional RAM and a IDE interface. See the [DivIDE Emulation](divide.html) section for more details.
*DivMMC interface* | If this option is selected, Fuse will emulate the DivMMC interface. See the [DIVMMC Emulation](divmmc.html) section for more details.
*Interface I* | If this option is selected, Fuse will emulate the Sinclair Interface I, and allow microdrive cartridges to be inserted and removed via the *Media > Microdrive* menus.
*Opus Discovery* | If this option is selected, Fuse will emulate the Opus Discovery interface, which provides both a floppy drive and a printer interface. See the [Opus Discovery Emulation](opus.html) section for more details.
*+D interface* | If this option is selected, Fuse will emulate the +D interface, which provides both a floppy drive and a printer interface. See the [+D Emulation](plusd.html) section for more details.
*Simple 8-bit IDE* | If this option is selected, Fuse will emulate the simple 8-bit IDE interface as used by the Spectrum +3e, and allow hard disks to be connected and disconnected via the *Media > IDE* menu.
*Spectranet* | If this option is selected, Fuse will emulate the Spectranet interface, which provides an ethernet interface for the Spectrum. See the [Spectranet Emulation](spectranet.html) section for more details.
*ZXATASP interface* | If this option is selected, Fuse will emulate the ZXATASP interface, which provides both additional RAM and an IDE interface. See the [ZXATASP and ZXCF Emulation](zxatasp.html) section for more details.
*ZXCF interface* | If this option is selected, Fuse will emulate the ZXCF interface, which provides both additional RAM and a CompactFlash interface. See the [ZXATASP and ZXCF Emulation](zxatasp.html) section for more details.
*ZXMMC interface* | If this option is selected, Fuse will emulate the ZXMMC interface. Available for +2A, +3 and +3e machines.

<br>
### +3 options

See the [Weak Disk Data](weak.html) section for more details.

OPTION | DESCRIPTION
:--- | :---
*+3 Detect Speedlock* | Specify whether the +3 drives try to detect Speedlock protected disks, and emulate 'weak' sectors. If the disk image file (EDSK or UDI) contains weak sector data, than Speedlock detection is automatically omitted.

<br>
### Beta 128 options

See the [Beta 128 Emulation](trdos.html) section for more details.

OPTION | DESCRIPTION
:--- | :---
*Beta autoboot in 48K* | When Beta 128 emulation is enabled and a 48K or TC2048 machine is being emulated, this option controls whether the machine boots directly into the TR-DOS system.

<br>
### DivIDE options

See the [DivIDE Emulation](divide.html) section for more details.

OPTION | DESCRIPTION
:--- | :---
*DivIDE write protect* | This option controls the state of the DivIDE EEPROM write protect jumper (E).

<br>
### DivMMC options

See the [DivMMC Emulation](divmmc.html) section for more details.

OPTION | DESCRIPTION
:--- | :---
*DivMMC write protect* | This option controls the state of the DivMMC write protection jumper that prevents flashing the EEPROM chip.

<br>
### Interface I options

OPTION | DESCRIPTION
:--- | :---
*MDR cartridge len* | This option controls the number of blocks in a new microdrive cartridge. If the value smaller than 10 or greater than 254 Fuse assumes 10 or 254. Average real capacity is around 180 blocks (90 Kb).

<br>
### Spectranet options

See the [Spectranet Emulation](spectratnet.html) section for more details.

OPTION | DESCRIPTION
:--- | :---
*Spectranet disable* | This option controls the state of the Spectranet automatic page-in jumper (J2).

<br>
### ZXATASP options

See the [ZXATASP and ZXCF Emulation](zxatasp.html) section for more details.

OPTION | DESCRIPTION
:--- | :---
*ZXATASP upload* | This option controls the state of the ZXATASP upload jumper.
*ZXATASP write protect* | This option controls the state of the ZXATASP write protect jumper.

<br>
### ZXCF options

See the [ZXATASP and ZXCF Emulation](zxatasp.html) section for more details.

OPTION | DESCRIPTION
:--- | :---
*ZXCF upload* | This option controls the state of the ZXCF upload jumper.

<br>
### Printer options

See the [Printer Emulation](printer.html) section for more details.

OPTION | DESCRIPTION
:--- | :---
*Emulate printers* | Specify whether emulation should include a printer.
*Emulate ZX Printer* | If this option is selected, Fuse will emulate the ZX Printer.
*Graphic Output File* | Specify the file name and location for the printer graphics output file.
*Text Output File* | Specify the file name and location for the printer text output file.

<br>
### External sound interface options

OPTION | DESCRIPTION
:--- | :---
*None* | If this option is selected, Fuse will not emulate any external sound interface.
*Fuller* | If this option is selected, Fuse will emulate a Fuller Box AY sound and joystick interface. This emulation is only available for the 16k, 48k and TC2048 machines.
*Melodik* | If this option is selected, Fuse will emulate a Melodik AY sound interface. These interfaces and many similar ones were produced to make the 48K Spectrum compatible with the same AY music as the 128K Spectrum. This emulation is only  available  for  the  16k,  48k  and  TC2048 machines.
*SpecDrum* | If this option is selected, Fuse will emulate a Cheetah SpecDrum sound interface.  See the [World of Spectrum Infoseek web page](http://www.worldofspectrum.org/infoseekid.cgi?id=1000062) for manuals, software and more. This emulation is only available for the 48k, 128k and TC2048 machines.
*Covox* | If this option is selected, Fuse will emulate a Covox digital sound interface. This emulation is only available for the Pentagon, Pentagon 512k, Pentagon 1024k and Scorpion machines. The Pentagon variants use port 0xfb and the Scorpion version uses port 0xdd.

<br>
### Romantic Robot Multiface options

OPTION | DESCRIPTION
:--- | :---
*Multiface One* | If this option is selected, Fuse will emulate the Multiface One. Available for 16K, 48K and Timex TC2048 machines. The required ROM file is not supplied with Fuse for macOSX and so must be installed before the interface can be used. The expected file is an 8KB dump of the interface ROM named mf1.rom and placed in the Fuse packages Contents/Resources directory.
*Multiface 128* | If this option is selected, Fuse will emulate the Multiface 128. Available for 16K, 48K, Timex TC2048, 128K, +2 and SE machines. The required ROM file is not supplied with Fuse for macOSX and so must be installed before the interface can be used. The expected file is an 8KB dump of the interface ROM named mf128.rom and placed in the Fuse packages Contents/Resources directory.
*Multiface 3* | If this option is selected, Fuse will emulate the Multiface 3. Available for +2A, +3 and +3e machines. The required ROM file is not supplied with Fuse for macOSX and so must be installed before the interface can be used. The expected file is an 8KB dump of the interface ROM named mf3.rom and placed in the Fuse packages Contents/Resources directory.
*Multiface One Stealth* | This option controls the 'invisible' or 'stealth' mode of Multiface One, as the physical switch on the side of the interface.

<br>
### General Sound options

General Sound is a sound card built around a second Z80 with its own ROM, its own RAM and four 8-bit DACs, mixed to a stereo output with the first two channels on the left and the other two on the right. It was widely used on the Russian scene, and a number of demos and games have soundtracks written for it.

OPTION | DESCRIPTION
:--- | :---
*General Sound* | If this option is selected, Fuse will emulate a General Sound card. It is an expansion-bus device and can be fitted alongside any other sound interface. Its ports are the ones the real card used, which the DivIDE, Multiface 128, Multiface 3 and DISCiPLE interfaces also decode; enabling one of those at the same time will interfere with it, as it would on real hardware. Fitting the card makes the sound output stereo even with AY stereo separation set to None.
*RAM* | The amount of memory fitted to the card. Cards were built with anywhere from 512KB to 4MB. Leave this at 2048KB unless a particular program asks for something else: the card's firmware sizes memory by a method that cannot see a smaller card, and will place sample data by the size it expects rather than the size fitted.
*ROM* | The card's firmware. A 512KB image named gs.rom is supplied in the Fuse packages Contents/Resources directory and is used by default; the Choose button will point the card at any other 512KB image instead. Fuse will report an error and leave the card inactive if the file is missing or the wrong size.
