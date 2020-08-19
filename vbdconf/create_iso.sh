#!/bin/sh

export PATH="${PATH}:$(pwd)/../rumprun-blkddom/bin"

rm rumprun-back.bin.iso
rumprun iso back.bin

