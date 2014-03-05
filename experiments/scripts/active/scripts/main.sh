#!/bin/bash

source global_var.sh
echo $NFSSERVER


if [ $# -eq 0 ]; then
	echo "$0 <newbackup|initrep|initenv|initsingleenv|initsingle> [warehouse]"
	exit 1
elif [ $1 == "newbackup" ]; then
	./newbackup.sh
elif [ $1 == "initrep" ]; then
	./initrep.sh $2
elif [ $1 == "initenv" ]; then
	./initenv.sh
elif [ $1 == "initsingleenv" ]; then
	./initsingleenv.sh
elif [ $1 == "initsingle" ]; then
	./initsingle.sh $2
elif [ $1 == "failover" ]; then
	./failover.sh $2
elif [ $1 == "copysingle" ]; then
	./copysingle.sh
fi

exit 0
