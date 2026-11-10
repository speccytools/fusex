#!/usr/bin/env python3
"""Add one browser build to a persistent site; tagged builds are immutable."""
import datetime
import html
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from urllib.parse import quote


def git_sha(source):
    return subprocess.check_output(
        ["git", "-C", str(source), "rev-parse", "HEAD"], text=True
    ).strip()


def publish(sources, site, fuse_ref, libspectrum_ref, emscripten_version):
    development = fuse_ref == libspectrum_ref == "master"
    if not development:
        for ref in (fuse_ref, libspectrum_ref):
            if ref == "master":
                raise ValueError("Select master/master or two explicit tags")
            subprocess.run(["git", "check-ref-format", f"refs/tags/{ref}"], check=True)
    # Encode slashes in tags so each tag is exactly one directory component.
    path = "master" if development else (
        f"releases/{quote(fuse_ref, safe='')}/{quote(libspectrum_ref, safe='')}"
    )
    destination = site / path
    if destination.exists() and not development:
        raise ValueError(f"Release already published: {path}; refusing to overwrite")

    build = sources / "build/wasm/fuse"
    files = ("fuse.html", "fuse.js", "fuse.wasm", "fuse.data")
    for name in files:
        if not (build / name).is_file() or not (build / name).stat().st_size:
            raise ValueError(f"Missing or empty build output: {name}")
    manifest_file = site / "builds.json"
    entries = json.loads(manifest_file.read_text()) if manifest_file.exists() else []
    metadata = {
        "path": path,
        "fuse_ref": fuse_ref,
        "libspectrum_ref": libspectrum_ref,
        "fuse_sha": git_sha(sources / "fuse"),
        "libspectrum_sha": git_sha(sources / "libspectrum"),
        "emscripten_version": emscripten_version,
        "built_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    }
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    for name in files:
        shutil.copy2(build / name, destination / ("index.html" if name == "fuse.html" else name))
    notices = destination / "notices"
    for project in ("fuse", "libspectrum"):
        target = notices / project
        target.mkdir(parents=True)
        shutil.copy2(sources / project / "COPYING", target / "COPYING")
    shutil.copy2(sources / "fuse/roms/README.copyright", notices / "roms-copyright.txt")
    (destination / "build-info.json").write_text(json.dumps(metadata, indent=2) + "\n")
    entries = [entry for entry in entries if entry["path"] != path] + [metadata]
    entries.sort(key=lambda entry: (entry["path"] != "master", entry["path"]))
    manifest_file.write_text(json.dumps(entries, indent=2) + "\n")

    rows = []
    for entry in entries:
        url = quote(entry["path"], safe="/") + "/"
        label = f"Fuse {entry['fuse_ref']} / libspectrum {entry['libspectrum_ref']}"
        rows.append(
            f'<li><a href="{url}">{html.escape(label)}</a> '
            f'— {html.escape(entry["built_at"])} '
            f'(<a href="{url}build-info.json">build details</a>, '
            f'<a href="{url}notices/roms-copyright.txt">ROM notices</a>, '
            f'<a href="{url}notices/fuse/COPYING">Fuse licence</a>, '
            f'<a href="{url}notices/libspectrum/COPYING">libspectrum licence</a>)</li>'
        )
    (site / "index.html").write_text(
        '<!doctype html>\n<html lang="en"><meta charset="utf-8">\n'
        '<meta name="viewport" content="width=device-width, initial-scale=1">\n'
        '<title>Fuse browser builds</title>\n'
        '<h1>Fuse browser builds</h1>\n'
        '<p>The SDL Widget UI running in WebAssembly. Click the emulator canvas '
        'to focus keyboard input; audio may require a user gesture. '
        'Browser file pickers and persistent storage are not provided.</p>\n'
        '<ul>\n' + "\n".join(rows) + '\n</ul>\n</html>\n'
    )
    (site / ".nojekyll").touch()


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("Usage: publish_emscripten.py SOURCE_ROOT SITE_ROOT")
    publish(Path(sys.argv[1]), Path(sys.argv[2]), os.environ["FUSE_REF"],
            os.environ["LIBSPECTRUM_REF"], os.environ["EMSCRIPTEN_VERSION"])
