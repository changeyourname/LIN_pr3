#!/bin/bash

###############################################################################
#
# test_modlist.sh
#
# Script to test if the concurrency in modlist is handled correctly
#
###############################################################################

if [ -d test_logs ]; then
    rm -r test_logs
fi
mkdir test_logs

echo ""
echo " Testing modlist module"
echo " ================================================="

echo " launching some concurrent proccesses"
for w_script in {0..19}; do
    bash script_writer.sh ${w_script} 20 1000 &
done

for r_script in {0..2}; do
    bash script_reader.sh 0 20 ${r_script} &
done

for s_script in {0..3}; do
    bash script_sort.sh 0 3 &
done

JOBS=`jobs -rp`
echo -n " Waiting for the proccesses to end "
while [ ! -z "$JOBS" ]; do
    echo -n "."
    sleep 0.5
    JOBS=`jobs -rp`
done
echo  " DONE"

echo "sort" > /proc/modlist
cat /proc/modlist > test_logs/LAST_LECTURE.log
echo cleanup > /proc/modlist

#for log in `ls test_logs/`; do
#    echo "$log"
#done;


# No number should be repeated in the read_scripts outputs, so we can know
# everything is working right
echo ""
echo " To check if the result is correct, you can read test_logs/LAST_LECTURE.log"
echo " There should not be any repeated number."
echo " Also, all the other files are the lectures realized by the concurrent 'read scripts'"
echo " The same condition of zero-repeated-numbers applies."
