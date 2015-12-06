#!/bin/bash

if [ $# -lt 3 ]; then
    echo "3 params needed <start> <suma> <end>"
    exit
fi

i=$1
j=$2
max=$3

while [ $i -lt $max ]; do
	echo "add $i" > /proc/modlist
	i=$(( i + j ))
done
