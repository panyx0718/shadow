#!/bin/bash

BENDIR=$HOME/benchmarksql/run
export JAVA_HOME=/usr

if [ $# -ne 1 ]; then
	echo "$0 <warehouses>"
	exit 1
fi

echo "load database..."

psql -d postgres -c "create database tpcc;"
cd $BENDIR
./runSQL.sh postgres.properties sqlTableCreates || exit 1
./loadData.sh postgres.properties numWarehouses $1 || exit 1
./runSQL.sh postgres.properties sqlIndexCreates || exit 1

echo "done" 
