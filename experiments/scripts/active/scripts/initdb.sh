#!/bin/bash

source global_var.sh

if [ $# -eq 0 ]; then
	echo "$0 <warehouse>"
	exit 1
fi

echo "***********initdb**************"
rm $WORKDIR/data
ln -s $NFSDATA $WORKDIR/data

rm -rf `find $NFSDATA/ -mindepth 1 -maxdepth 1 -not -name "backup" -not -name "."`
rm -rf `find $NFSARCHIVE/ -mindepth 1 -maxdepth 1 -not -name "backup" -not -name "."`

echo "init primary database..."
mkdir $NFSDATA/sadata
initdb $NFSDATA/sadata

cd $NFSDATA/sadata
mv pg_xlog $NFSARCHIVE
ln -s $NFSARCHIVE/pg_xlog pg_xlog

echo "copy configuration files..."
cp $WORKDIR/scripts/master.conf postgresql.conf
cp $WORKDIR/scripts/pg_hba.conf pg_hba.conf
cd -

echo "***********loaddb*************"
postgres -D $NFSDATA/sadata > $WORKDIR/log 2>&1 &
sleep 2
./_loaddb.sh $1 || exit 1
sleep 2

echo "*********finish***********"
