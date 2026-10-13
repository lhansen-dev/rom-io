"""Extract ROM archives with progress measured by archives processed."""

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import os
from pathlib import Path
import shutil
import subprocess
import sys


def progress(done, total, label):
    width = 30
    filled = width * done // total
    bar = "#" * filled + "-" * (width - filled)
    text = f"[{bar}] {done}/{total} ({done * 100 // total:3d}%) {label}"
    if sys.stdout.isatty():
        # Keep the bar on one line, even with long archive names.
        columns = shutil.get_terminal_size().columns
        print("\r\033[K" + text[:max(1, columns - 1)], end="", flush=True)
    else:
        print(text, flush=True)


def raise_walk_error(error):
    raise error


def extract(archive):
    try:
        result = subprocess.run(
            ["unzip", "-n", "-q", str(archive), "-d", str(archive.parent)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
        )
        if result.returncode:
            return result.stdout.strip() or f"unzip exited with status {result.returncode}"
        archive.unlink()
    except OSError as exc:
        return str(exc)
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--jobs", type=int, default=4, help="parallel extractions (default: 4)")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")
    root = args.root.resolve()
    if not root.is_dir():
        parser.error(f"ROM directory does not exist: {args.root}")
    if shutil.which("unzip") is None:
        parser.error("unzip is not installed")

    archives = []
    for directory, _, filenames in os.walk(root, onerror=raise_walk_error):
        for name in filenames:
            path = Path(directory) / name
            if path.suffix.lower() == ".zip" and not path.is_symlink() and path.is_file():
                archives.append(path)
    archives.sort()
    if not archives:
        print(f"No ZIP archives found in {args.root}.")
        return 0

    total = len(archives)
    failed = 0
    progress(0, total, f"Extracting with {min(args.jobs, total)} workers")
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {executor.submit(extract, archive): archive for archive in archives}
        try:
            for done, future in enumerate(as_completed(futures), start=1):
                label = str(futures[future].relative_to(root))
                error = future.result()
                if error:
                    failed += 1
                    if sys.stdout.isatty():
                        print(flush=True)
                    print(f"Failed: {label} (archive kept)\n{error}", file=sys.stderr, flush=True)
                progress(done, total, f"{'Failed' if error else 'Done'}: {label}")
        except KeyboardInterrupt:
            for future in futures:
                future.cancel()
            raise

    if sys.stdout.isatty():
        print()
    print(f"Finished: {total - failed} archives extracted and deleted; {failed} failed.")
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nExtraction interrupted; pending archives kept.", file=sys.stderr)
        sys.exit(130)
    except OSError as exc:
        print(f"\nExtraction failed: {exc}", file=sys.stderr)
        sys.exit(1)
