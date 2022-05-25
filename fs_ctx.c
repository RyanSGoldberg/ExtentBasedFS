/*
 * This code is provided solely for the personal and private use of students
 * taking the CSC369H course at the University of Toronto. Copying for purposes
 * other than this use is expressly prohibited. All forms of distribution of
 * this code, including but not limited to public repositories on GitHub,
 * GitLab, Bitbucket, or any other online platform, whether as given or with
 * any changes, are expressly prohibited.
 *
 * Authors: Alexey Khrabrov, Karen Reid
 *
 * All of the files in this directory and all subdirectories are:
 * Copyright (c) 2019 Karen Reid
 */

/**
 * CSC369 Assignment 1 - File system runtime context implementation.
 */

#include "fs_ctx.h"
#include "a1fs.h"

bool fs_ctx_init(fs_ctx *fs, void *image, size_t size)
{
	if (!image) return false;
	fs->image = image;
	fs->size = size;
	fs->superblock   = (a1fs_superblock *)(image + A1FS_BLOCK_SIZE);
	fs->d_bitmap = (char *)(image + fs->superblock->data_bitmap * A1FS_BLOCK_SIZE);
	fs->inode_table = (a1fs_inode *)(image + fs->superblock->inode_table * A1FS_BLOCK_SIZE);
	fs->data_blks = (image + fs->superblock->data_blk * A1FS_BLOCK_SIZE);
	return true;
}

void fs_ctx_destroy(fs_ctx *fs)
{
	(void)fs;
}
