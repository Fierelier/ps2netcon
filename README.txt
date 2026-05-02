A simple PS2 TCP terminal service, meant for exploring and managing the file-system.

HIGHLIGHTS:
* Purely human readable protocol
* Can load IRX modules, like neutrino's, to enable access of new and exotic media
* File transfer can be achieved with 'recv' - see sendfile.sh

You can connect by using 'socat - TCP:192.168.0.10:1234'.

I was able to transfer a game onto the internal HDD, formatted as exfat, by first loading the necessary modules:
irx mc0:/APPS/neutrino/modules/iomanX.irx
irx mc0:/APPS/neutrino/modules/bdm.irx
irx mc0:/APPS/neutrino/modules/bdmfs_fatfs.irx
irx mc0:/APPS/neutrino/modules/ata_bd.irx

And then transferring by using sendfile.sh:
./sendfile.sh 192.168.0.1 "my.iso" "mass0:/DVD/my.iso"

In my testing, the speed was over 4.0 MiB/s, and could reach and maintain over 5.0 MiB/s given some time.

CONFIGURATION:
The network configuration is in mc0:/SYS-CON/IPCONFIG.DAT. You can create it using LaunchELF.

If you want to create the config manually, the format of the network configuration file looks like this:
IP NETMASK GATEWAY

* IP: IP of your PS2 - choose one
* NETMASK: Netmask of your network
* GATEWAY: IP of your router

Here is a valid IPCONFIG.DAT for example:
192.168.0.10 255.255.255.0 192.168.0.1

COMMANDS:
* exit - leave session
* reset - restart system
* help - this help
* cd - change working directory
* mkdir - make directory
* rmdir - remove directory
* rm - remove file
* mv - BROKEN. move/rename file
* pwd - print working directory
* ls - list files
* irx - load IRX module
* elf - BROKEN. launch ELF file
* recv - receive file

TODO:
* Commands for copying files
* Command for launching elf files, preferably with arguments
* Lua scripting?

THANKS:
* rpc/tcpips/ee-echo sample in PS2SDK
* network/tcpip-basic sample in PS2SDK
