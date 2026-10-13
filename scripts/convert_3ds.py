"""Organize cartridge images and create CIAs alongside the originals."""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


BOOT9_HASHES = {
    "2f88744feed717856386400a44bba4b9ca62e76a32c715d4f309c399bf28166f",
    "7331f7edece3dd33f2ab4bd0b3a5d607229fd19212c10b734cedcaf78c1a7b98",
}


def find_boot9(explicit):
    candidates = [Path(explicit).expanduser()] if explicit else [
        base / name
        for base in (Path.cwd(), Path.home() / ".3ds")
        for name in ("boot9.bin", "boot9_prot.bin")
    ]
    for candidate in candidates:
        if not candidate.is_file():
            continue
        if candidate.stat().st_size not in (0x8000, 0x10000):
            raise ValueError(f"Invalid BootROM dump size: {candidate}")
        if hashlib.sha256(candidate.read_bytes()).hexdigest() not in BOOT9_HASHES:
            raise ValueError(f"BootROM dump failed verification: {candidate}")
        return candidate.resolve()
    raise ValueError("BootROM dump not found. Set BOOT9=/path/to/boot9.bin or put it in ~/.3ds/.")


def plan_moves(root):
    moves = []
    destinations = set()
    for directory, dirs, filenames in os.walk(root):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        for name in sorted(filenames):
            source = Path(directory) / name
            if source.suffix.lower() != ".3ds" or source.is_symlink():
                continue
            folder = root / source.stem
            destination = folder / source.name
            if destination in destinations:
                raise ValueError(f"Multiple ROMs would map to {destination}")
            if folder.exists() and not folder.is_dir():
                raise ValueError(f"Game folder is occupied by a file: {folder}")
            if source != destination and (destination.exists() or destination.is_symlink()):
                raise ValueError(f"Destination already exists: {destination}")
            destinations.add(destination)
            moves.append((source, destination))
    return moves


def convert(source, converter, boot9):
    destination = source.with_suffix(".cia")
    # A temporary output folder prevents interrupted conversions from being
    # mistaken for completed CIAs on the next run, or served by the ROM browser.
    with tempfile.TemporaryDirectory(prefix=".cia-", dir=source.parent) as temporary:
        command = [converter, f"--output={temporary}", f"--boot9={boot9}", str(source)]
        completed = False
        with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, errors="replace") as process:
            try:
                for line in process.stdout:
                    print(line, end="", flush=True)
                    if "Done converting 1 out of 1 files." in line:
                        completed = True
                code = process.wait()
            except BaseException:
                process.terminate()
                process.wait()
                raise
        output = Path(temporary) / destination.name
        if code or not completed or not output.is_file() or output.stat().st_size == 0:
            raise ValueError(f"Conversion failed: {source.name} (original kept)")
        # Publishing with a hard link refuses to overwrite an existing CIA,
        # including one created by another run while conversion was underway.
        os.link(output, destination)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--organize-only", action="store_true")
    parser.add_argument("--converter", default="3dsconv")
    parser.add_argument("--boot9", default=os.environ.get("BOOT9_PATH", ""))
    args = parser.parse_args()
    root = args.root.resolve()
    if not root.is_dir():
        parser.error(f"ROM directory does not exist: {args.root}")
    moves = plan_moves(root)
    if not moves:
        print(f"No .3ds ROMs found in {root}.")
        return 0
    pending = [destination for _, destination in moves if not destination.with_suffix(".cia").exists()]
    converter = boot9 = None
    if pending and not args.organize_only:
        converter = shutil.which(args.converter)
        if not converter:
            parser.error("3dsconv not found. Run make install-converter first.")
        converter = str(Path(converter).resolve())
        boot9 = find_boot9(args.boot9)

    for source, destination in moves:
        if source != destination:
            destination.parent.mkdir(exist_ok=True)
            source.rename(destination)
            print(f"Moved: {destination.relative_to(root)}", flush=True)
    print(f"Organized {len(moves)} ROMs.", flush=True)
    if args.organize_only:
        return 0

    failed = 0
    for index, source in enumerate(pending, start=1):
        print(f"\n[{index}/{len(pending)}] Converting {source.name}", flush=True)
        try:
            destination = convert(source, converter, boot9)
            print(f"Created: {destination.relative_to(root)}", flush=True)
        except (OSError, ValueError) as exc:
            failed += 1
            print(f"Error: {exc}", file=sys.stderr, flush=True)
    skipped = len(moves) - len(pending)
    print(f"\nFinished: {len(pending) - failed} CIAs created; {skipped} existing CIAs skipped; {failed} failed.")
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nConversion interrupted; original ROMs kept.", file=sys.stderr)
        sys.exit(130)
    except (OSError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)
