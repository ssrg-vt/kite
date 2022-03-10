#!/bin/bash
#time_in_sec=300
time_in_sec=60
thread=20
#block_size=(16K 32K 64K 128K 256K 512K 1M 2M 4M 8M 10M 20M 40M 80M 100M)
block_size=(16K 32K))

rm sys.disk.result.txt
echo "BS OP TP LAT AVG/SD" > sys.disk.result.txt

for bs in ${block_size[@]}; do
	sysbench fileio --threads=$thread --file-block-size=$bs --file-total-size=150G --file-test-mode=rndrw --max-time=$time_in_sec --max-requests=0 run > out
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
	echo  $bs $OPS $total $LAT $SD >> sys.disk.result.txt
done
