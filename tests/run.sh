#!/bin/sh
# Builds and runs the host tests (tests/test_host.cpp). ArduinoJson is the one header they need: it is fetched once
# into tests/build, or taken from $ARDUINOJSON_DIR. Any arguments are feed replies (JSON files) to read as well.
set -e
here=$(cd "$(dirname "$0")" && pwd)
build="$here/build"
mkdir -p "$build"
include="${ARDUINOJSON_DIR:-$build}"
if [ ! -f "$include/ArduinoJson.h" ]; then
  curl -fsSL -o "$build/ArduinoJson.h" https://github.com/bblanchon/ArduinoJson/releases/download/v7.4.3/ArduinoJson-v7.4.3.h
  include="$build"
fi
${CXX:-c++} -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fsanitize=address,undefined -I"$include" \
  "$here/test_host.cpp" -o "$build/test_host"
"$build/test_host" "$@"
