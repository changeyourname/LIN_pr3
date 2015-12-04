#!/bin/bash

i=0

while [ $i -lt 100 ]; do
	cat /proc/modlist
	i=$[$i+1]
	sleep 1
done
