#!/bin/bash
TARGET=$1
NUM_ITER=1
#CONCURRENCY=(1 3 5 7 9 11 13 15 17 19)
CONCURRENCY=(1 3)
DATA2=(32 8192)

average()
{
	SUM=0
	INPUT=("$@")
	for i in "${INPUT[@]}";
	do	
		#echo "$i"
		SUM=$(echo "$SUM + $i" | bc)
	done
	NUM="${#INPUT[@]}"
	AVG=$(echo "$SUM/$NUM" | bc)
}

stdv()
{
	SUM=0
	INPUT=("$@")
	for i in "${INPUT[@]}";
	do	
		echo "$i"
		TMP=$(echo "$i * $i"| bc)
		SUM=$(echo "$SUM + $TMP" | bc)
	done
	NUM="${#INPUT[@]}"
	STD=$(echo "$SUM/$NUM" | bc)
	STD=$(echo "sqrt($STD)" | bc)
}

for DAT2 in "${DATA2[@]}";
do
	echo "Data: $DAT2"
	for CONC in "${CONCURRENCY[@]}";
	do
		echo "Concurrency: $CONC"
		for ITER in `seq 1 $NUM_ITER`;
		do
			echo "Iteration: $ITER"
			memtier_benchmark -s 192.168.0.31 -p 11211 -P memcache_binary -c 10 -t ${CONC} -d ${DAT2} --test-time=120 --out-file=$TARGET-$DAT2-$CONC
		done
	done
done
