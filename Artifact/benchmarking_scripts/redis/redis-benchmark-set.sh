#!/bin/bash
TARGET=$1
#NUM_ITER=10
NUM_ITER=3
#CONCURRENCY=(1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20)
CONCURRENCY=(1)
DATA=(128M)
DATA2=(128M)

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
for DAT in "${DATA[@]}";
do
	echo "Data: $DAT"
	for CONC in "${CONCURRENCY[@]}";
	do
		echo "Concurrency: $CONC"
		for ITER in `seq 1 $NUM_ITER`;
		do
			echo "Iteration: $ITER"
			TMP_RESULT=`redis-benchmark -h 192.168.0.31 -p 1000 -n 1000000 -q -t set -P 1000 -c ${CONC} -d ${DAT}`
			echo $TMP_RESULT >> $TARGET-set-$DAT-$CONC
		done
	done
done
: '
for DAT2 in "${DATA2[@]}";
do
	echo "Data: $DAT2"
	for CONC in "${CONCURRENCY[@]}";
	do
		echo "Concurrency: $CONC"
		for ITER in `seq 1 $NUM_ITER`;
		do
			echo "Iteration: $ITER"
			TMP_RESULT=`redis-benchmark -h 192.168.0.31 -p 1000 -n 1000000 -q -t get -P 1000 -c ${CONC} -d ${DAT2}`
			echo $TMP_RESULT >> $TARGET-get-$DAT2-$CONC
		done
	done
done
'
