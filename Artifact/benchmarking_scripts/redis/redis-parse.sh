#!/bin/bash
TARGET=$1
CONCURRENCY=(1)
#CONCURRENCY=(1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20)
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

for DAT in "${DATA[@]}";
do
	for CONC in "${CONCURRENCY[@]}";
	do
		FILE="${TARGET}-set-${DAT}-${CONC}"
		mac2unix $FILE
		cat $FILE | grep "requests" | awk {'print $2'} > temp.txt 
		readarray THROUGHPUT < temp.txt
		average ${THROUGHPUT[@]}
		stdev ${THROUGHPUT[@]}
#		RESULT="${CONC} ${AVG} ${RSD}"
		RESULT="${CONC} ${AVG}"
		echo $RESULT >> "${TARGET}-set-${DAT}-average"
		rm temp.txt
	done
done

for DAT2 in "${DATA2[@]}";
do
	for CONC in "${CONCURRENCY[@]}";
	do
		FILE="${TARGET}-get-${DAT2}-${CONC}"
		mac2unix $FILE
		cat $FILE | grep "requests" | awk {'print $2'} > temp.txt 
		readarray THROUGHPUT < temp.txt
		average ${THROUGHPUT[@]}
		stdev ${THROUGHPUT[@]}
#		RESULT="${CONC} ${AVG} ${RSD}"
		RESULT="${CONC} ${AVG}"
		echo $RESULT >> "${TARGET}-get-${DAT2}-average"
		rm temp.txt
	done
done
