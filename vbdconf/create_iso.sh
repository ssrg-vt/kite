#!/bin/sh

export PATH="${PATH}:$(pwd)/../rumprun/bin"

rm rumprun-back.bin.iso
rumprun iso back.bin

