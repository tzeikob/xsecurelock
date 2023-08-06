#!/bin/bash

# clang-tidy.
if which clang-tidy; then
  set -- \
    'bugprone-*' \
    'cert-*' \
    'clang-analyzer-*' \
    'misc-*' \
    'performance-*' \
    'readability-*' \
    '-cert-env33-c' \
    '-cert-msc30-c' \
    '-cert-msc50-cpp' \
    '-clang-analyzer-alpha.core.FixedAddr' \
    '-clang-analyzer-alpha.core.PointerArithm' \
    '-clang-analyzer-alpha.deadcode.UnreachableCode'
  checks=$(echo "$*" | tr ' ' ,)
  # Try once without extensions.
  clang-tidy -checks="$checks" \
    -extra-arg=-DHELPER_PATH=\"\" \
    -extra-arg=-DDOCS_PATH=\"\" \
    -extra-arg=-DAUTH_EXECUTABLE=\"\" \
    -extra-arg=-DAUTHPROTO_EXECUTABLE=\"\" \
    -extra-arg=-DGLOBAL_SAVER_EXECUTABLE=\"\" \
    -extra-arg=-DSAVER_EXECUTABLE=\"\" \
    -extra-arg=-DPAM_SERVICE_NAME=\"\" \
    *.[ch] */*.[ch]
  # Try again with all extensions.
  clang-tidy -checks="$checks" \
    -extra-arg=-I/usr/include/freetype2 \
    -extra-arg=-DHELPER_PATH=\"\" \
    -extra-arg=-DDOCS_PATH=\"\" \
    -extra-arg=-DAUTH_EXECUTABLE=\"\" \
    -extra-arg=-DAUTHPROTO_EXECUTABLE=\"\" \
    -extra-arg=-DGLOBAL_SAVER_EXECUTABLE=\"\" \
    -extra-arg=-DSAVER_EXECUTABLE=\"\" \
    -extra-arg=-DPAM_SERVICE_NAME=\"\" \
    -extra-arg=-DHAVE_DPMS_EXT \
    -extra-arg=-DHAVE_XCOMPOSITE_EXT \
    -extra-arg=-DHAVE_XFIXES_EXT \
    -extra-arg=-DHAVE_XKB_EXT \
    -extra-arg=-DHAVE_XFT_EXT \
    -extra-arg=-DHAVE_XRANDR_EXT \
    -extra-arg=-DHAVE_XSCREENSAVER_EXT \
    *.[ch] */*.[ch]
fi

# CPPCheck.
if which cppcheck; then
  cppcheck --enable=all --inconclusive --std=posix  .
fi

# Clang Analyzer.
if which scan-build; then
  make clean
  scan-build make
fi

# Build for Coverity Scan.
if which cov-build; then
  make clean
  rm -rf cov-int
  cov-build --dir cov-int make
  tar cvjf cov-int.tbz2 cov-int/
  rm -rf cov-int
  rev=$(git describe --always --dirty)
  curl --form token="$COVERITY_TOKEN" \
    --form email="$COVERITY_EMAIL" \
    --form file=@cov-int.tbz2 \
    --form version="$rev" \
    --form description="$rev" \
    https://scan.coverity.com/builds?project=xsecurelock
  rm -f cov-int.tbz2
fi
