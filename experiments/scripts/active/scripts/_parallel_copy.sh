#!/bin/bash

if [ $# -ne 1 ]; then
  echo "Usage $0 <data dir>"
  exit 1
fi

rsync -a share/backup/ $1 --exclude=pg_xlog &
id1=$!

rsync -a archive/backup/ archive/ &
id2=$!

sleep 10

wait $id1 $id2
