#!/bin/bash

WORKDIR=$HOME/workspace/c/share

echo "************init environment**********"
rm -rf $WORKDIR/pdata/*
rm -rf /tmp/archive/*

echo "************create new backup***********"
psql -d postgres -p 5431 -c "select pg_start_backup('label', true)"
rsync -a $WORKDIR/sdata/* 127.0.0.1:$WORKDIR/pdata --exclude postmaster.pid 
psql -d postgres -p 5431 -c "select pg_stop_backup()"

cp $WORKDIR/util/master_rep.conf $WORKDIR/pdata/postgresql.conf
cp $WORKDIR/util/recovery2.conf $WORKDIR/pdata/recovery.conf


echo "*********start new standby***********"
postgres -D $WORKDIR/pdata > plog 2>&1 &
echo "************done*******************"
