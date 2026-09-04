#!/usr/bin/env bash
# Build the Doxygen API reference: html in build/doxygen/html, warnings in
# build/doxygen/warnings.log, their count on the last line. The output
# directory has to exist before Doxygen opens its warning log, hence this
# wrapper rather than a bare `doxygen`. docs.yml runs the same script and
# publishes the html to GitHub Pages.
set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p build/doxygen
doxygen Doxyfile
count=$(wc -l < build/doxygen/warnings.log)
echo "doxygen: ${count} warning(s), see build/doxygen/warnings.log"
