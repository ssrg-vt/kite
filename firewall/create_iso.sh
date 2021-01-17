#!/bin/sh

export PATH="${PATH}:$(pwd)/../rumprun-netfront/bin"

rm rumprun-back.bin.iso
rumprun iso back.bin

