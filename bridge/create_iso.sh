#!/bin/sh

export PATH="${PATH}:$(pwd)/../rumprun-10g/bin"

rm rumprun-back.bin.iso
rumprun iso back.bin

