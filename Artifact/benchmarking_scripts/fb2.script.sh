#!/bin/bash

BENCH=$1
OUTPUT=$2
iterations=(1)

AVG=0.0
average()
{
	SUM=0.0
	INPUT=("$@")
	for i in "${INPUT[@]}";
	do	
		#echo "$i"
		SUM=$(echo "$SUM + $i" | bc)
	done
	NUM=$(echo "${#INPUT[@]}" | bc)
	AVG=`bc -l <<< "$SUM / $NUM"`
}

stdev()
{
	SQSUM=0.0
	INPUT=("$@")
	average ${INPUT[@]}

	for i in "${INPUT[@]}";
	do	
		#echo "$i"
		DIF=$(echo "$i - $AVG" | bc)
		SQ=$(echo "$DIF * $DIF" | bc)
		SQSUM=$(echo "$SQSUM + $SQ" | bc)
	done
	DIV=`bc -l <<< "$SQSUM / $NUM"`
	SD=$(echo "sqrt($DIV)" | bc)
	RSD=`bc -l <<< "$SD * 100 / $AVG"`
}

filter()
{
	rm tp_nums
	avg_tp=`bc <<< "scale=2; $AVG/1"`
	first_sd=`bc <<< "scale=2; $SD/1"`

	hb=`bc -l <<< "$avg_tp + $first_sd"`
	lb=`bc -l <<< "$avg_tp - $first_sd"`

	INPUT=("$@")

	for i in "${INPUT[@]}";
	do	
		#if [[ $i -le $hb ]] || [[ $i -ge $lb ]]
		if (( $(echo "$i >= $lb && $i <=$hb" |bc -l) )) ; then
			echo $i >> tp_nums
		fi
	done
}

bash -c "echo 0 > /proc/sys/kernel/randomize_va_space"
filebench -f $BENCH | tee out
OP=$(cat out | grep "IO" | awk '{print $6}' | grep -o -E '([0-9]*\.[0-99]*)')
TP=$(cat out | grep "IO" | awk '{print $10}' | grep -o -E '([0-9]*\.[0-99]*)')
LAT=$(cat out | grep "IO" | awk '{print $11}' | grep -o -E '([0-9]*\.[0-99]*)')

average ${TP[@]}
stdev ${TP[@]}
filter ${TP[@]}
readarray results < tp_nums
average ${results[@]}
stdev ${results[@]}

tp=`bc <<< "scale=2; $AVG/1"`
rsd=`bc <<< "scale=2; $RSD/1"`

average ${OP[@]}
op=`bc <<< "scale=2; $AVG/1"`

average ${LAT[@]}
lat=`bc <<< "scale=2; $AVG/1"`

echo "Operations=$op per sec, Throughput=$tp mb/sec, Latency=$lat ms/op" >> $OUTPUT
