#!/bin/bash

threads=(1 5 10 20 40 60 80 100)

rm mysql.cpu.txt

for thread in ${threads[@]}; do
	./sysstat-12.3.3/sar -u -o datafile 1 > out &
	ssh user@192.168.0.20 -t "/home/user/sys.remote.script.sh $thread"
	pkill sar

	numbers=`awk -v user=4 -v sys=6 '{print $user} {print $sys}' out`

	total=0.0
	count=0
	prev=0
	declare -a num_array
	for num in ${numbers[@]}; do
		if [[ $num =~ ^[+-]?[0-9]+\.?[0-9]*$ ]];then
			count=$(($count + 1))
			if [[ $count%2 -eq 0 ]]; then
				temp=$(echo $num + $prev | bc)
				num_array+=($temp)
			else
				prev=$num
			fi
			total=$(echo $num + $total | bc)
		fi
	done
	avg=`bc -l <<< 2*$total/$count`

	sum=0
	for num in ${num_array[@]}; do
		dif=$(awk '{print $1-$2}' <<<"${num} ${avg}")
		sq=$(awk '{print $1*$2}' <<<"${dif} ${dif}")
		sum=$(awk '{print $1+$2}' <<<"${sum} ${sq}")
	done

	div=$(awk '{print 2*$1/$2}' <<<"${sum} ${count}")
	sd=`bc -l <<< "sqrt($div)"`

#	echo $thread $avg $sd>> mysql.cpu.txt
	echo $thread $avg>> mysql.cpu.txt
done
