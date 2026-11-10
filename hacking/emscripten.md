# Building Fuse for Emscripten

You can build Fuse as a single-threaded WebAssembly application with the SDL 2
Widget UI. This build shows the Fuse interface in a browser canvas.
It does not include these features:

- A browser file picker.
- Storage that keeps data after you reload the page.
- Network access.
- A JavaScript control API.

## C11 atomics and threads

Fuse and libspectrum use C11 atomic operations. These operations do not require
threads in this build. Emscripten supports them without `-pthread` or shared
WebAssembly memory. Fuse's network thread is disabled.

The libspectrum option `--with-fake-glib` enables internal memory allocation
locks. These locks use `atomic_flag` operations with acquire/release ordering.
They do not require a worker thread.

Do not add `-pthread`. It enables shared memory and a pthread runtime.
The browser then requires `SharedArrayBuffer` and cross-origin isolation.
This Fuse build does not need these features.

## Tested toolchain

The local build and publication workflow target Emscripten 6.0.12.

Install and activate the SDK with emsdk. Run these commands in the directory
where you want to install emsdk:

```sh
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install 6.0.12
./emsdk activate 6.0.12
. ./emsdk_env.sh
```

Put clean `libspectrum` and `fuse` source directories in the same parent
directory. Use separate directories for build files and installed libraries.
This keeps host libraries separate from WebAssembly libraries.

Keep `--host=wasm32-unknown-emscripten` in the commands. This option selects
WebAssembly instead of the host system for Libtool.

The message `cross_compiling=no` is not an error. Node can run the configure
test programs.

## Prepare the sources

Install these tools on the build host:

- A C compiler and make.
- Autoconf, Automake, and Libtool.
- pkg-config and Perl.
- Flex and Bison.
- Python 3.

Do not use host SDL or libspectrum libraries for the WebAssembly build.

For Git source directories, generate the configure scripts in both directories.
Run these commands from their parent directory:

```sh
(cd libspectrum && ./autogen.sh)
(cd fuse && ./autogen.sh)
```

Release archives already contain the configure scripts.

Use clean source directories and separate build directories. Do not reuse host
build files. Do not reuse object files from a different SDK or threading mode.

## Build libspectrum

Start in the parent directory that contains `libspectrum` and `fuse`.

```sh
mkdir -p build/wasm/libspectrum build/wasm/prefix
cd build/wasm/libspectrum

emconfigure ../../../libspectrum/configure \
  --host=wasm32-unknown-emscripten \
  --prefix="$(cd ../prefix && pwd)" \
  --disable-shared --enable-static \
  --with-zlib --without-bzip2 --without-libgcrypt \
  --with-wav-backend=pcm \
  --with-fake-glib \
  CFLAGS='-O2' LDFLAGS='-sUSE_ZLIB'
emmake make -j4
emmake make install
```

## Build Fuse

Return to the parent directory that contains `libspectrum` and `fuse`.
Then run these commands:

```sh
mkdir -p build/wasm/fuse
cd build/wasm/fuse
PREFIX="$(cd ../prefix && pwd)"

EM_PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" \
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" \
LIBSPECTRUM_CFLAGS="-I$PREFIX/include" \
LIBSPECTRUM_LIBS="$PREFIX/lib/libspectrum.a" \
SDL2_CFLAGS='-sUSE_SDL=2' \
SDL2_LIBS='-sUSE_SDL=2' \
emconfigure ../../../fuse/configure \
  --host=wasm32-unknown-emscripten \
  --with-sdl --without-x \
  --without-pthread --disable-sockets --without-joystick \
  --with-zlib --without-png --without-libxml2 \
  --disable-desktop-integration \
  CFLAGS='-O2' LDFLAGS='-sUSE_ZLIB'

EM_PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" \
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" \
emmake make -j4
```

Start an HTTP server in the Fuse build directory. Do not open the HTML file
with a `file://` URL.

```sh
python3 -m http.server 8000
```

1. Open `http://localhost:8000/fuse.html` in a browser.
2. Click the canvas to enable audio.
3. Press F1 to open the Widget menu.

The browser can keep audio stopped until you click the page.

This single-threaded build does not require cross-origin isolation headers.

The build produces these files. Keep them together when you publish the build:

- `fuse.html`
- `fuse.js`
- `fuse.wasm`
- `fuse.data`

`fuse.data` contains the ROMs and UI files from the normal Fuse build.
It does not contain a demonstration tape or a special configuration file.
Fuse starts with its default settings.

## Manually publishing to GitHub Pages

Use `.github/workflows/build_emscripten_pages.yml` to build and publish the web
version. You must start this workflow manually. A push, tag, or schedule does
not start it.

Before the first run:

1. In the GitHub repository's **Settings → Pages**, select **GitHub Actions** as
   the build/deployment source.
2. Set the repository rules to let the workflow's `GITHUB_TOKEN` push to
   `gh-pages`. The build job requests `contents: write` permission.
3. Set the `github-pages` environment to permit deployment from the default
   branch. Run the workflow from that branch. Use the workflow inputs to
   select the source versions.

In **Actions → Build and publish Fuse for the web → Run workflow**, select:

- `fuse_ref=master` and `libspectrum_ref=master` to replace the current
  development build at `master/`.
- Two tags to add a release at `releases/<fuse-tag>/<libspectrum-tag>/`.
  Do not use branch names or commit IDs for a release.
  Do not combine `master` with a tag. The workflow rejects this combination.
  The workflow encodes tag names for use in directory names when necessary.

The workflow gets Fuse from the repository where it runs.
It gets libspectrum from `fuse-emulator/libspectrum`.
Both source versions must support these build instructions.
The workflow does not change old releases or apply patches.
It uses Emscripten 6.0.12.

The workflow stores all published files on `gh-pages`.
It uploads those files as a Pages artifact and deploys that artifact.
Keep Pages configured to use GitHub Actions, not the `gh-pages` branch.
Do not store unrelated site content on this branch.

The workflow keeps existing releases. It fails if you try to publish the same
tag pair again. It does not replace that release.

The `emscripten-pages` concurrency group permits one active publication job.
Other publication workflows must use the same group.
GitHub keeps no more than one pending run in this group.
Wait until the previous run starts before you request another run.

The site's main index lists the builds. Each build includes:

- The four application files, with `fuse.html` copied to `index.html`.
- The source commit IDs and SDK version.
- The build date and time.
- The licence and ROM notices.

The application files stay in the same directory as the HTML file.
They use relative URLs, including when Pages uses a repository subdirectory.
Publication does not add a file picker or storage for settings.

A Pages deployment can fail after the workflow pushes files to `gh-pages`.
The stored files can then be newer than the public site.
Run the failed deployment job again. Do not run the full release build again.

GitHub Pages limits storage and bandwidth. These limits also apply to the
release archive. Read the ROM redistribution notices before you make the site
public.

You can also use the shared build script on your computer.
First, activate the SDK with `. emsdk_env.sh`.
Put clean `fuse` and `libspectrum` source directories in `SOURCE_ROOT`.

```sh
# Run from SOURCE_ROOT, which contains fuse/ and libspectrum/.
bash fuse/.github/scripts/build_emscripten.sh /absolute/path/to/SOURCE_ROOT
```

To test the publication code, run this command from the Fuse source directory.
These tests do not require the SDK.

```sh
python3 .github/scripts/test_publish_emscripten.py
```

## Disabled features

- POSIX threads and sockets. This build does not include network access.
- Hardware joystick access. Use the keyboard instead.
- libxml2 settings, bzip2 compression, libgcrypt, libpng, and
  desktop integration. These features are not required for this build.
  Zlib stays enabled for compressed snapshot data.
  Fuse uses its default settings and SDL 2 audio and display code.
- Internal unit tests. These tests require Fuse's null UI.
  With an interactive UI, `make check` reports that a null-UI build is required.

## Audio pacing and browser scheduling

Fuse and the Widget UI use blocking loops and delay calls.
Asyncify lets these calls give control back to the browser.

The audio producer uses `compat/emscripten/wait.c` when it must wait.
A short `emscripten_sleep()` call lets browser audio and input callbacks run.
The producer then checks the audio buffer and admission conditions again.
The audio callback never sleeps.
A timeout does not permit the producer to overwrite queued PCM data.

Host builds keep their existing wait/wake code.
The web build does not require POSIX semaphores or pthreads.

Browsers can slow timers in background tabs.
They can also stop audio until you click the page.
The producer gives control back to the browser while it waits for audio.
This lets the browser process your input.
Emulation can stop while the browser does not consume audio.

This build keeps the existing main loop. It does not use a browser-specific
main loop.

## Testing

### Clean container build

The `emscripten/emsdk:6.0.12` image contains the SDK.
It does not contain all required host build tools.
Use this Dockerfile to add them:

```dockerfile
FROM emscripten/emsdk:6.0.12
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential autoconf automake libtool pkg-config perl flex bison python3 \
    && rm -rf /var/lib/apt/lists/*
```

Save the file as `Dockerfile.emscripten` in the parent directory of clean
`fuse` and `libspectrum` source directories.
Run these commands from that parent directory:

```sh
docker build -f Dockerfile.emscripten -t fuse-emscripten:6.0.12 .
docker run --rm -it -v "$PWD:/src" -w /src fuse-emscripten:6.0.12 bash
```

In the container, run the source preparation and build commands above.
Do not run the emsdk installation steps. The SDK is already active.

Use separate copies of the sources to test each build method.
The build writes files into the source directories and the build directories.

To test the publication script, start another container with clean source
copies. Run this command from `/src`:

```sh
bash fuse/.github/scripts/build_emscripten.sh /src
```

Make sure that these files exist under `build/wasm/fuse` and are not empty:

- `fuse.html`
- `fuse.js`
- `fuse.wasm`
- `fuse.data`

Container builds check the required tools and library selection.
They do not test browser playback.
Use an HTTP server to test the files in browsers.

### Browser tests

Check that the interface responds and audio has no unwanted breaks.
Test these conditions:

- Play a tape.
- Open and close the Widget UI.
- Switch to another application while playback continues.
- Stop and restart browser audio.
- Run with sound disabled.

Browser timer limits and audio rules can differ between browsers.

### Cooperative wait test

This test runs under Node.
First, activate the SDK and configure Fuse.
Then run these commands from the Fuse source directory:

```sh
emcc -O2 -sASYNCIFY -I. compat/emscripten/wait.c \
  unittests/emscriptenwaittest.c -o /tmp/emscriptenwaittest.js
node /tmp/emscriptenwaittest.js
```

The test checks timeouts and progress notifications.
It also checks that a JavaScript callback can run while the producer waits.
It does not test SDL audio playback or sound quality.
