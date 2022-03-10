#!/bin/bash

#ab -n 100 -c 10 http://192.168.0.31/1k.img

requests=10000
#concurrency=(5 10 20 40 80 100)
concurrency=(5)
#size=(512 1k 2k 4k 8k 16k 32k 64k 128k)
size=(256k)

rm result.txt mean.txt sd.txt

for s in ${size[@]}; do
	output=""
	for c in ${concurrency[@]}; do
#		ab=$(ab -n $requests -c $c http://192.168.0.31/$s.img)
		ab -n $requests -c $c http://192.168.0.31/$s.img > out
		cat out | grep Total: | awk {'print $3'} >> mean.txt
		cat out | grep Total: | awk {'print $4'} >> sd.txt
#		tr=$(grep "Transfer rate" <<< "$ab")
		tr=$(cat out | grep "Transfer rate")
		result=$(echo $tr | grep -o -E '([0-9]*\.[0-9])\w+')
		output="${output} $result"
		sleep 2
	done
	echo $output >> "result.txt"
done

#output format

# s/c--> 5 10 20 40 80 100 

# 4K
#
# 8K
#
# 16K
#
# 32K
#
# 64K
