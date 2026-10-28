# Tests and builds. `make test` runs everything that needs no screen; `make device DEVICE=<address>` the tests on a
# real screen (tests/device_test.py), which put back every setting they change.

DEVICE ?=
SOAK ?= 30
# With CHURN (minutes), the soak changes the map and range that often, to catch what a change leaves behind.
CHURN ?= 0

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

# Also each map drawn whole, a restart that keeps the settings, and the screen going off and waking at a tap.
device-full:
	@test -n "$(DEVICE)" || { echo "make device-full DEVICE=<the screen's address>"; exit 2; }
	tests/device_test.py $(DEVICE) --maps --reboot --sleep

# Memory over SOAK minutes (and with CHURN, the map changed every CHURN minutes).
soak:
	@test -n "$(DEVICE)" || { echo "make soak DEVICE=<the screen's address> [SOAK=minutes] [CHURN=minutes]"; exit 2; }
	tests/device_test.py $(DEVICE) --no-touch --soak $(SOAK) --churn $(CHURN)

firmware:
	esphome compile skyscope.yaml
