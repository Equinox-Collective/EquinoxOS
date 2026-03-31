@echo off
set PATH=C:\Program Files\qemu;%PATH%
cd /d %~dp0
qemu-system-x86_64.exe -m 128M -drive file=hdd.img,format=raw,index=0,media=disk -cdrom equos.iso -serial stdio -netdev user,id=n0,hostfwd=tcp::2222-:22 -device rtl8139,netdev=n0 -object filter-dump,id=f1,netdev=n0,file=packets.pcap
