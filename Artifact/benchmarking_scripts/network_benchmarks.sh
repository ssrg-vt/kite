#!/bin/bash
# server (DomU) IP is 192.168.0.31

rm request_$1

echo "Running Ping"
ping 192.168.0.31 -c 100 | tee out
result=`cat out | grep rtt| awk -F/ '{print $5}'`
echo "Ping latency: $result ms" > result_$1
printf "\n" >> result_$1

echo "Running Netperf"
netperf -H 192.168.0.31 -l 100 -t TCP_RR -v 2 -- -o mean_latency > out
result=`sed -n 3p out`
echo "Netperf latency: $result us" >> result_$1
printf "\n" >> result_$1

echo "Running Redis benchmark"
sleep 30 
./redis/redis-benchmark-set.sh $1
./redis/redis-benchmark-get.sh $1
./benchmarking_scripts/redis/redis-parse.sh $1
set=`cat $1-set-128M-average | awk '{printf $2}'`
echo "Redis Set: $set operations/sec" >> result_$1
get=`cat $1-get-128M-average | awk '{printf $2}'`
echo "Redis Get: $get operations/sec" >> result_$1

#echo "Redis Set" >> result_$1
#echo "Thread Throughput(Operations/sec)" >> result_$1
#cat $1-set-128M-average >> result_$1
#printf "\n" >> result_$1

#echo "Redis Get" >> result_$1
#echo "Thread Throughput(Operations/sec)" >> result_$1
#cat $1-get-128M-average >> result_$1
#printf "\n" >> result_$1
rm $1-*

#echo "Running MySQL Sysbench benchmark"
#sleep 30
#echo "MySQL Sysbench benchmark"  >> result_$1
#./sys.script.sh >> result_$1
#printf "\n" >> result_$1

echo "Running MySQL Sysbench benchmark"
sleep 30
rm mysql.tp.txt
ssh user@192.168.0.31 -t /home/user/sys.cpu.sh
scp user@192.168.0.31:/home/user/mysql.cpu.txt .

echo "MySQL sysbench troughput" >> result_$1
echo "Thread Throughput(Operations/sec)" >> result_$1
cat mysql.tp.txt >> result_$1
printf "\n" >> result_$1

echo "MySQL CPU utilization" >> result_$1
cat mysql.cpu.txt >> result_$1
printf "\n" >> result_$1

echo "Runnning Apache benchmark"
sleep 30
ab -n 10000 -c 1 http://192.168.0.31/512k.img > out
echo "Apache benchmark specific example with 512KB" >> result_$1
cat out | grep "Request" >> result_$1
cat out | grep "Transfer rate" >> result_$1
cat out | grep "\[ms\] (mean)" >> result_$1
printf "\n" >> result_$1

sleep 30
echo "Apache benchmark varyring block size" >> result_$1
./ab.script.sh $1
printf "\n" >> result_$1
