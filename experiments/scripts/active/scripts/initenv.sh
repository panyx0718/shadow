#!/bin/bash

source global_var.sh

sudo chown -R ec2user /mnt
sudo chgrp -R ec2user /mnt

sudo sysctl kernel.shmmax=29769803776
sudo sysctl kernel.shmall=7340032

sudo mount -t nfs -o nolock $NFSSERVER:$HOME/share $NFSDATA
sudo mount -t nfs -o nolock $NFSSERVER:$HOME/archive $NFSARCHIVE


