Fuse for Windows
================

This is a binary distribution of Fuse (the Free Unix Spectrum Emulator) for
Windows. For an introduction to Fuse, see README.txt. For more detailed
documentation, read fuse.html.

Requirements
------------

A supported 32-bit or 64-bit Windows system for the bundled 32-bit build.

Libraries
---------

The package includes the runtime DLLs needed to run Fuse. No separate MSYS2
installation is required. Use the bundled DLLs rather than replacing them
with libraries from another installation.
Depending on the selected UI and enabled features, the package may include
DLLs from these components:

    bzip2
    libgcrypt
    libgpg-error
    libpng
    libspectrum
    libstdc++
    libxml2
    libiconv
    gettext runtime / libintl
    winpthreads
    xz / liblzma
    zlib
    SDL2

See LICENSES.txt for copyright and license details.

For build and packaging instructions, see INSTALL in the Fuse source tree.
MSYS2 installation and environment documentation: https://www.msys2.org/
