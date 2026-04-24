A simple PS2 TCP terminal service, meant for exploring and managing the file-system.

Highlights:
* Purely human readable protocol (socat - TCP:192.168.0.10:1234)
* Can load IRX modules, like neutrino's, to enable access of new and exotic media
* File transfer can be achieved with 'recv' - see sendfile.sh

I was able to transfer a game onto the internal HDD, formatted as exfat, by first loading the necessary modules:
irx mc0:/neutrino/modules/iomanX.irx
irx mc0:/neutrino/modules/bdm.irx
irx mc0:/neutrino/modules/bdmfs_fatfs.irx
irx mc0:/neutrino/modules/ata_bd.irx

And then transferring by using sendfile.sh:
./sendfile.sh 192.168.0.1 "my.iso" "mass0:/DVD/my.iso"

In my testing, the speed was over 4.0 MiB/s

BUGS:
* Can't delete files

TODO:
* Allow configuring network setup
* Commands for copying and moving files
* Command for launching elf files, preferably with arguments
* Lua scripting?

THANKS:
* rpc/tcpips/ee-echo sample in PS2SDK
* network/tcpip-basic sample in PS2SDK
