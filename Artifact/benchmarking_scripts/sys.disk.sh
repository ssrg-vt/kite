#!/bin/bash
#time_in_sec=300
time_in_sec=10
thread=20
#block_size=(16K 64K 256K 1M 4M 8M 32M 64M 128M)
block_size=(16K 64K 256K 8M 64M 128M)

rm sys.disk.result.txt
cd disk
#echo "BS OP TP LAT AVG/SD" > sys.disk.result.txt
echo "Block Throughput" > sys.disk.result.txt
echo "Block Throughput" >> ../$1

sysbench fileio --file-total-size=3G --file-test-mode=rndrw --file-block-size=128M --max-time=$time_in_sec --max-requests=0 prepare > /dev/null

for bs in ${block_size[@]}; do
	sysbench fileio --threads=$thread --file-block-size=$bs --file-total-size=16G --file-test-mode=rndrw --max-time=$time_in_sec --max-requests=0 run > out
	READS=`cat out | grep reads | grep -o -E '([0-9]*\.[0-9])\w+'`
	WRITES=`cat out | grep writes | grep -o -E '([0-9]*\.[0-9])\w+'`
	FSYNCS=`cat out | grep fsyncs | grep -o -E '([0-9]*\.[0-9])\w+'`
	OPS=$(echo $READS + $WRITES + $FSYNCS | bc)

	LAT=`cat out | grep "avg:" | awk '{print $2}'`
	SD=`cat out | grep "execution time" | awk '{print $4}'`

	THROUGHPUT=`cat out | grep MiB | grep -o -E '([0-9]*\.[0-9])\w+'`
	total=0.0
	for TP in ${THROUGHPUT[@]}; do
		total=$(echo $TP + $total | bc)
	done
	echo $bs $total
	#echo  $bs $OPS $total $LAT $SD >> sys.disk.result.txt
	echo  "$bs $total Mbps" >> sys.disk.result.txt
	echo  "$bs $total Mbps" >> ../$1
	sleep 10
done
cd ..
