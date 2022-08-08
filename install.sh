path=~/res.txt

echo "" > $path

echo "start:>>>>>>" >> $path
date >> $path


make -j4 2>>$path
echo "make done >>>>>>>>>" >> $path
date >> $path


make modules_install 2>>$path
echo "modules_install done >>>>>>>>>" >> $path
date >> $path

rm -rf /boot/config-5.16.0-rc1-damon*
rm -rf /boot/initrd.img-5.16.0-rc1-damon*
rm -rf /boot/System.map-5.16.0-rc1-damon*
rm -rf /boot/vmlinuz-5.16.0-rc1-damon*
echo "grub del done >>>>>>>>>" >> $path



make install 2>>$path
echo "install done >>>>>>>>>" >> $path
date >> $path



