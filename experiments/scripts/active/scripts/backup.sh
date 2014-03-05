#!/bin/bash

if [ $# -ne 4 ]; then
        echo "$0 <sourceport> <srcdir> <destip> <dstdir>"
        exit 1
fi

date +%s > protect_ts
echo  "start backup" >> protect_ts
psql -d postgres -p $1 -c "select pg_start_backup('label', true)"

date +%s >> protect_ts
echo "start rsync" >> protect_ts
rsync -a $2/* $3:$4 --exclude postmaster.pid

date +%s >> protect_ts
echo "stop backup" >> protect_ts
psql -d postgres -p $1 -c "select pg_stop_backup()"
date +%s >> protect_ts
echo "finish" >> protect_ts

ssh $USER@$STANDBY "date +%s > $WORKDIR/protect_ts"
