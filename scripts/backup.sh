#!/bin/bash

if [ $# -ne 4 ]; then
        echo "$0 <sourceport> <srcdir> <destip> <dstdir>"
        exit 1
fi

psql -d postgres -p $1 -c "select pg_start_backup('label', true)"
rsync -a $2/* $3:$4 --exclude postmaster.pid
psql -d postgres -p $1 -c "select pg_stop_backup()"

