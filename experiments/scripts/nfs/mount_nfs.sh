#!/bin/bash

sudo mount /dev/xvdg share
sudo mount /dev/xvdh archive

sudo chown -R ec2user share
sudo chown -R ec2user archive
sudo chgrp -R ec2user share
sudo chgrp -R ec2user archive

chmod 0700 share

sudo service nfs-kernel-server restart
sudo service portmap restart
