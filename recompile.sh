#!bin/bash 
#This script is simply meant for quickly recompiling openbox
sudo make uninstall 
make clean 
./bootstrap
./configure --prefix=/usr --sysconfdir=/etc
make
sudo make install