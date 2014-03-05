#!/bin/bash

if [ $# -ne 3 ]; then
  echo "<file> <warmup> <exp_duration>"
  exit 1
fi

source global_var.sh

file=$1
warm_up=`expr 60 \* $2`
exp_duration=`expr 60 \* $3`
total_duration=`expr $warm_up + $exp_duration`


iostat -xk 10 > $WORKDIR/exp/$file &
iostat_pid=$!

nfsiostat 10 > $WORKDIR/exp/${file}_2 &
nfsiostat_pid=$!


if [ -n "$STANDBY" ]; then
    ssh -f $STANDBY "iostat -xk 10 > $WORKDIR/exp/${file}_8 & id=\$!; sleep $total_duration; kill \$id"
    ssh -f $STANDBY "nfsiostat 10 > $WORKDIR/exp/${file}_9 & id=\$!; sleep $total_duration; kill \$id"
fi

ssh -f $NFSSERVER "iostat -xk 10 > ~/exp/${file}_3 & id=\$!; sleep $total_duration; kill \$id"

echo "***************warm up**************"
sleep $warm_up

echo "**************start droping cache**************"
run=0
freq=5
while [ $run -lt $exp_duration ]
do
  sleep $freq
  run=`expr $run + $freq`
  sudo ./drop_cache.sh
  
  if [ -n "$STANDBY" ]; then
    ssh $STANDBY "/home/ec2user/pg_hack/ha/scripts/drop_cache.sh"
  fi
  echo "buffer cleared"
done

kill -9 $iostat_pid $nfsiostat_pid
mv $BENDIR/reports/* $WORKDIR/exp/${file}_4
mv $BENDIR/log $WORKDIR/exp/${file}_5
cp $WORKDIR/log $WORKDIR/exp/${file}_6
cp $WORKDIR/data/postgresql.conf $WORKDIR/exp/${file}_7
