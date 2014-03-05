#!/bin/bash

echo "*************init environment****************"

rm -rf `find $NFSDATA/ -mindepth 1 -maxdepth 1 -not -name "backup" -not -name "."`
rm -rf `find $NFSARCHIVE/ -mindepth 1 -maxdepth 1 -not -name "backup" -not -name "."`
rm -rf $BENDIR/reports/*


mkdir $NFSDATA/pdata
mkdir $NFSDATA/sdata
chmod 0700 $NFSDATA/pdata
chmod 0700 $NFSDATA/sdata

rm $WORKDIR/data
ln -s $NFSDATA/pdata $WORKDIR/data
ssh $STANDBY "rm $WORKDIR/data; ln -s $NFSDATA/sdata $WORKDIR/data; chmod 0700 $WORKDIR/data"

scp $WORKDIR/scripts/_parallel_copy.sh $NFSSERVER:~
ssh $NFSSERVER "~/_parallel_copy.sh share/pdata"

mv $NFSDATA/pdata/base $NFSDATA/
mv $NFSDATA/pdata/global $NFSDATA/
ln -s $NFSDATA/base $NFSDATA/pdata/base
ln -s $NFSDATA/global $NFSDATA/pdata/global
ln -s $NFSARCHIVE/pg_xlog $NFSDATA/pdata/pg_xlog

ssh $STANDBY "if [ -d /mnt/pg_xlog ]; then rm -rf /mnt/pg_xlog/*; else mkdir /mnt/pg_xlog; fi"


chmod 0700 $WORKDIR/data
postgres -D $WORKDIR/data > $WORKDIR/log 2>&1 &
sleep 10

echo "done" 
