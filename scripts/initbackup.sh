#/bin/bash

WORKDIR=$HOME/workspace/c/share

echo "creating backup..."
cp $WORKDIR/util/backup.sh $WORKDIR/pdata
cd pdata
./backup.sh $WORKDIR/pdata/ $WORKDIR/sdata 127.0.0.1 || exit 1
cd ..

echo "copying configuration files..."
cp $WORKDIR/util/slave.conf $WORKDIR/sdata/postgresql.conf
cp $WORKDIR/util/recovery.conf $WORKDIR/sdata/recovery.conf


echo "done" 
