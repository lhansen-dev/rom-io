3DS ?= 192.168.1.233:5000
PORT ?= 8765

.PHONY: install server client ftp-client unzip test

# Arch + devkitPro pacman repo. The server itself only needs Python's standard library.
install:
	sudo pacman -S --needed python unzip curl 3ds-dev

server:
	python3 server/server.py --port $(PORT)

client:
	$(MAKE) -C client

# --retry covers the 3DS's Wi-Fi waking up from sleep; STOR overwrites the old copy.
ftp-client: client
	curl --connect-timeout 10 --retry 5 --retry-all-errors -T client/rom-io.3dsx ftp://$(3DS)/3ds/rom-io.3dsx

# Extracts each zip next to itself (never overwriting) and deletes the zip only if it extracted cleanly.
unzip:
	find roms/ -type f -iname '*.zip' -exec sh -c 'unzip -n -q "$$1" -d "$$(dirname "$$1")" && rm -- "$$1"' _ {} \;

test:
	cd server && python3 -m unittest
