#!/bin/bash

mkdir disk
rm disk/*
./refresh.sh

rm result_$1

echo "Running DD write operation"
{ time sudo dd if=/dev/zero of=disk/10gb bs=1G count=10; } 2> out;
write=`cat out | grep real | awk {'print $2'}`
echo "DD write time $write" > result_$1
printf "\n" >> result_$1
sleep 60

echo "Running DD read operation"
{ time sudo dd if=disk/10gb of=/dev/null bs=1G count=10; } 2> out;
read=`cat out | grep real | awk {'print $2'}`
echo "DD read time $read" >> result_$1
printf "\n" >> result_$1
sleep 60

./refresh.sh
echo "Running filebench Fileserver benchmark"
echo "Filebench Fileserver:" >> result_$1
./fb2.script.sh fileserver.f result_$1
printf "\n" >> result_$1
sleep 60

./refresh.sh
echo "Running filebench Webserver benchmark"
echo "Filebench Webserver:" >> result_$1
./fb2.script.sh webserver.f result_$1
printf "\n" >> result_$1
sleep 60

./refresh.sh
echo "Running filebench MongoDB benchmark"
echo "Filebench MongoDB:" >> result_$1
./fb2.script.sh mongo.f result_$1
printf "\n" >> result_$1
sleep 60

./refresh.sh
echo "Running sysbench varying block size"
echo "Sysbench varying block size" >> result_$1
./sys.disk.sh result_$1
#cat disk/sys.disk.result.txt >> result_$1
#printf "\n" >> result_$1
#sleep 60

umount disk
