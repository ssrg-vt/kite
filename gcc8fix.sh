#!/bin/sh
gcc "$@" "-Wno-tautological-compare" "-Wno-attributes" "-Wno-uninitialized"
#gcc "$@" "-Wno-cast-function-type" "-Wno-tautological-compare" "-Wno-packed-not-aligned" "-Wno-attributes" "-Wno-uninitialized"
