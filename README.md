# rom-io

*Pronounced "Romeo".* Download your ROMs to your 3DS over the home network.

- `server/`: a small Python HTTP server (standard library only) that serves `roms/`.
- `client/`: a 3DS homebrew app (`rom-io.3dsx`) for browsing the server and downloading ROMs to the SD card.

## Make targets

| | |
|---|---|
| `make` / `make help` | show available targets, configuration, and examples |
| `make install` | install dependencies (Arch, with the devkitPro pacman repo) |
| `make server` | start the server (`PORT=...` to override 8765, `ROMS_DIR=...` to override `roms`) |
| `make client` | build the 3DS client |
| `make ftp-client` | build the client and FTP it to the 3DS's `/3ds/rom-io.3dsx`, replacing the old one (`3DS=ip:port` to override 192.168.1.233:5000; open ftpd on the 3DS first) |
| `make unzip` | extract every zip under `roms/` next to itself with an archive progress bar and four parallel workers (`JOBS=...` to override), deleting each zip that extracted cleanly |
| `make convert` | run `unzip`, then `cia`; stop if extraction fails |
| `make organize-3ds` | move each `.3ds` under `roms/3ds/` into a folder named after the game |
| `make cia` | organize 3DS ROMs and create a `.cia` alongside each original; install the converter automatically |
| `make install-converter` | install a pinned version of 3dsconv in `.tools/3dsconv/` |
| `make test` | server tests |
| `make test-client` | host tests for the client download queue and CIA installer (requires a C compiler; `HOST_CC=...` to override) |
| `make clean` | remove client build artifacts |

Set `PYTHON=...` to use a different Python interpreter. `ROMS_DIR=...` also controls the directory used by `make unzip`.

`make cia BOOT9=/path/to/boot9.bin` converts cartridge images with [3dsconv](https://github.com/ihaveamac/3dsconv).
Use `make convert` to extract ZIP archives before converting; overrides such as `ROMS_DIR`, `JOBS`, and `BOOT9` apply to both stages.
The BootROM dump is also found automatically in `~/.3ds/boot9.bin` or `~/.3ds/boot9_prot.bin`.
Original `.3ds` files are preserved, existing CIAs are skipped, and unfinished output is kept hidden and removed after failure.
For example, the resulting layout is:

```text
roms/3ds/WarioWare Gold (USA)/
  WarioWare Gold (USA).3ds
  WarioWare Gold (USA).cia
```

## Server

Put one folder per platform in `roms/` (gitignored; here it is a symlink to `/storage/roms` on the HDD):

```
roms/
  gba/  Some Game (USA).gba
  snes/ ...
```

```sh
python3 server/server.py                  # serves ./roms on port 8765
python3 server/server.py --root /path/to/roms --port 9000
```

Find the server's LAN IP with `ip -4 addr` and allow the port through your firewall if you run one.
Anyone on your network can read these files, and the server has no login.

Tests: `cd server && python3 -m unittest`

## 3DS client

You need devkitPro with the `3ds-dev` group installed.

```sh
cd client && make
```

Copy `client/rom-io.3dsx` to `sdmc:/3ds/` and launch it from the Homebrew Launcher.

- The first time it runs, enter the server address, e.g. `192.168.1.10:8765`.
- Long ROM names wrap across lines in the browser, with scrolling and paging adjusted to keep complete entries visible.
- **D-pad / Circle Pad**: move. **Left / Right**: page up/down. **A**: open a folder or download a file. **Y**: filter the list (every word must appear in the name, ignoring case). **B**: clear the filter, else go up a folder (or cancel a download). **X**: change the server. **L**: cycle 1 → 2 → 3 → 4 → 1 download streams (starts at 2). **R**: refresh the current folder, keeping the filter and selected entry if it still exists. **START**: quit.
- When you download, the app asks for a destination folder (default: the last one you used) and creates it if it doesn't exist. If the filename already exists there, **A** confirms overwriting and **B** cancels. It downloads to `<name>.part` first, then renames the file when the download finishes, so a failed download never overwrites a good file.
- By default, files of at least 1 MiB download over two concurrent HTTP connections. The selected number of streams (up to four) splits the file into contiguous ranges, with four queued 256 KiB chunks per stream. Data is written directly to its position in one temporary file using 64-bit offsets. Range boundaries, lengths, and file versions are verified before combining the ranges. Restart the server after updating to enable Range support.
- SD allocation runs in a worker and shows allocated MiB; **B** requests cancellation between allocation steps. Connection startup and header validation also run in workers while the client drains ready streams. A transfer with no progress for 30 seconds is cancelled, and completed connections close immediately to free slots for queued requests.
- Press **L** in the browser to compare stream counts on the same file; the footer shows the selected count. One-stream mode uses a 512 KiB SD write buffer. Smaller files, relative destination paths, and servers without Range support use one stream automatically. The New 3DS CPU speedup is enabled. Progress updates at most four times per second and shows average speed, estimated time remaining, and max throughput sampled over one-second intervals across all streams. The peak resets for each download; downloads shorter than a second use their overall rate.
- Files of 4 GiB or larger cannot fit on the FAT32 SD card and are rejected before downloading. Trim cartridge images or convert them to smaller CIA files on your computer first.
- Settings are saved to `sdmc:/3ds/rom-io/config.txt`. Line 1 is the server and line 2 is the last folder you used.

### CIA installation

Selecting a `.cia` offers **A: download only**, **X: download then install**, or **B: cancel**.
Installation requires access to the 3DS Application Manager under custom firmware such as Luma3DS.
Choose an absolute SD destination such as `sdmc:/cias`; the existing parallel download completes before installation starts.
The installer validates CIA metadata and available SD space, then asks for confirmation with the title ID and version.
It supports SD applications, demos, updates, and DLC; NAND/system titles and DSiWare are rejected.
Press **B** during installation to cancel. Interrupted reads/writes cancel the active installation, and errors show the result code.
The downloaded CIA is kept after success, cancellation, or installation failure, so allow room for both the CIA and installed contents.
Existing-title conflicts can cause installation to fail; rom-io does not delete installed titles or tickets to make room.
Automatic title-seed import is not included; seed-encrypted titles may need their seed imported separately with FBI before they can launch.
