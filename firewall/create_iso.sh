#!/bin/sh

export PATH="${PATH}:$(pwd)/../rumprun-pvdrivers/bin"

rm rumprun-back.bin.iso
rumprun iso back.bin

