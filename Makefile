.DEFAULT_GOAL := help

3DS ?= 192.168.1.233:5000
PORT ?= 8765
PYTHON ?= python3
ROMS_DIR ?= roms
JOBS ?= 4
BOOT9 ?=
CONVERTER ?= .tools/3dsconv/bin/rom-io-3dsconv

.PHONY: help install server client ftp-client unzip convert organize-3ds cia install-converter test clean

help: ## Show available targets and configuration
	@printf 'Usage: make [target] [VARIABLE=value ...]\n\nTargets:\n'
	@awk 'BEGIN { FS = ":.*## " } /^[a-zA-Z0-9_-]+:.*## / { printf "  %-14s %s\n", $$1, $$2 }' $(MAKEFILE_LIST)
	@printf '\nVariables (current values):\n'
	@printf '  %-14s %s\n' '3DS' '$(3DS)' 'PORT' '$(PORT)' 'PYTHON' '$(PYTHON)' 'ROMS_DIR' '$(ROMS_DIR)' 'JOBS' '$(JOBS)' 'BOOT9' '$(BOOT9)' 'CONVERTER' '$(CONVERTER)'
	@printf '\nExamples:\n  make server PORT=9000\n  make ftp-client 3DS=192.168.1.100:5000\n'

# Arch + devkitPro pacman repo. The server itself only needs Python's standard library.
install: ## Install dependencies (Arch with the devkitPro pacman repo)
	sudo pacman -S --needed python unzip curl 3ds-dev

server: ## Start the ROM server
	$(PYTHON) server/server.py --root "$(ROMS_DIR)" --port "$(PORT)"

client: ## Build the 3DS client
	$(MAKE) -C client

# --retry covers the 3DS's Wi-Fi waking up from sleep; STOR overwrites the old copy.
ftp-client: client ## Build and upload the client (open ftpd on the 3DS first)
	curl --connect-timeout 10 --retry 5 --retry-all-errors -T client/rom-io.3dsx "ftp://$(3DS)/3ds/rom-io.3dsx"

# Extracts each zip next to itself (never overwriting) and deletes the zip only if it extracted cleanly.
unzip: ## Extract ROM zips in parallel with a progress bar (JOBS controls concurrency)
	@$(PYTHON) scripts/unzip_roms.py "$(ROMS_DIR)" --jobs "$(JOBS)"

convert: ## Extract ROM zips, then organize 3DS ROMs and create CIAs
	$(MAKE) unzip
	$(MAKE) cia

organize-3ds: ## Move each 3DS ROM into its own game folder
	@$(PYTHON) scripts/convert_3ds.py "$(ROMS_DIR)/3ds" --organize-only

cia: $(CONVERTER) ## Organize 3DS ROMs and create a CIA beside each original (BOOT9=path)
	@$(PYTHON) scripts/convert_3ds.py "$(ROMS_DIR)/3ds" --converter "$(CONVERTER)" --boot9 "$(BOOT9)"

install-converter: .tools/3dsconv/bin/rom-io-3dsconv ## Install 3dsconv in a local Python environment

.tools/3dsconv/bin/rom-io-3dsconv:
	$(PYTHON) -m venv .tools/3dsconv
	.tools/3dsconv/bin/python -m pip install '3dsconv @ git+https://github.com/ihaveamac/3dsconv.git@50a30d292e039a8b315eaccef85a067a8f48c500'
	@printf '%s\n' '#!/bin/sh' 'exec "$$(dirname "$$0")/python" -m 3dsconv.3dsconv "$$@"' > $@
	@chmod +x $@

test: ## Run the server tests
	cd server && $(PYTHON) -m unittest

clean: ## Remove client build artifacts
	$(MAKE) -C client clean
