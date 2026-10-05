#!/usr/bin/env python3
"""Exercise CP_START while the Z80 runs, including busy/error/reset paths."""
import ctypes
import http.server
import os
import struct
import threading
import time

PAYLOAD = bytes(range(256)) * 4096
started = threading.Event()
LZ4_FRAME = PAYLOAD[:3456]
LZ4_BLOCK = b'\xf0' + b'\xff' * 13 + b'\x7e' + LZ4_FRAME
LZ4_PAYLOAD = b'B4F1' + struct.pack('<I', 3456 * 32) + (struct.pack('<H', len(LZ4_BLOCK)) + LZ4_BLOCK) * 32


class Server(http.server.BaseHTTPRequestHandler):
    def do_HEAD(self):
        self.send_response(200)
        self.end_headers()

    def do_GET(self):
        if self.path == '/missing':
            self.send_error(404)
            return
        payload = LZ4_PAYLOAD if self.path == '/lz4' else PAYLOAD
        self.send_response(200)
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        started.set()
        try:
            if self.path == '/waiting':
                time.sleep(1)
            for i in range(0, len(payload), 16384):
                self.wfile.write(payload[i:i + 16384])
                self.wfile.flush()
                time.sleep(.02)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *args):
        pass


def main():
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
        for _ in range(400):
            lib.spectrum_do_frame()
        controller = (ctypes.c_uint8 * 4096).in_dll(lib, 'spectranext_controller')
        xfs = (ctypes.c_uint8 * 4096).in_dll(lib, 'xfs_registers')
        base = (ctypes.c_char * 512).in_dll(lib, 'xfs_base_path').value.decode()

        def mount(protocol='http', host=None, path='/'):
            host = host or f'127.0.0.1:{server.server_port}'
            request = struct.pack('<32s64s288s64s64s', protocol.encode(), host.encode(), path.encode(), b'', b'')
            xfs[6] = 3
            xfs[8:520] = request
            lib.xfs_handle_mount(xfs)
            assert struct.unpack('<h', bytes(xfs[2:4]))[0] == 0, bytes(xfs[:8])

        def command(number):
            controller[0xffe] = 0xff
            before = time.monotonic()
            lib.spectranext_controller_write(None, 0xffe, number)
            assert time.monotonic() - before < .1, 'command blocked emulation'
            return controller[0xfff]

        def start(source, output='cp-test.bin', operation='cp'):
            controller[0x800:0x980] = struct.pack('<128s128s128s', source.encode(), output.encode(), operation.encode())
            return command(13)

        def state():
            return lib.spectranext_controller_read(None, 0xffc)

        def wait(expected):
            deadline = time.monotonic() + 15
            frames = 0
            before = time.monotonic()
            while state() == 1 and time.monotonic() < deadline:
                frame_started = time.monotonic()
                lib.spectrum_do_frame()
                assert time.monotonic() - frame_started < .2, "emulation frame stalled"
                frames += 1
                time.sleep(.001)
            assert state() == expected, (state(), controller[0xffd])
            return frames, time.monotonic() - before

        mount()
        assert start('3:slow') == 0
        assert state() == 1
        assert start('3:slow', 'rejected.bin') == 1
        assert state() == 1
        assert command(6) == 1  # Synchronous ENGINECALL must not share CP buffers.
        frames, elapsed = wait(2)
        assert frames > 20
        assert open(os.path.join(base, 'cp-test.bin'), 'rb').read() == PAYLOAD
        assert not os.path.exists(os.path.join(base, 'rejected.bin'))
        print(f'PASS: slow copy, {frames} frames advanced during {elapsed:.2f}s, busy rejected')
        assert start('3:missing') == 0
        wait(3)
        assert ctypes.c_int8(controller[0xffd]).value == -2

        # Resolve the current mount on the emulation thread before dispatch.
        lib.spectranet_ram_page(0xc0)[0xf6f] = 3
        assert start('slow') == 0
        wait(2)
        assert open(os.path.join(base, 'cp-test.bin'), 'rb').read() == PAYLOAD

        # Cancel while open waits for the first byte, then restart cleanly.
        started.clear()
        assert start('3:waiting') == 0
        assert started.wait(2)
        before = time.monotonic()
        lib.xfs_reset()
        assert time.monotonic() - before < .5, 'reset failed to cancel blocked open'
        assert state() != 1
        mount()
        assert start('3:slow') == 0
        wait(2)
        assert open(os.path.join(base, 'cp-test.bin'), 'rb').read() == PAYLOAD
        assert start('3:lz4', operation='lz4') == 0
        wait(2)
        assert open(os.path.join(base, 'cp-test.bin'), 'rb').read() == LZ4_FRAME * 32
        started.clear()
        assert start('3:waiting') == 0
        assert started.wait(2)
        xfs[6] = 3
        lib.xfs_handle_umount.argtypes = (ctypes.c_void_p,)
        before = time.monotonic()
        lib.xfs_handle_umount(xfs)
        assert time.monotonic() - before < .5, 'unmount failed to cancel copy'
        assert state() != 1
        print('PASS: missing file, default mount, reset cancellation, restart, LZ4, unmount')

        # Optional deployed HTTPS copy using the same engine and command.
        if os.environ.get('FUSEX_CP_HTTPS'):
            lib.xfs_reset()
            mount('https', 'spectranext.net', '/demo/badapple/')
            assert start('3:badapple1.bin') == 0
            frames, elapsed = wait(2)
            size = os.path.getsize(os.path.join(base, 'cp-test.bin'))
            assert size > 100000
            print(f'PASS: deployed HTTPS {size} bytes, {frames} frames advanced, {elapsed:.2f}s')
    finally:
        lib.engine_job_cancel_and_wait()
        lib.fuse_end()
        server.shutdown()


if __name__ == '__main__':
    main()
