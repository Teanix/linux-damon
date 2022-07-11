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

make install 2>>$path
echo "install done >>>>>>>>>" >> $path
date >> $path
