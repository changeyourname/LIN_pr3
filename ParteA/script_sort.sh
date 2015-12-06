#!/bin/bash

if [ $# -lt 2 ]; then
    echo "2 params needed start end"
    exit
fi 
i=$1
max=$2

while [ $i -lt $max ]; do
	echo "sort" > /proc/modlist
    i=$(( i + 1 ))
    sleep 1
done
