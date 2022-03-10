# Kite: Lightweight Critical Service Domains

Kite implements rumprun-smp based network and storage driver domains for Xen.

## Setup (physical machine)

###### Xen: 
First, install Ubuntu 18.04 LTS on a 64bit x86 machine. Please select “Use LVM with the new Ubuntu installation”. Then, install the Xen hypervisor and reboot the machine;
GRUB should automatically boot Xen and launch Dom0:

```
# apt install xen-hypervisor-amd64
```

###### PCI passthrough: 
Find BDF numbers of the available PCI devices (NIC, NVMe) using the lspci command. Then, add the corresponding device to the PCI assignable
list, where xx:xx.x represents the BDF number:

```
# modprobe xen-pciback
# xl pci-assignable-add xx:xx.x
```

###### Kite’s build environment:
First, install some pre-requisite libraries.

```
# apt install build-essential git
# apt install libz-dev libxen-dev
```

Next, get Kite’s source and build it:

```
$ git clone https://github.com/ssrg-vt/kite
$ cd kite
$ git submodule update --init --recursive --remote
$ CC=`echo $PWD`/gcc8fix.sh ./build-rr.sh -j16 hw
$ cd bridge
$ ./ifconf.sh && ./run.sh
$ cd ../vbdconf
$ ./run.sh
```

## Running Kite driver domains
###### Network domain:
First, update Artifact/config/network/ubunt_dd.cfg with the BDF number of the network device:

```
pci=[‘xx:xx.x,permissive=1’]
```
Next, launch the Kite network domain using the following command.

```
# xl create -c Artifact/config/network/kite_dd.cfg
```

###### Storage domain:
First, update Artifact/config/storage/ubunt_dd.cfg with the BDF number of the storage device:

```
pci=[‘xx:xx.x,permissive=1’]
```
Next, launch the Kite storage domain using the following command.

```
# xl create -c Artifact/config/network/kite_dd.cfg
```

## Some importatnt directories

```
kite/
 \_ Artifact                    - Evaluation instruction and files
 \_ bridge/                     - Network domain application 
 \_ platform/
|   \_ hw/
|     \_ librumpxen_blkback/    - Storage driver backend
|     \_ librumpxen_netback/    - Network driver backend
\_ vbdconf/                     - Storage domain application
```
