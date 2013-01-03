#!/bin/bash

if [ $# -ne 3 ]; then
        echo "$0 <srcdir> <dstdir> <dstip>"
        exit 1
fi

psql -d postgres -c "select pg_start_backup('label', true)"
rsync -a $1/* $3:$2 --exclude postmaster.pid
psql -d postgres -c "select pg_stop_backup()"

