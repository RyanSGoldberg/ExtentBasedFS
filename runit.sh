#!/bin/bash
make

mkdir mnt_pnt

echo "Making an image file of size 256KB"
truncate -s 256K image_file

echo "Formatting the image file with 256 inodes"
./mkfs.a1fs -i 256 image_file

echo "Mounting the file system with mount point mnt_pnt"
./a1fs image_file mnt_pnt

echo "cd to mnt_pnt and performing a few operations"
cd mnt_pnt
mkdir dir
touch dir/file
echo "Hello" > dir/file2
cat dir/file2
ls -lR
cd ..


echo "Unmount and the remount the image"
fusermount -u /home/osboxes/Documents/UofT/CSC369/goldb159/a1b/mnt_pnt
./a1fs image_file mnt_pnt
cd mnt_pnt
ls -lR
cat dir/file2
cd ..

echo "Unmounting and cleaning up the generated files"
fusermount -u mnt_pnt
rmdir mnt_pnt
rm image_file
