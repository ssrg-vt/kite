#!/bin/bash

host=192.168.0.31
user='username'
pass='user_password'
port=3306
time_in_sec=30

#rm result.txt

THROUGHPUT=`sysbench oltp_read_only --threads=$1 --time=$time_in_sec --mysql-host=$host --mysql-user=$user --mysql-password=$pass --mysql-port=$port --tables=10 --table-size=1000000 --report-interval=1 --db-driver=mysql  run | grep sec | grep -o -E '([0-9]*\.[0-9])\w+'`

total=0.0
for TP in ${THROUGHPUT[@]}; do
	total=$(echo $TP + $total | bc)
done
echo  $1 $total >> mysql.tp.txt

