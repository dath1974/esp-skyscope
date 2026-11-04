# esp-skyscope build
#
#   make                  the firmware and the tests that need no hardware;
#                         with DEVICE=<ESP32 IP Address>, test-esp32 too
#
# Firmware, for BOARD=<one of boards/*.yaml> [waveshare-amoled-241]
#   make firmware         compile skyscope.yaml with ESPHOME_VERSION
#   make firmware-all     compile it for every board, as a release does
#   make firmware-flash   compile it and install it on DEVICE=<address or
#                         serial port>; without DEVICE, ESPHome asks which
#   make firmware-esphome install ESPHome ESPHOME_VERSION
#   make firmware-esphome-version
#                         print ESPHOME_VERSION
#
# Tests - No Hardware Required
#   make test             test-host, test-python and test-page: everything
#                         that needs no screen
#   make test-host        the C++, with the address and undefined-behaviour
#                         sanitizers
#   make test-python      tools/release_site.py and the mock (tests/mock_feed.py)
#   make test-page        the web page's script (Node 18 or newer)
#   make test-markers     every kind of aircraft drawn on a sheet,
#                         tests/build/markers.png
#
# Tests - Real ESP32 Hardware Needed
# Must specify DEVICE=<ESP32 IP Address>. All but test-esp32-public run
# against a mock here (tests/mock_feed.py, port PORT, 8095 unless given),
# asking nothing of a public service, so each run goes the same way on every
# board. Screens tested at once each need their own PORT.
#   make test-esp32       the tests
#   make test-esp32-full  also each map drawn whole, a restart and sleep
#   make test-esp32-mock  the mock's receiver and update server, and every
#                         failure of each; INSTALL=<firmware.ota.bin> to
#                         install that firmware
#   make test-esp32-soak  memory over SOAK minutes [SOAK=30] [CHURN=0]
#   make test-esp32-public  the public services themselves, at a test place
#
# Release, in order (release.yml does the rest once the tag is pushed)
#   make release-version VERSION=<x.y.z>
#                         set version in skyscope.yaml and commit it
#   make release-tag MSG="What changed, in one line"
#                         check, build and test, then tag v<version>
#   make release          push main and the tag, which publishes it
#
# Housekeeping
#   make clean            remove what the build and tests leave behind
#                         (the ESPHome toolchain is kept)
#   make help             print this list

DEVICE ?=
# The mock's port for the tests on a screen: another for each screen tested at once.
PORT ?= 8095
# The board a firmware is built for (boards/<BOARD>.yaml), and every board.
BOARD ?= waveshare-amoled-241
BOARDS := $(basename $(notdir $(wildcard boards/*.yaml)))
SOAK ?= 30
# With CHURN (minutes), the soak changes the map and range that often, to
# catch what a change leaves behind.
CHURN ?= 0
# The ESPHome the firmware is built with, here and for a release
# (release.yml): `make firmware-esphome` installs it.
ESPHOME_VERSION := 2026.10.1
# The firmware's version, as release.yml reads it: a release's tag is
# v<this>.
SKYSCOPE_VERSION = $(shell sed -nE 's/^  version: "?([^"]+)"?$$/\1/p' \
	skyscope.yaml | head -1)
# The tag's message, from the environment so that quotes in it are safe.
export MSG

.PHONY: all firmware firmware-all firmware-flash firmware-esphome firmware-esphome-version \
	test test-host test-python test-page test-markers \
	test-esp32 test-esp32-full test-esp32-mock test-esp32-soak test-esp32-public \
	release-version release-tag release clean help

all: firmware test $(if $(DEVICE),test-esp32)

firmware:
	@v=$$(esphome version | sed 's/.* //'); \
	test "$$v" = "$(ESPHOME_VERSION)" || \
	  { echo "ESPHome $$v, not $(ESPHOME_VERSION): make firmware-esphome"; \
	    exit 2; }
	@test -f boards/$(BOARD).yaml || \
	  { echo "No boards/$(BOARD).yaml: BOARD is one of $(BOARDS)"; exit 2; }
	@want=$$(sed -nE 's|.*network_adapter_[a-z0-9]+-([0-9.]+)\.bin.*|\1|p' \
	  boards/$(BOARD).yaml | head -1); \
	test -z "$$want" || { \
	  py=$$(head -1 "$$(command -v esphome)" | sed 's/^#!//'); \
	  have=$$($$py tools/esp_hosted_version.py) || exit 2; \
	  test "$$want" = "$$have" || \
	    { echo "boards/$(BOARD).yaml carries ESP-Hosted $$want for its" \
	      "Wi-Fi chip; this ESPHome builds $$have:" \
	      "boards/esp-hosted/README.md"; exit 2; }; }
	esphome -s board $(BOARD) compile skyscope.yaml

firmware-all:
	for b in $(BOARDS); do $(MAKE) firmware BOARD=$$b || exit 1; done

# Over Wi-Fi to an address, or over USB to a serial port (/dev/ttyACM0).
firmware-flash: firmware
	esphome -s board $(BOARD) upload skyscope.yaml $(if $(DEVICE),--device $(DEVICE))

# With pipx where there is one (as ESPHome suggests), else with pip.
firmware-esphome:
	if command -v pipx >/dev/null; then \
	  pipx install --force esphome==$(ESPHOME_VERSION); \
	else pip install esphome==$(ESPHOME_VERSION); fi

firmware-esphome-version:
	@echo $(ESPHOME_VERSION)

# What the firmware build, the tests and Python leave behind. The toolchain
# ESPHome fetched (.esphome/.espressif) and what it knows of the screen
# (.esphome/storage) stay, so the next build need not download them again.
clean:
	rm -rf tests/build .esphome/build .esphome/idedata
	find . -name __pycache__ -type d -prune -exec rm -rf {} +

# The comment at the top of this file, so the list has one home.
help:
	@sed -n '/^#/!q; s/^# \{0,1\}//p' $(firstword $(MAKEFILE_LIST))

test: test-host test-python test-page

# The C++ that needs no screen, with the address and undefined-behaviour
# sanitizers (ARDUINOJSON_DIR, CXX are used).
test-host:
	tests/run.sh

# tools/release_site.py, the manifest the screens read, and tests/mock_feed.py,
# what the tests on a real screen are served.
test-python:
	python3 -m unittest discover -s tests -p "test_*.py"

# The web page's script (Node 18 or newer).
test-page:
	node --test tests/test_page.mjs

# Every kind of aircraft as the screen draws it, on a sheet
# (tests/build/markers.png), to look at without a screen.
test-markers:
	mkdir -p tests/build
	$${CXX:-c++} -std=c++20 -O2 tools/markers.cpp -o tests/build/markers
	tests/build/markers tests/build/markers.png

test-esp32:
	@test -n "$(DEVICE)" || \
	  { echo "make test-esp32 DEVICE=<the screen's address>"; exit 2; }
	tests/device_test.py $(DEVICE) --port $(PORT)

# The public services the mock stands in for, each asked by the screen at a
# test place: that they still answer as the screen expects.
test-esp32-public:
	@test -n "$(DEVICE)" || \
	  { echo "make test-esp32-public DEVICE=<the screen's address>"; exit 2; }
	tests/device_test.py $(DEVICE) --public

# Also each map drawn whole, a restart that keeps the settings, and the
# screen going off and waking at a tap.
test-esp32-full:
	@test -n "$(DEVICE)" || \
	  { echo "make test-esp32-full DEVICE=<the screen's address>"; exit 2; }
	tests/device_test.py $(DEVICE) --port $(PORT) --maps --reboot --sleep

# The mock's receiver and update server: known planes, a busy sky, every
# failure a feed or an update server can have, and installs that must fail;
# with INSTALL=<firmware.ota.bin> that firmware installed from it.
test-esp32-mock:
	@test -n "$(DEVICE)" || \
	  { echo "make test-esp32-mock DEVICE=<the screen's address>"; exit 2; }
	tests/device_test.py $(DEVICE) --port $(PORT) --no-touch --mock \
	  $(if $(INSTALL),--install $(INSTALL))

# Memory over SOAK minutes (and with CHURN, the view changed every CHURN
# minutes), under the mock's circling planes.
test-esp32-soak:
	@test -n "$(DEVICE)" || \
	  { echo "make test-esp32-soak DEVICE=<the screen's address>" \
	    "[SOAK=minutes] [CHURN=minutes]"; exit 2; }
	tests/device_test.py $(DEVICE) --port $(PORT) --soak $(SOAK) --churn $(CHURN)

# Only skyscope.yaml is committed, and only when it has no other changes.
release-version:
	@echo "$(VERSION)" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$$' || \
	  { echo "make release-version VERSION=<x.y.z>"; exit 2; }
	@test "$(VERSION)" != "$(SKYSCOPE_VERSION)" || \
	  { echo "skyscope.yaml already says $(VERSION)"; exit 2; }
	@git diff --quiet HEAD -- skyscope.yaml || \
	  { echo "skyscope.yaml has changes of its own: commit them first"; \
	    exit 2; }
	sed -i -E '0,/^  version: /s/^  version: .*/  version: "$(VERSION)"/' \
	  skyscope.yaml
	@test "$$(sed -nE 's/^  version: "?([^"]+)"?$$/\1/p' skyscope.yaml | \
	  head -1)" = "$(VERSION)" || \
	  { echo "skyscope.yaml's version not set"; exit 1; }
	git commit -m "Version $(VERSION)" -- skyscope.yaml

# Refuses unless everything committed is on main, nothing of origin's main
# is missing, and v<version> is new here and on origin; then the firmware
# of every board and the tests (test-esp32 too, with DEVICE). Undone with git
# tag -d.
release-tag:
	@test -n "$$MSG" || \
	  { echo 'make release-tag MSG="What changed, in one line"'; exit 2; }
	@test -n "$(SKYSCOPE_VERSION)" || \
	  { echo "No version in skyscope.yaml"; exit 2; }
	@test "$$(git symbolic-ref --short HEAD)" = main || \
	  { echo "Not on main"; exit 2; }
	@test -z "$$(git status --porcelain --untracked-files=no)" || \
	  { echo "Uncommitted changes: commit or stash them first"; exit 2; }
	git fetch origin main
	@git merge-base --is-ancestor origin/main HEAD || \
	  { echo "origin's main has commits this one lacks: pull first"; \
	    exit 2; }
	@! git rev-parse -q --verify "refs/tags/v$(SKYSCOPE_VERSION)" \
	  >/dev/null || \
	  { echo "v$(SKYSCOPE_VERSION) is already tagged here:" \
	    "make release-version VERSION=<x.y.z>"; exit 2; }
	@! git ls-remote --exit-code --tags origin \
	  "refs/tags/v$(SKYSCOPE_VERSION)" >/dev/null || \
	  { echo "v$(SKYSCOPE_VERSION) is already on origin"; exit 2; }
	$(MAKE) firmware-all all
	git tag -a "v$(SKYSCOPE_VERSION)" -m "$$MSG"

# Asks first: once pushed, the release is built and offered to every screen.
release:
	@git rev-parse -q --verify "refs/tags/v$(SKYSCOPE_VERSION)" \
	  >/dev/null || \
	  { echo "No tag v$(SKYSCOPE_VERSION): make release-tag MSG=..."; \
	    exit 2; }
	@test "$$(git symbolic-ref --short HEAD)" = main || \
	  { echo "Not on main"; exit 2; }
	@test "$$(git rev-parse "v$(SKYSCOPE_VERSION)^{commit}")" = \
	  "$$(git rev-parse HEAD)" || \
	  { echo "v$(SKYSCOPE_VERSION) is not this commit"; exit 2; }
	@git tag -l --format='v$(SKYSCOPE_VERSION): %(contents:subject)' \
	  "v$(SKYSCOPE_VERSION)"
	@printf 'Push main and v%s to origin, publishing it? [y/N] ' \
	  "$(SKYSCOPE_VERSION)"; read a; test "$$a" = y || \
	  { echo "Not pushed"; exit 1; }
	git push --atomic origin main "refs/tags/v$(SKYSCOPE_VERSION)"
