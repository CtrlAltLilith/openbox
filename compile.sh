#!bin/bash
#This script is simply meant for quickly recompiling openbox
#Check for pgo data
genflags=(-frecord-gcc-switches -fprofile-generate=$(pwd)/pgo -flto -fdevirtualize-at-ltrans -flto-partition=one -pipe -Ofast -march=native -mtune=native -fgraphite-identity -floop-nest-optimize -fno-semantic-interposition -ftree-vectorize -fvar-tracking-assignments -fno-semantic-interposition -ftree-slp-vectorize -fuse-linker-plugin -fuse-ld=gold)
useflags=(-frecord-gcc-switches -fprofile-use=$(pwd)/pgo -flto -fdevirtualize-at-ltrans -flto-partition=one -pipe -Ofast -march=native -mtune=native -fgraphite-identity -floop-nest-optimize -fno-semantic-interposition -ftree-vectorize -fvar-tracking-assignments -fno-semantic-interposition -ftree-slp-vectorize -fuse-linker-plugin -fuse-ld=gold)
ldflag=(${CFLAGS} -Wl,--hash-style=gnu,--as-needed,-O2,--sort-common,-z,relro,-z,now)
if  [[ ! -d "pgo" ]]; then
    #generate pgo
    echo "setting pgo to generate"
    mkdir pgo
    export CFLAGS="${genflags[@]}"
    else
    echo "setting pgo to use"
    #use pgo
    export CFLAGS="${useflags[@]}"
fi
    export LDFLAGS="${ldflags[@]}"

sudo make uninstall
make clean
./bootstrap
./configure --prefix=/usr --sysconfdir=/etc
make
sudo make install
