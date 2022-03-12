#!/bin/bash

umount disk
yes | mkfs.ext4 /dev/xvdb
mount /dev/xvdb disk
