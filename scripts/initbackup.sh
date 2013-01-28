#/bin/bash

WORKDIR=$HOME/workspace/c/share

if [ $# -ne 4 ]; then
	echo "$0 <sorceport> <srcdir> <destip> <destdir>"
	exit 1
fi

echo "creating backup..."
cp $WORKDIR/util/backup.sh $WORKDIR/pdata
cd pdata
./backup.sh $1 $WORKDIR/$2 $3 $WORKDIR/$4 || exit 1
cd ..

echo "copying configuration files..."
cp $WORKDIR/util/slave.conf $WORKDIR/sdata/postgresql.conf
cp $WORKDIR/util/recovery.conf $WORKDIR/sdata/recovery.conf


echo "done" 
