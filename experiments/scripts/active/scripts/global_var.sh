#!/bin/bash

export BENDIR=$HOME/benchmarksql/run
export WORKDIR=$HOME/pg_hack/ha
export NFSDATA=$WORKDIR/share
export NFSARCHIVE=$WORKDIR/archive
export PRIMARY=ip-10-39-82-114
export STANDBY=ip-10-119-39-140
export NFSSERVER=ip-10-243-2-95
export USER=`whoami`


echo "$WORKDIR" "$STANDBY" "$NFSSERVER"

if [ -n "$STANDBY" ]; then
  ssh-copy-id $STANDBY
else
  echo "empty standby address"
fi

if [ -n "$NFSSERVER" ]; then
  ssh-copy-id $NFSSERVER
else
  echo "empty nfs address"
fi

