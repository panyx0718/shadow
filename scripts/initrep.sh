#/bin/bash

WORKDIR=$HOME/workspace/c/share

if [ $# -ne 1 ]; then
	echo "$0 <warehouses>"
	exit 1
fi

echo "***********initdb**************"
./initdb.sh

echo "***********loaddb*************"
postgres -D $WORKDIR/pdata > $WORKDIR/plog 2>&1 & 
sleep 2
./loaddb.sh $1 || exit 1
sleep 2
pg_ctl stop -m smart -D $WORKDIR/pdata || exit 1

echo "***********copy replication configuration***********"
cp $WORKDIR/util/master_rep.conf $WORKDIR/pdata/postgresql.conf
postgres -D $WORKDIR/pdata > $WORKDIR/plog 2>&1 &
sleep 2

echo "**********init backup***********"
./initbackup.sh 5432 pdata 127.0.0.1 sdata || exit 1
postgres -D $WORKDIR/sdata > $WORKDIR/slog 2>&1 &
sleep 2

echo "*********finish***********"
