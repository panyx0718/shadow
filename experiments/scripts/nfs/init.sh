#!/bin/bash

sudo mkfs -t ext3 /dev/xvdg
sudo mkfs -t ext3 /dev/xvdh

./mount_nfs.sh
