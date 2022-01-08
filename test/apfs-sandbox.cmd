# ./u-boot -c "$(< ./test/apfs-sandbox.cmd)"
host bind 0 /media/psf/Home/Desktop/apfs-test-vm.macosvm/disk.img
gpt read host 0
part list host 0
apfs
