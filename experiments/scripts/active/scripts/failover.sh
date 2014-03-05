#!/bin/bash

source global_var.sh

if [ $# -eq 0 ]; then
  "give me the primary pid"
  exit 1
fi


kill -9 $1
ssh $USER@$STANDBY "touch /tmp/pg_switch && date +%s > $WORKDIR/failover_start"
