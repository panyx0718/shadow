#!/bin/bash

echo "************init environment**********"
ssh $USER@$STANDBY "rm -rf $WORKDIR/data/*"

echo "************create new backup***********"
psql -d postgres -p 5432 -c "select pg_start_backup('label', true)"
rsync -a $WORKDIR/data/* $STANDBY:$WORKDIR/data --exclude postmaster.pid 
psql -d postgres -p 5432 -c "select pg_stop_backup()"

ssh $USER@$STANDBY "cp $WORKDIR/scripts/slave.conf $WORKDIR/data/postgresql.conf"
ssh $USER@$STANDBY "cp $WORKDIR/scripts/recovery2.conf $WORKDIR/data/recovery.conf"
ssh $USER@$STANDBY "rm $WORKDIR/data/pg_xlog"
ssh $USER@$STANDBY "mkdir /mnt/pg_xlog"
ssh $USER@$STANDBY "ln -s /mnt/pg_xlog $WORKDIR/data/pg_xlog"

echo "*********start new standby***********"
ssh $USER@$STANDBY "postgres -D $WORKDIR/data > $WORKDIR/log 2>&1 &"
echo "************done*******************"
