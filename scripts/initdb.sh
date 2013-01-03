#/bin/bash

WORKDIR=$HOME/workspace/c/share

echo "init working environment..."
rm -rf $WORKDIR/pdata/*
rm -rf $WORKDIR/sdata/*
rm -rf $WORKDIR/share/*

if [ -f /tmp/archive ]; then
	rm -rf /tmp/archive/*
else
	mkdir /tmp/archive
fi

echo "init primary database..."
initdb $WORKDIR/pdata
cd $WORKDIR/pdata
mv base global $WORKDIR/share
ln -s $WORKDIR/share/base base
ln -s $WORKDIR/share/global global

echo "copy configuration files..."
cp $WORKDIR/util/master.conf postgresql.conf
cp $WORKDIR/util/pg_hba.conf pg_hba.conf
cd ..

echo "done" 
