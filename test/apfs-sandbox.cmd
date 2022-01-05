# ./u-boot -c "$(< ./test/apfs-sanbox.cmd)"
host bind 0 /media/psf/Home/Desktop/apfs-test-vm.macosvm/disk.img
gpt read host 0
part list host 0