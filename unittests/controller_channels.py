#!/usr/bin/env python3
"""Two general controller workers: ownership, concurrency, bounds and reset."""
import ctypes
import http.server
import os
import struct
import tempfile
import threading
import time

PAYLOAD = bytes(range(256)) * 4096
started = threading.Event()

class Server(http.server.BaseHTTPRequestHandler):
    def do_HEAD(self):
        self.send_response(200)
        self.end_headers()
    def do_GET(self):
        if self.path == '/missing':
            self.send_response(404)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        self.send_response(200)
        self.send_header('Content-Length', str(len(PAYLOAD)))
        self.end_headers()
        started.set()
        try:
            if self.path == '/waiting': time.sleep(1)
            for offset in range(0, len(PAYLOAD), 16384):
                self.wfile.write(PAYLOAD[offset:offset + 16384])
                self.wfile.flush()
                time.sleep(.02)
        except (BrokenPipeError, ConnectionResetError): pass
    def log_message(self, *args): pass

def main():
    with tempfile.TemporaryDirectory() as config:
        os.environ['XDG_CONFIG_HOME'] = config
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Server)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        lib = ctypes.CDLL(os.environ.get('FUSEX_LIB', '/opt/fusex/lib/libfusex.so'))
        lib.fuse_init.argtypes = (ctypes.c_int, ctypes.POINTER(ctypes.c_char_p))
        lib.fuse_init.restype = ctypes.c_int
        lib.spectranext_controller_write.argtypes = (ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint8)
        lib.spectranext_controller_read.argtypes = (ctypes.c_void_p, ctypes.c_uint16)
        lib.spectranext_controller_read.restype = ctypes.c_uint8
        lib.xfs_handle_mount.argtypes = (ctypes.c_void_p,)
        lib.spectranet_ram_page.argtypes = (ctypes.c_uint8,)
        lib.spectranet_ram_page.restype = ctypes.POINTER(ctypes.c_uint8)
        args = [b'fusex', b'--machine=48', b'--spectranet', b'--no-spectranet-disable', b'--no-banner']
        assert lib.fuse_init(len(args), (ctypes.c_char_p * len(args))(*args)) == 0
        try:
            for _ in range(400): lib.spectrum_do_frame()
            controller = (ctypes.c_uint8 * 4096).in_dll(lib, 'spectranext_controller')
            xfs = (ctypes.c_uint8 * 4096).in_dll(lib, 'xfs_registers')
            base = (ctypes.c_char * 512).in_dll(lib, 'xfs_base_path').value.decode()
            with open(os.path.join(base, 'seed.bin'), 'wb') as file: file.write(PAYLOAD)
            def write(offset, value): lib.spectranext_controller_write(None, offset, value)
            def status(channel): return lib.spectranext_controller_read(None, 0xffd if channel else 0xfff)
            def submit(channel, command, payload=b''):
                offset = 0xc00 if channel else 0x800
                for index, value in enumerate(payload): write(offset + index, value)
                write(0xffd if channel else 0xfff, 0xff)
                before = time.monotonic()
                write(0xffc if channel else 0xffe, command)
                assert time.monotonic() - before < .1, 'submission blocked emulation'
            def wait(channel, expected=0):
                deadline = time.monotonic() + 15
                frames = 0
                while status(channel) == 0xff and time.monotonic() < deadline:
                    before = time.monotonic()
                    lib.spectrum_do_frame()
                    assert time.monotonic() - before < .2, 'emulation stalled'
                    frames += 1
                    time.sleep(.001)
                assert status(channel) == expected, (channel, status(channel))
                return frames
            def engine(channel, source, output, operation='cp'):
                submit(channel, 6, struct.pack('<128s128s256s', source.encode(), output.encode(), operation.encode()))
            def read(channel, page, offset, count=512):
                submit(channel, 8, struct.pack('<128sIBHI', b'/seed.bin', 0, page, offset, count))
            def mount():
                xfs[6] = 3
                xfs[8:520] = struct.pack('<32s64s288s64s64s', b'http',
                    f'127.0.0.1:{server.server_port}'.encode(), b'/', b'', b'')
                lib.xfs_handle_mount(xfs)
                assert struct.unpack('<h', bytes(xfs[2:4]))[0] == 0, bytes(xfs[:8])
            # Both channels use the same dispatcher, including ordinary commands.
            for channel in range(2):
                submit(channel, 0); wait(channel)
                submit(channel, 1); wait(channel)
                submit(channel, 2, b'\0'); wait(channel)
                submit(channel, 5, b'127.0.0.1\0'); wait(channel)
                start = 0xc00 if channel else 0x800
                read(channel, 0x48, start + 0x100); wait(channel)
                assert bytes(controller[start + 0x100:start + 0x300]) == PAYLOAD[:512]
                read(channel, 0x48, 0x900 if channel else 0xd00); wait(channel, 1)
                engine(channel, '0:seed.bin', f'range{channel}.bin', 'cp 100 65536'); wait(channel)
                assert open(os.path.join(base, f'range{channel}.bin'), 'rb').read() == PAYLOAD[100:100 + 65536]
                engine(channel, '0:seed.bin', '', 'not-an-engine'); wait(channel, 1)
                assert ctypes.c_int8(controller[start + 512]).value == -1
            print('PASS: both channels dispatch status, scan/AP, DNS, XFS_READ, ranged enginecalls and errors')
            mount()
            for background in range(2):
                foreground = 1 - background
                started.clear()
                engine(background, '3:slow', 'slow.bin')
                assert started.wait(2)
                assert status(background) == 0xff
                original = bytes(controller[(0xc00 if background else 0x800):][:512])
                # Busy writes must not corrupt worker-owned arguments or finish it early.
                submit(background, 0, b'corrupt')
                assert status(background) == 0xff
                assert bytes(controller[(0xc00 if background else 0x800):][:512]) == original
                for _ in range(10):
                    read(foreground, 0xd0, 0, 8192); wait(foreground)
                    assert bytes(lib.spectranet_ram_page(0xd0)[:4096]) == PAYLOAD[:4096]
                    assert bytes(lib.spectranet_ram_page(0xd1)[:4096]) == PAYLOAD[4096:8192]
                    assert status(background) == 0xff
                assert wait(background) > 20
                assert open(os.path.join(base, 'slow.bin'), 'rb').read() == PAYLOAD
            print('PASS: either channel reads while the other downloads; busy ownership and multi-page contents verified')
            engine(1, '3:missing', 'missing.bin'); wait(1, 1)
            started.clear()
            engine(1, '3:waiting', 'cancel.bin')
            assert started.wait(2)
            before = time.monotonic()
            lib.xfs_reset()
            assert time.monotonic() - before < .5, 'reset did not cancel blocked download'
            wait(1, 1)
            mount()
            engine(1, '3:slow', 'restart.bin'); wait(1)
            assert open(os.path.join(base, 'restart.bin'), 'rb').read() == PAYLOAD
            # Implicit source mount is captured when submitted, not when a worker runs.
            lib.spectranet_ram_page(0xc0)[0xf6f] = 3
            engine(1, 'slow', 'default.bin')
            lib.spectranet_ram_page(0xc0)[0xf6f] = 0
            wait(1)
            assert open(os.path.join(base, 'default.bin'), 'rb').read() == PAYLOAD
            print('PASS: HTTP error, reset cancellation, subsequent reuse and default-mount snapshot')
        finally:
            lib.controller_job_cancel_and_wait()
            lib.fuse_end()
            server.shutdown()
if __name__ == '__main__': main()
