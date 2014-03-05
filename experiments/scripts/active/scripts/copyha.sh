#!/bin/bash


source global_var.sh

if [[ -z "$WORKDIR" ]]; then
  echo "Error: WORKDIR not initialized"
  exit 1
fi

echo "***********copy standalone db**************"
./_copyha.sh

echo "**********init backup***********"
./_initbackup.sh 5432 data $STANDBY data || exit 1

echo "*********finish***********"

