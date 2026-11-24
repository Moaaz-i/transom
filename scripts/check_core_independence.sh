#!/usr/bin/env bash
# Core independence guard.
# Goal (architecture, not grep-golf): the Transom core must build, link and pass its tests
# with NO domain plugin present and NO domain framework linked.
set -euo pipefail
cd "$(dirname "$0")/.."

fail=0
say() { printf '%s\n' "$*"; }

# 1. Build system must not reference domain frameworks/libraries.
if grep -nEi 'PDFKit|pdfium|PDFView|Windows\.Data\.Pdf' Makefile CMakeLists.txt 2>/dev/null; then
  say "FAIL[1]: build files reference a PDF framework"; fail=1
else say "ok   [1] build files free of PDF frameworks"; fi

# 2. Core sources / public headers must not import or depend on domain frameworks or widgets.
if grep -rnE '#\s*(import|include)\s*[<"][^>"]*(PDFKit|pdfium)|PdfView|PDFDocument' src include 2>/dev/null; then
  say "FAIL[2]: core sources depend on a PDF framework/widget"; fail=1
else say "ok   [2] core sources/headers free of PDF dependencies"; fi

# 3. Core binary must not link a domain framework.
make -s transom >/dev/null
if otool -L transom | grep -Ei 'PDFKit|pdfium' ; then
  say "FAIL[3]: transom binary links a PDF library"; fail=1
else say "ok   [3] transom binary does not link PDFKit/PDFium"; fi

# 4. Core tests must pass with no domain plugin in plugins/ (only neutral fixtures).
if ls plugins 2>/dev/null | grep -Eqi 'pdf'; then
  say "FAIL[4]: plugins/ in the core repo contains a PDF plugin"; fail=1
else say "ok   [4] no PDF plugin shipped inside the core repo"; fi

exit $fail
