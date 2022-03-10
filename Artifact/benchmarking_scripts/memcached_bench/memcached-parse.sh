#!/bin/bash
TARGET=$1
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

stdev()
{
	SUM=0
	COUNT=0
	INPUT=("$@")
	average ${INPUT[@]}

	for i in "${INPUT[@]}";
	do	
		#echo "$i"
		COUNT=$(echo "$COUNT + 1" | bc)
		DIF=$(echo "$i - $AVG" | bc)
		SQ=$(echo "$DIF * $DIF" | bc)
		SUM=$(echo "$SUM + $SQ" | bc)
	done
	DIV=$(echo "$SUM / $COUNT" | bc)
	SD=$(echo "sqrt($DIV)" | bc)
	RSD=`bc -l <<< "$SD * 100 / $AVG"`
}

for DAT2 in "${DATA2[@]}";
do
	rm latency.txt
	rm ${TARGET}-${DAT2}-average

	for CONC in "${CONCURRENCY[@]}";
	do
		FILE="${TARGET}-${DAT2}-${CONC}"
		cat $FILE | grep "Totals" | awk {'print $2'} > temp.txt 
		LAT=`cat $FILE | grep "Totals" | awk {'print $5'}`
		echo $CONC $LAT >> latency.txt 
		readarray THROUGHPUT < temp.txt
		average ${THROUGHPUT[@]}
		stdev ${THROUGHPUT[@]}
		RESULT="${CONC} ${AVG} ${RSD}"
		#rm result/${TARGET}-${DAT2}-average
		echo $RESULT >> "${TARGET}-${DAT2}-average"
		rm temp.txt
	done
done
