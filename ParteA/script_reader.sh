#!/bin/bash

if [ $# -lt 2 ]; then
    echo "3 params needed start end code"
    exit
fi

i=$1
max=$2
code=$3

while [ $i -lt $max ]; do
	cat /proc/modlist > test_logs/test_read_${code}_${i}.log
    i=$(( i + 1 ))
    sleep 0.2
done
