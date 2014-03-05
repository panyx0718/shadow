#!/bin/bash


if [ $# -ne 4 ]; then
	echo "$0 <sorceport> <srcdir> <destip> <destdir>"
	exit 1
fi

sleep 20

echo "creating backup..."
cp $WORKDIR/scripts/_backup.sh $WORKDIR/data
cd $WORKDIR/data
./_backup.sh $1 $WORKDIR/$2 $3 $WORKDIR/$4 || exit 1
cd ..

echo "copying configuration files..."
ssh $USER@$STANDBY "cp $WORKDIR/scripts/slave.conf $WORKDIR/data/postgresql.conf"
ssh $USER@$STANDBY "cp $WORKDIR/scripts/recovery.conf $WORKDIR/data/recovery.conf"
ssh $USER@$STANDBY "cp $WORKDIR/scripts/pg_hba.conf $WORKDIR/data/pg_hba.conf"

ssh $USER@$STANDBY "rm $WORKDIR/data/pg_xlog"
ssh $USER@$STANDBY "ln -s /mnt/pg_xlog $WORKDIR/data/pg_xlog"

ssh $USER@$STANDBY "postgres -D $WORKDIR/data > $WORKDIR/log 2>&1 &"
sleep 2

echo "done" 
