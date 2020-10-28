#!/bin/sh

export PATH="${PATH}:$(pwd)/../rumprun-netddom/bin"

rm rumprun-back.bin.iso
rumprun iso back.bin

