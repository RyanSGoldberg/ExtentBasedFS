#!/bin/bash
MOUNT_POINT=~/Documents/UofT/CSC369/FuseFS/

echo "Starting Tests ..." 
make > /dev/null
if test 0 -lt $?
then 
    echo "Build Failed"; exit
fi
echo "Testing on a 256KB disk, with 256 inodes"
./mkfs.a1fs -z -i 256 Images/256KB_256I_image && ./a1fs Images/256KB_256I_image $MOUNT_POINT

# Creating directories
echo "Testing Directory Creation && Deletion"
(cd $MOUNT_POINT && echo "Testing Directory Creation && Deletion" &&
echo "1.0 - Make 1 dir"
mkdir dir0 && ls -a; # A single dir can be made
echo "1.1 - Make 16 dirs (1 block)"
for i in {1..15}; do mkdir "dir$i"; done && ls -a; # The rest of the block (16 dirs) can be made
echo "1.2 - Make a sub dir"
mkdir dir0/sub_dir0 && ls -a dir0 && ls -a; # A sub_directory can be made
# test it will allocate a block which will force the need for a new extent
echo "1.3 - Make a dir that will need 1 more (non-consecutive block)"
mkdir dir16 && ls -a;
echo "1.4 - Remove the sub dir"
rmdir dir0/sub_dir0 && ls -a dir0;
echo "1.5 - Remove all the dirs"
rmdir *;
ls -aR
echo "1.6 - Make sure the number of allocated inodes and data blocks is correct"
stat -f . | head -3
stat -f . | tail -2
echo "1.7 - Use an indirect block"
for ext_num in {1..10}
do
    for i in {1..15}; do mkdir "dir$ext_num-$i"; done && truncate -s 1 "file$ext_num"
done
mkdir indirect
ls
) > Tests/test-directories
diff --color=always -y --suppress-common-lines Tests/test-directories Tests/correct-directories

# Unmount and remake an empty file system
fusermount -u $MOUNT_POINT
./mkfs.a1fs -z -i 256 Images/256KB_256I_image && ./a1fs Images/256KB_256I_image $MOUNT_POINT

# Creating & removing files and truncating
echo "Testing File Truncation/Creation/Deletion"
(cd $MOUNT_POINT && echo "Testing File Truncation" &&
echo "2.0 - Making a file"
touch file
ls -a
stat -c %s file # Make sure the size is zero
stat -f -c %f . # Make sure 1 inode was allocated, and 1 data block is allocated (for the first entry in root, not the file)
stat -f -c %d .
echo "2.1 - Extending a file"
truncate -s 32 file
stat -c %s file
xxd file
echo "2.2 - Shrinking a file"
truncate -s 16 file
stat -c %s file
xxd file
echo "2.3 - Shrinking a file, using a negative"
truncate -s -8 file
stat -c %s file
xxd file
echo "2.4 - Removing a file"
rm file
ls -a
stat -f -c %f .
stat -f -c %d .
) > Tests/test-truncation
diff --color=always -y --suppress-common-lines Tests/test-truncation Tests/correct-truncation

# Unmount and remake an empty file system
fusermount -u $MOUNT_POINT
./mkfs.a1fs -z -i 256 Images/256KB_256I_image && ./a1fs Images/256KB_256I_image $MOUNT_POINT

# Reading and writing files
echo "Testing Reading and Writing"
(cd $MOUNT_POINT && echo "Testing Reading and Writing" &&
echo "3.0 - Writing to an empty file and then reading its content"
touch file
echo "Hello" > file
xxd file
echo "3.1 - Appending to a file"
echo "World" >> file
xxd file
echo "3.2 - Reading the last line of a file (Reading from an offset)"
tail -1 file
echo "3.4 - Writing to the start of a file"
echo "HelloWorld" > file2
echo -n 'XYZ' > temp; dd conv=notrunc if=temp of=file2 status=none; rm temp
xxd file2
echo "3.5 - Writing to the middle of a file"
echo "HelloWorld" > file2
echo -n 'ABC' > temp; dd oflag=seek_bytes seek=2  conv=notrunc if=temp of=file2 status=none; rm temp
xxd file2
echo "3.6 - Writing and leaving a hole in the middle"
echo "HelloWorld" > file2
echo -n 'AfterHole' > temp; dd oflag=seek_bytes seek=15  conv=notrunc if=temp of=file2 status=none; rm temp
xxd file2
) > Tests/test-readwrite
 diff --color=always -y --suppress-common-lines Tests/test-readwrite Tests/correct-readwrite

# Unmount and exit
fusermount -u $MOUNT_POINT
