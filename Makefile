# This code is provided solely for the personal and private use of students
# taking the CSC369H course at the University of Toronto. Copying for purposes
# other than this use is expressly prohibited. All forms of distribution of
# this code, including but not limited to public repositories on GitHub,
# GitLab, Bitbucket, or any other online platform, whether as given or with
# any changes, are expressly prohibited.
#
# Authors: Alexey Khrabrov, Karen Reid
#
# All of the files in this directory and all subdirectories are:
# Copyright (c) 2019 Karen Reid

CC = gcc
CFLAGS  := $(shell pkg-config fuse --cflags) -g3 -Wall -Wextra -Werror $(CFLAGS)
LDFLAGS := $(shell pkg-config fuse --libs) $(LDFLAGS)
MOUNT_POINT := ~/Documents/UofT/CSC369/FuseFS/ # REMOVE ME

.PHONY: all clean

all: a1fs mkfs.a1fs

a1fs: fs_ctx.o a1fs.o map.o options.o fs_utils.o
	$(CC) $^ -o $@ $(LDFLAGS)

mkfs.a1fs: fs_ctx.o map.o fs_utils.o mkfs.o 
	$(CC) $^ -o $@ $(LDFLAGS)

SRC_FILES = $(wildcard *.c)
OBJ_FILES = $(SRC_FILES:.c=.o)

-include $(OBJ_FILES:.o=.d)

%.o: %.c
	$(CC) $< -o $@ -c -MMD $(CFLAGS)

clean:
	rm -f $(OBJ_FILES) $(OBJ_FILES:.o=.d) a1fs mkfs.a1fs

# TEMP: Remove me later (both below)

# Mount the sample fs
mount: a1fs mkfs.a1fs
	./mkfs.a1fs -z -i 256 Images/256KB_256I_image && ./a1fs -f Images/256KB_256I_image $(MOUNT_POINT)

# cd to the mount point
go:
	(cd $(MOUNT_POINT) && bash)

# Unmount the fs
unmount:
	fusermount -u $(MOUNT_POINT)

# make && ./mkfs.a1fs -z -i 256 Images/256KB_256I_image && gdb --args ./a1fs Images/256KB_256I_image ../../FuseFS -f
