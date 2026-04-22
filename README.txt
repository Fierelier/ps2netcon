A simple PS2 TCP terminal service, meant for exploring and managing the file-system.

Highlights:
* Purely human readable protocol (nc 192.168.0.10 1234)
* Can load IRX modules, like neutrino's, to enable access of new and exotic media
* File transfer can be achieved with 'recv' - see sendfile.sh

I was able to transfer a game onto the internal HDD, formatted as exfat, by first loading the necessary modules:
irx host:/neutrino/modules/iomanX.irx
irx host:/neutrino/modules/fileXio.irx
irx host:/neutrino/modules/bdm.irx
irx host:/neutrino/modules/bdmfs_fatfs.irx
irx host:/neutrino/modules/ata_bd.irx

And then transferring by using sendfile.sh:
./sendfile.sh 192.168.0.1 "my.iso" "mass0:/DVD/my.iso"

In my testing, the speed was about 780 KiB/s.

... but things still seem buggy. It's something I need to look into.

BUGS:
* Can't delete files
* exfat acts permanently weird, after certain actions

TODO:
* Commands for copying and moving files
* Command for launching elf files, preferably with arguments
* Lua scripting?
* The ability to use this without ps2link

THANKS:
* rpc/tcpips/ee-echo sample in PS2SDK
