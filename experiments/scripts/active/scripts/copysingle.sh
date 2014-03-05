#!/bin/bash 

source global_var.sh

echo "***********copydb**************"
rm $WORKDIR/data
ln -s $NFSDATA $WORKDIR/data

rm -rf $BENDIR/reports/*
rm $BENDIR/log
rm -rf `find $NFSDATA/ -mindepth 1 -maxdepth 1 -not -name "backup" -not -name "."`
rm -rf `find $NFSARCHIVE/ -mindepth 1 -maxdepth 1 -not -name "backup" -not -name "."`

scp $WORKDIR/scripts/parallel_copy.sh $NFSSERVER:~
ssh $NFSSERVER "~/parallel_copy.sh share/"

ln -s $NFSARCHIVE/pg_xlog $NFSDATA/pg_xlog

postgres -D $WORKDIR/data > $WORKDIR/log 2>&1 &
sleep 2

echo "*********finish***********"
