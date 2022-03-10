#!/bin/bash
#time_in_sec=300
time_in_sec=60
#threads=(1 5 10 20 40 60 80 100)
threads=(1 5)

rm sys.disk.result.txt
echo "TH OP TP LAT AVG/SD" > sys.disk.result.txt

for thread in ${threads[@]}; do
	sysbench fileio --threads=$thread --file-block-size=10M --file-total-size=150G --file-test-mode=rndrw --max-time=$time_in_sec --max-requests=0 run > out
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
	
	echo $thread $total
	echo  $thread $OPS $total $LAT $SD >> sys.disk.result.txt
done
