#!/bin/sh
# Builds and runs the host tests (no ESP-IDF needed): sh firmware/test/run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
cc -std=c11 -Wall -Wextra -Werror -I"$here/../main" "$here/../main/knock.c" "$here/test_knock.c" -o "$here/test_knock"
"$here/test_knock"
