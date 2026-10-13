# rom-io

*Pronounced "Romeo".* Download your ROMs to your 3DS over the home network.

- `server/`: a small Python HTTP server (standard library only) that serves `roms/`.
- `client/`: a 3DS homebrew app (`rom-io.3dsx`) for browsing the server and downloading ROMs to the SD card.

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
- **D-pad / Circle Pad**: move. **Left / Right**: page up/down. **A**: open a folder or download a file. **B**: go up a folder (or cancel a download). **X**: change the server. **START**: quit.
- When you download, the app asks for a destination folder (default: the last one you used) and creates it if it doesn't exist. It downloads to `<name>.part` first, then renames the file when the download finishes, so a failed download never overwrites a good file.
- Settings are saved to `sdmc:/3ds/rom-io/config.txt`. Line 1 is the server and line 2 is the last folder you used.
