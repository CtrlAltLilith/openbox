#!/bin/bash

export VERS=3.7-n
export ARCH=amd64

apt-get update && apt-get -y install gcc \
          gettext automake autoconf autopoint \
          libtool libpango1.0-dev pkg-config \
          libglib2.0-dev libxml2-dev \
          libstartup-notification0-dev \
          xorg-dev libimlib2-dev uuid \
          build-essential devscripts dpkg-dev \
          libpangoxft-1.0-0 docbook-to-man

./autobuild_deb.sh &&
     ( dpkg -i ../openbox_${VERS}_$ARCH.deb ||
         apt-get -y -f install ) &&
     ln -s -f /usr/bin/openbox-session /etc/alternatives/x-window-manager
