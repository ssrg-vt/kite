#!/bin/sh

export PATH="${PATH}:$(pwd)/../rumprun-blkback_persistent_strategy/bin"

rm rumprun-back.bin.iso
rumprun iso back.bin

