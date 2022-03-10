#!/bin/bash

host=192.168.0.31
user='user'
pass='a'
port=3306
time_in_sec=60
threads=(5)
#threads=(1 5 10 20 40 60 80 100)

rm result.txt latency.txt sd.txt

for thread in ${threads[@]}; do
	sysbench oltp_read_only --threads=$thread --time=$time_in_sec --mysql-host=$host --mysql-user=$user --mysql-password=$pass --mysql-port=$port --tables=10 --table-size=1000000 --report-interval=1 --db-driver=mysql  run > out
	THROUGHPUT=cat out | grep sec | grep -o -E '([0-9]*\.[0-9])\w+'
	cat out | grep "avg:" | awk '{print $2}' > latency.txt
	cat out | grep "execution time" | awk '{print $4}' > sd.txt
	total=0.0
	for TP in ${THROUGHPUT[@]}; do
		total=$(echo $TP + $total | bc)
	done
	echo  $thread $total >> result.txt
done
