# Tests and builds. `make test` runs everything that needs no screen; `make device DEVICE=<address>` the tests on a
# real screen (tests/device_test.py), which put back every setting they change.

DEVICE ?=
SOAK ?= 30

.PHONY: test host python page device device-full soak firmware

test: host python page

# The C++ that needs no screen, with the address and undefined-behaviour sanitizers (ARDUINOJSON_DIR, CXX are used).
host:
	tests/run.sh

# tools/release_site.py: the manifest the screens read.
python:
	python3 -m unittest discover -s tests -p "test_*.py"

# The web page's script (Node 18 or newer).
page:
	node --test tests/test_page.mjs

device:
	@test -n "$(DEVICE)" || { echo "make device DEVICE=<the screen's address>"; exit 2; }
	tests/device_test.py $(DEVICE)

# Also each map drawn whole, and a restart that keeps the settings.
device-full:
	@test -n "$(DEVICE)" || { echo "make device-full DEVICE=<the screen's address>"; exit 2; }
	tests/device_test.py $(DEVICE) --maps --reboot

# Memory over SOAK minutes.
soak:
	@test -n "$(DEVICE)" || { echo "make soak DEVICE=<the screen's address> [SOAK=minutes]"; exit 2; }
	tests/device_test.py $(DEVICE) --soak $(SOAK)

firmware:
	esphome compile skyscope.yaml
