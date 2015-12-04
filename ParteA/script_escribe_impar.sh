#!/bin/bash

i=1

while [ $i -lt 100 ]; do
	echo add $i > /proc/modlist
	i=$[$i+2]
	sleep 1
done
