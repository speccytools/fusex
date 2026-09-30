---
title: DivMMC Emulation
description: This section describes the DivMMC interface emulation in Fuse.
order: 105
group: Hard Disk Interfaces
---

The DivMMC is a MMC interface for the Spectrum. Originally designed by
Alessandro Dorigatti for the V6Z80P+ FPGA board as the fusion of DivIDE and
ZXMMC+ interfaces, later assembled as an interface for real spectrums by Mario
Prato. Currently there are variants with different RAM size, one/two memory
cards slots, optional kempston jostick, etc.

The interface can be activated via the *DivMMC interface* option from the
[Peripherals preferences](peripherals.html) dialog, and the state of its EEPROM
write protect jumper controlled via the *DivMMC write protect* option.

An 8192-byte raw DivMMC ROM can be loaded at startup with
`--divmmc-rom /path/to/rom.bin`.

If you're going to be using the DivMMC, you'll need to load the
[ESXDOS firmware](http://www.esxdos.org/) or use the ZX Spectrum +3e ROMs by
Garry Lancaster.

You'll also need a HDF image to store the contents of the memory card. There are
several tools to create and manipulate this file format, e.g.,
[hdfmonkey](https://github.com/gasman/hdfmonkey).
