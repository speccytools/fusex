#!/usr/bin/env python3
"""Compare frame-only and scheduled automation-enabled null Fuse executables.

Run from the source root: make check-sound-engine-equivalence
Interval overrides are compiled into test executables, never runtime options.
Executables must be able to find the checkout's ROMs. This is deliberately
not a pacing test. Fixtures are generated, not copyrighted game snapshots.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile


def snapshot(path, ay, ay_only=False):
    header = bytearray(27)
    header[19] = 4
    header[23:25] = struct.pack("<H", 0x9000)
    header[25] = 1
    ram = bytearray(49152)
    if ay:
        # Select mixer: tones enabled, noise disabled; A period 64; volume A.
        code = bytes.fromhex(
            "f3 01 fd ff 3e 07 ed 79 01 fd bf 3e 38 ed 79 "
            "01 fd ff 3e 00 ed 79 01 fd bf 3e 40 ed 79 "
            "01 fd ff 3e 08 ed 79")
        loop = 0x8000 + len(code)
        # Alternate AY volume and ULA EAR, separated by DJNZ delays.
        code += bytes.fromhex(
            "01 fd bf 3e 0f ed 79 3e 10 d3 fe 06 ff 10 fe "
            "01 fd bf af ed 79 d3 fe 06 ff 10 fe c3")
        code += struct.pack("<H", loop)
        if ay_only:
            code = code.replace(bytes.fromhex("d3 fe"), bytes.fromhex("00 00"))
        extension = struct.pack("<HBB", 0x8000, 0, 0) + bytes(5 * 16384)
    else:
        code = bytes.fromhex(
            "f3 3e 10 d3 fe 06 ff 10 fe af d3 fe 06 ff 10 fe c3 01 80")
        ram[0x5000:0x5002] = struct.pack("<H", 0x8000)
        extension = b""
    ram[0x4000:0x4000 + len(code)] = code
    if ay:
        # SNA identifies 128K snapshots as Pentagon. Use Z80 v2 to explicitly
        # select a real 128K machine, without depending on Pentagon ROMs.
        z80 = bytearray(30)
        z80[8:10] = struct.pack("<H", 0x9000)
        z80[29] = 1
        extra = bytearray(23)
        extra[:2] = struct.pack("<H", 0x8000)
        extra[2] = 3  # v2 Spectrum 128K
        pages = []
        for bank in range(8):
            page = bytearray(16384)
            if bank == 2:
                page[:len(code)] = code
            pages.append(struct.pack("<HB", 0xffff, bank + 3) + page)
        path.write_bytes(z80 + struct.pack("<H", 23) + extra + b"".join(pages))
    else:
        path.write_bytes(header + ram + extension)


def delayed_prefix_fixture(snapshot_path, output):
    data = bytearray(snapshot_path.read_bytes())
    # Start normally, then cross several scheduler cuts in the legal prefix
    # workload. Keep the SNA entry stack outside the prefix/program payload.
    lead = bytes.fromhex("f3 01 ff ff 0b 78 b1 20 fb")
    loop = 0x8001 + len(lead) + 12000
    code = bytes.fromhex(
        "f3 3e 10 d3 fe 06 ff 10 fe af d3 fe 06 ff 10 fe c3")
    payload = lead + bytes([0xdd]) * 12000 + code + struct.pack("<H", loop)
    data[27 + 16384:27 + 16384 + len(payload)] = payload
    data[23:25] = struct.pack("<H", 0xf000)
    data[27 + 0xb000:27 + 0xb002] = struct.pack("<H", 0x8000)
    output.write_bytes(data)
    return loop - 1, 0x8000 + len(payload)


def rzx_fixture(snapshot_path, output):
    # Uncompressed RZX 0.12 snapshot and input blocks, no input-port reads.
    data = snapshot_path.read_bytes()
    snap = struct.pack("<I4sI", 0, b"SNA\0", len(data)) + data
    frames = struct.pack("<HH", 6000, 0) * 120
    inputs = struct.pack("<IBII", 120, 0, 0, 0) + frames
    def block(kind, payload):
        return struct.pack("<BI", kind, len(payload) + 5) + payload
    output.write_bytes(b"RZX!" + struct.pack("<BBI", 0, 12, 0) +
                       block(0x30, snap) + block(0x80, inputs))


def capture(executable, fixture, output, machine, separation="none", tape=None):
    output.mkdir()
    roms = Path(__file__).resolve().parent.parent / "roms"
    options = ["--no-autosave-settings", "--rom-48", str(roms / "48.rom"),
               "--rom-128-0", str(roms / "128-0.rom"),
               "--rom-128-1", str(roms / "128-1.rom")]
    if tape is not None:
        options += ["--tape", str(tape), "--no-auto-load", "--no-fastload",
                    "--no-traps", "--no-accelerate-loader"]
    media = "--playback" if fixture.suffix == ".rzx" else "--snapshot"
    subprocess.run([
        str(executable), "--machine", machine, media, str(fixture),
        "--sound", "--sound-freq", "44100", "--separation", separation,
        "--speaker-type", "automatic", "--speed", "100",
        "--volume-ay", "100", "--volume-beeper", "100",
        "--automation-output", str(output), "--automation-frames", "100",
        "--automation-capture-audio", *options,
    ], check=True, stdout=subprocess.DEVNULL)
    result = json.loads((output / "result.json").read_text())
    if result["execution"]["actual_machine"] != machine:
        raise SystemExit(f"expected {machine}, got {result['execution']['actual_machine']}")
    return result


def compare(results, dirs, name):
    for key in ("execution", "state", "settings"):
        if results[0][key] != results[1][key]:
            raise SystemExit(name + ": " + key + " differs")
    if (dirs[0] / "audio.wav").read_bytes() != (dirs[1] / "audio.wav").read_bytes():
        raise SystemExit(name + ": PCM differs")
    if results[1]["execution"]["actual_machine"] != results[0]["execution"]["actual_machine"]:
        raise SystemExit(name + ": actual machine differs")
    audio = results[1]["artifacts"]["audio"]
    print(f"{name}: {audio['frames']} frames, PCM {audio['pcm_crc32']}, "
          "identical WAV and execution state")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("frame_only", type=Path)
    parser.add_argument("production", type=Path)
    parser.add_argument("--interval-builds", nargs=5, type=Path,
                        metavar="EXE", help="test builds for 35000/17500/8750/997/1")
    args = parser.parse_args()
    baseline = args.frame_only.resolve()
    trials = [("production", args.production.resolve())]
    if args.interval_builds:
        trials += [(str(interval), exe.resolve()) for interval, exe in
                   zip((35000, 17500, 8750, 997, 1), args.interval_builds)]
    with tempfile.TemporaryDirectory(prefix="fuse-audio-equivalence-") as temp:
        root = Path(temp)
        for name, ay, machine in [("beeper", False, "48"),
                                  ("ay", True, "128")]:
            fixture = root / (name + (".z80" if ay else ".sna"))
            snapshot(fixture, ay)
            # The same real CPU workload crosses instruction, source-event,
            # AY-tick and frame boundaries for every production interval.
            fixtures = [(name, fixture, None)]
            if ay:
                ay_only = root / "ay-only.z80"
                snapshot(ay_only, True, ay_only=True)
                fixtures.append(("ay-only", ay_only, None))
            else:
                rzx = root / "fallback.rzx"
                rzx_fixture(fixture, rzx)
                fixtures.append(("rzx-fallback", rzx, None))
                prefix = root / "delayed-prefix.sna"
                prefix_pc = delayed_prefix_fixture(fixture, prefix)
                fixtures.append(("delayed-prefix", prefix, None))
                tape_snap = root / "tape.sna"
                data = bytearray(fixture.read_bytes())
                data[11:13] = struct.pack("<H", 1)  # DE byte count
                data[17:19] = struct.pack("<H", 0x8000)  # IX destination
                data[21:23] = bytes((1, 255))  # AF: load data, carry set
                data[27 + 0x5000:27 + 0x5002] = struct.pack("<H", 0x0556)
                tape_snap.write_bytes(data)
                tape = root / "edges.tzx"
                tape.write_bytes(b"ZXTape!\x1a\x01\x14" +
                                 bytes((0x12,)) + struct.pack("<HH", 110, 65535))
                fixtures.append(("tape-edges", tape_snap, tape))
            for label, media, tape in fixtures:
                for separation in ("none", "ACB"):
                    base_dir = root / (label + separation + "base")
                    base = capture(baseline, media, base_dir, machine, separation, tape=tape)
                    if label == "delayed-prefix" and not (
                            prefix_pc[0] <= base["state"]["cpu"]["pc"] < prefix_pc[1]):
                        raise SystemExit("prefix fixture did not reach its final loop")
                    if tape and not base["state"]["machine"]["tape_playing"]:
                        raise SystemExit("tape fixture did not start playback")
                    if separation != "none" and base["artifacts"]["audio"]["channels"] != 2:
                        print(f"{label}: null UI fixes stereo enumeration to mono; "
                              "stereo covered by the direct synthesis test")
                        continue
                    for interval, executable in trials:
                        out = root / (label + separation + interval)
                        trial = capture(executable, media, out, machine,
                                        separation, tape=tape)
                        compare([base, trial], [base_dir, out],
                                f"{label}/{separation}/{interval}")


if __name__ == "__main__":
    main()
