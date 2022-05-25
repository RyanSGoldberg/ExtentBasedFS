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
 * CSC369 Assignment 1 - a1fs driver implementation.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

// Using 2.9.x FUSE API
#define FUSE_USE_VERSION 29
#include <fuse.h>

#include "a1fs.h"
#include "fs_ctx.h"
#include "options.h"
#include "map.h"
#include "fs_utils.h"

//NOTE: All path arguments are absolute paths within the a1fs file system and
// start with a '/' that corresponds to the a1fs root directory.
//
// For example, if a1fs is mounted at "~/my_csc369_repo/a1b/mnt/", the path to a
// file at "~/my_csc369_repo/a1b/mnt/dir/file" (as seen by the OS) will be
// passed to FUSE callbacks as "/dir/file".
//
// Paths to directories (except for the root directory - "/") do not end in a
// trailing '/'. For example, "~/my_csc369_repo/a1b/mnt/dir/" will be passed to
// FUSE callbacks as "/dir".


/**
 * Initialize the file system.
 *
 * Called when the file system is mounted. NOTE: we are not using the FUSE
 * init() callback since it doesn't support returning errors. This function must
 * be called explicitly before fuse_main().
 *
 * @param fs    file system context to initialize.
 * @param opts  command line options.
 * @return      true on success; false on failure.
 */
static bool a1fs_init(fs_ctx *fs, a1fs_opts *opts)
{
	// Nothing to initialize if only printing help
	if (opts->help) return true;

	size_t size;
	void *image = map_file(opts->img_path, A1FS_BLOCK_SIZE, &size);
	if (!image) return false;

	return fs_ctx_init(fs, image, size);
}

/**
 * Cleanup the file system.
 *
 * Called when the file system is unmounted. Must cleanup all the resources
 * created in a1fs_init().
 */
static void a1fs_destroy(void *ctx)
{
	fs_ctx *fs = (fs_ctx*)ctx;
	if (fs->image) {
		munmap(fs->image, fs->size);
		fs_ctx_destroy(fs);
	}
}

/** Get file system context. */
static fs_ctx *get_fs(void)
{
	return (fs_ctx*)fuse_get_context()->private_data;
}


/**
 * Get file system statistics.
 *
 * Implements the statvfs() system call. See "man 2 statvfs" for details.
 * The f_bfree and f_bavail fields should be set to the same value.
 * The f_ffree and f_favail fields should be set to the same value.
 * The following fields can be ignored: f_fsid, f_flag.
 * All remaining fields are required.
 *
 * Errors: none
 *
 * @param path  path to any file in the file system. Can be ignored.
 * @param st    pointer to the struct statvfs that receives the result.
 * @return      0 on success; -errno on error.
 */
static int a1fs_statfs(const char *path, struct statvfs *st)
{
	if(VERBOSE) printf("statf(%s)\n", path);
	(void)path;// unused
	fs_ctx *fs = get_fs();

	memset(st, 0, sizeof(*st));
	st->f_bsize   = A1FS_BLOCK_SIZE;
	st->f_frsize  = A1FS_BLOCK_SIZE;
	
	// The total number of blocks and the number of free blocks (all of which are data blocks)
	st->f_blocks  = fs->superblock->size / A1FS_BLOCK_SIZE;
	st->f_bfree   = fs->superblock->num_free_dblocks;
	st->f_bavail  = st->f_bfree;
	
	// The total number of inodes and the number of free inodes 
	st->f_files  = fs->superblock->num_inodes;
	st->f_ffree  = fs->superblock->num_free_inodes;
	st->f_favail = st->f_ffree;

	st->f_namemax = A1FS_NAME_MAX;
	return 0;
}

/**
 * Get file or directory attributes.
 *
 * Implements the lstat() system call. See "man 2 lstat" for details.
 * The following fields can be ignored: st_dev, st_ino, st_uid, st_gid, st_rdev,
 *                                      st_blksize, st_atim, st_ctim.
 * All remaining fields are required.
 *
 * NOTE: the st_blocks field is measured in 512-byte units (disk sectors);
 *       it should include any metadata blocks that are allocated to the 
 *       inode.
 *
 * NOTE2: the st_mode field must be set correctly for files and directories.
 *
 * Errors:
 *   ENAMETOOLONG  the path or one of its components is too long.
 *   ENOENT        a component of the path does not exist.
 *   ENOTDIR       a component of the path prefix is not a directory.
 *
 * @param path  path to a file or directory.
 * @param st    pointer to the struct stat that receives the result.
 * @return      0 on success; -errno on error;
 */
static int a1fs_getattr(const char *path, struct stat *st)
{
	if (strlen(path) >= A1FS_PATH_MAX) return -ENAMETOOLONG;
	fs_ctx *fs = get_fs();
	memset(st, 0, sizeof(*st));

	int i;
	if((i = path_lookup(path, fs))  < 0) return i;
	if(VERBOSE) printf("getaddr(%s) <inum=%d>\n", path, i);
	a1fs_ino_t i_num = i;
	a1fs_inode *inode = &fs->inode_table[i_num];
	st->st_mode = inode->mode;
	st->st_nlink = inode->links;
	st->st_size = inode->size;
	st->st_blocks = inode->size / 512; // Since it is inode->size/BLK_SIZE * BLK_SIZE/512
	st->st_mtim = inode->mtime;
	return 0;
}

/**
 * Read a directory.
 *
 * Implements the readdir() system call. Should call filler(buf, name, NULL, 0)
 * for each directory entry. See fuse.h in libfuse source code for details.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a directory.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a filler() call failed).
 *
 * @param path    path to the directory.
 * @param buf     buffer that receives the result.
 * @param filler  function that needs to be called for each directory entry.
 *                Pass 0 as offset (4th argument). 3rd argument can be NULL.
 * @param offset  unused.
 * @param fi      unused.
 * @return        0 on success; -errno on error.
 */
static int a1fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi)
{
	if(VERBOSE) printf("readdir(%s)\n", path);
	(void)offset;// unused
	(void)fi;// unused
	fs_ctx *fs = get_fs();

	// The current and parent directories
	filler(buf, "." , NULL, 0);
	filler(buf, "..", NULL, 0);
	
	a1fs_ino_t i_num = path_lookup(path, fs);
	a1fs_inode *inode = &fs->inode_table[i_num];

	a1fs_block_iterator b_iter;
	block_iterator_init(inode, &b_iter, fs);

	a1fs_dentry *cur_entry;
	void *cur_blk;
	// Iterate over the inode's data blocks
	while(NULL != (cur_blk = block_iterator_next_blk(&b_iter, fs))){
		// Iterate over the NUM_DENTRY_PER_BLOCK dir_entries in a block
		for(uint32_t d_ind = 0; d_ind < NUM_DENTRY_PER_BLOCK; d_ind++)
		{
			cur_entry = (a1fs_dentry *) (cur_blk + d_ind * sizeof(a1fs_dentry));
			// If the dir name is not the empty string, then that dir entry is allocated
			if('\0' != *cur_entry->name)
			{
				if(0 != filler(buf, cur_entry->name, NULL, 0)) return -ENOMEM;
			}
		}
	}
	return 0;
}


/**
 * Create a directory.
 *
 * Implements the mkdir() system call.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" doesn't exist.
 *   The parent directory of "path" exists and is a directory.
 *   "path" and its components are not too long.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a malloc() call failed).
 *   ENOSPC  not enough free space in the file system.
 *
 * @param path  path to the directory to create.
 * @param mode  file mode bits.
 * @return      0 on success; -errno on error.
 */
static int a1fs_mkdir(const char *path, mode_t mode)
{	
	if(VERBOSE) printf("mkdir(%s)\n", path);
	fs_ctx *fs = get_fs();
	return add_dir_entry(path, mode | S_IFDIR, 2, fs);
}

/**
 * Remove a directory.
 *
 * Implements the rmdir() system call.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a directory.
 *
 * Errors:
 *   ENOTEMPTY  the directory is not empty.
 *
 * @param path  path to the directory to remove.
 * @return      0 on success; -errno on error.
 */
static int a1fs_rmdir(const char *path)
{
	if(VERBOSE) printf("rmdir(%s)\n", path);
	fs_ctx *fs = get_fs();
	a1fs_inode *inode = &fs->inode_table[path_lookup(path, fs)];

	// Check if the directory is empty
	a1fs_block_iterator b_iter;
    block_iterator_init(inode, &b_iter, fs);

	a1fs_dentry *cur_entry;
    void *cur_blk;
    // Iterate over the inodes data blocks
    while(NULL != (cur_blk = block_iterator_next_blk(&b_iter, fs))){
        // Iterate over the NUM_DENTRY_PER_BLOCK dir_entries in a block
        for(uint32_t d_ind = 0; d_ind < NUM_DENTRY_PER_BLOCK; d_ind++)
        {
            cur_entry = (a1fs_dentry *) (cur_blk + d_ind * sizeof(a1fs_dentry));
            // Check for an empty a1fs_dentry
            if('\0' != *cur_entry->name)
            {
				// There is an entry with a non-empty name i.e it is NOT empty
                return -ENOTEMPTY;
            }
        }
    }
	// The directory is empty so it can be removed
	remove_dir_entry(path, fs);
	return 0;
}

/**
 * Create a file.
 *
 * Implements the open()/creat() system call.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" doesn't exist.
 *   The parent directory of "path" exists and is a directory.
 *   "path" and its components are not too long.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a malloc() call failed).
 *   ENOSPC  not enough free space in the file system.
 *
 * @param path  path to the file to create.
 * @param mode  file mode bits.
 * @param fi    unused.
 * @return      0 on success; -errno on error.
 */
static int a1fs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	if(VERBOSE) printf("creat(%s)\n", path);
	(void)fi;// unused
	assert(S_ISREG(mode));
	fs_ctx *fs = get_fs();
	return add_dir_entry(path, mode, 1, fs);
}

/**
 * Remove a file.
 *
 * Implements the unlink() system call.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a file.
 *
 * Errors: none
 *
 * @param path  path to the file to remove.
 * @return      0 on success; -errno on error.
 */
static int a1fs_unlink(const char *path)
{
	if(VERBOSE) printf("unlink(%s)\n", path);
	fs_ctx *fs = get_fs();
	// Remove the file from its parent's directory entires
	remove_dir_entry(path, fs);
	return 0;
}


/**
 * Change the modification time of a file or directory.
 *
 * Implements the utimensat() system call. See "man 2 utimensat" for details.
 *
 * NOTE: You only need to implement the setting of modification time (mtime).
 *       Timestamp modifications are not recursive. 
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists.
 *
 * Errors: 
 *   EFAULT	 inode->mtime points outside the accessible address space
 *
 * @param path   path to the file or directory.
 * @param times  timestamps array. See "man 2 utimensat" for details.
 * @return       0 on success; -errno on failure.
 */
static int a1fs_utimens(const char *path, const struct timespec times[2])
{
	if(VERBOSE) printf("utimens(%s)\n", path);
	fs_ctx *fs = get_fs();
	a1fs_inode *inode = &fs->inode_table[path_lookup(path, fs)]; 

	if(UTIME_NOW == times[1].tv_nsec || NULL == times)
	{ 
		// set mtime to the current time
		if(clock_gettime(CLOCK_REALTIME, &inode->mtime) < 0) return -EFAULT;
	}else if(UTIME_OMIT != times[1].tv_nsec)
	{
		// set mtime to times[1]
		inode->mtime = times[1];
	}
	return 0;
}

/**
 * Change the size of a file.
 *
 * Implements the truncate() system call. Supports both extending and shrinking.
 * If the file is extended, the new uninitialized range at the end must be
 * filled with zeros.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a file.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a malloc() call failed).
 *   ENOSPC  not enough free space in the file system.
 *   EFAULT	 inode->mtime points outside the accessible address space
 *
 * @param path  path to the file to set the size.
 * @param size  new file size in bytes.
 * @return      0 on success; -errno on error.
 */
static int a1fs_truncate(const char *path, off_t size)
{
	if(VERBOSE) printf("truncate(%s, %ld)\n", path, size);
	fs_ctx *fs = get_fs();

	a1fs_inode *inode = &fs->inode_table[path_lookup(path, fs)];
	// Update the modification time
	if(clock_gettime(CLOCK_REALTIME, &inode->mtime) < 0) return -EFAULT;

	if((uint64_t)size > inode->size)
	{ // The file is being extended
		off_t additional_bytes = size - inode->size;
		if(0 > allocate_data_blocks(inode, additional_bytes, fs)) return -ENOSPC;
		
		char *buf;
		if(NULL == (buf = malloc(additional_bytes))) return -ENOMEM;
		memset(buf, 0, additional_bytes);
		// Write additional_bytes of zeros to the inodes data blocks, starting at
		// offset=inode->size, i.e the end of the data blocks so far
		copy_between_buf_and_fs(inode, buf, additional_bytes, inode->size, true, fs);
		free(buf);
	}else if((uint64_t)size < inode->size)
	{ // The file is being shrunk        
		a1fs_extent *cur_extent;
		int num_extents = inode->num_extents;
		int blk_idx = 0;
        // Iterate over the inodes data blocks
        for(uint32_t i = 0; i < inode->num_extents; i++)
        {
            cur_extent = get_extent(inode, i, fs);
			int count = cur_extent->count;
			// Itterate over the blocks
            for(a1fs_blk_t b = cur_extent->start; b < cur_extent->start+count; b++, blk_idx++){
				// Dealocate the data block if the blk index * block size is greater than the needed size
                if(blk_idx * A1FS_BLOCK_SIZE > size)
				{
					fs->d_bitmap[b/8] = fs->d_bitmap[b/8] & ~(1 << (b % 8));
                	fs->superblock->num_free_dblocks++;
					cur_extent->count--;
					if(0 == cur_extent->count)
					{
						if(A1FS_NUM_DIRECT_EXTENT == num_extents)
						{ // Deallocate the indirect block
							a1fs_blk_t indir = inode->indirect_extent_blk;
							fs->d_bitmap[indir/8] = fs->d_bitmap[indir/8] & ~(1 << (indir % 8));
                			fs->superblock->num_free_dblocks++;
						}
						num_extents--;
					}
				}
            }
        }
		inode->num_extents = num_extents;

		if(VERBOSE) print_data_block_bitmap("Dealocation Complete", fs);
	}
	// Note: if the size is equal than do nothing

	// Set the inode size to the new size
	inode->size = size;
	return 0;
}


/**
 * Read data from a file.
 *
 * Implements the pread() system call. Must return exactly the number of bytes
 * requested except on EOF (end of file). Reads from file ranges that have not
 * been written to must return ranges filled with zeros. You can assume that the
 * byte range from offset to offset + size is contained within a single block.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a file.
 *
 * Errors: none
 *
 * @param path    path to the file to read from.
 * @param buf     pointer to the buffer that receives the data.
 * @param size    buffer size (number of bytes requested).
 * @param offset  offset from the beginning of the file to read from.
 * @param fi      unused.
 * @return        number of bytes read on success; 0 if offset is beyond EOF;
 *                -errno on error.
 */
static int a1fs_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi)
{
	if(VERBOSE) printf("read(%s, %p, %ld, %ld)\n", path, (void *)buf, size, offset);	
	(void)fi;// unused
	fs_ctx *fs = get_fs();
	memset(buf, 0, size); // Zero the buffer before use
	a1fs_inode *inode = &fs->inode_table[path_lookup(path, fs)];
	// Copy from the fs to buf
	return copy_between_buf_and_fs(inode, buf, size, offset, false, fs);
}

/**
 * Write data to a file.
 *
 * Implements the pwrite() system call. Must return exactly the number of bytes
 * requested except on error. If the offset is beyond EOF (end of file), the
 * file must be extended. If the write creates a "hole" of uninitialized data,
 * the new uninitialized range must filled with zeros. You can assume that the
 * byte range from offset to offset + size is contained within a single block.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a file.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a malloc() call failed).
 *   ENOSPC  not enough free space in the file system.
 *   ENOSPC  too many extents (a1fs only needs to support 512 extents per file)
 *   EFAULT	 inode->mtime points outside the accessible address space
 * 
 * @param path    path to the file to write to.
 * @param buf     pointer to the buffer containing the data.
 * @param size    buffer size (number of bytes requested).
 * @param offset  offset from the beginning of the file to write to.
 * @param fi      unused.
 * @return        number of bytes written on success; -errno on error.
 */
static int a1fs_write(const char *path, const char *buf, size_t size,
                      off_t offset, struct fuse_file_info *fi)
{
	if(VERBOSE) printf("write(%s, %p, %ld, %ld)\n", path, (void *)buf, size, offset);	
	(void)fi;// unused
	fs_ctx *fs = get_fs();
	
	a1fs_inode *inode = &fs->inode_table[path_lookup(path, fs)];
	// Update the modification time
	if(clock_gettime(CLOCK_REALTIME, &inode->mtime) < 0) return -EFAULT;

	if((uint64_t)offset > inode->size)
	{ // We need to fill in the 'hole' by zeroing out this memory
		off_t additional_bytes = offset - inode->size;
		if(0 > allocate_data_blocks(inode, additional_bytes, fs)) return -ENOSPC;
		
		char *zero_buf;
		if(NULL == (zero_buf = malloc(additional_bytes))) return -ENOMEM;
		memset(zero_buf, 0, additional_bytes);
		copy_between_buf_and_fs(inode, zero_buf, additional_bytes, inode->size, true, fs);
		free(zero_buf);
		inode->size += additional_bytes;
	}

	if(0 > allocate_data_blocks(inode, size, fs)) return -ENOSPC;
	inode->size += size;
	// Copy from buf to the fs
	return copy_between_buf_and_fs(inode, (char *)buf, size, offset, true, fs);
}


static struct fuse_operations a1fs_ops = {
	.destroy  = a1fs_destroy,
	.statfs   = a1fs_statfs,
	.getattr  = a1fs_getattr,
	.readdir  = a1fs_readdir,
	.mkdir    = a1fs_mkdir,
	.rmdir    = a1fs_rmdir,
	.create   = a1fs_create,
	.unlink   = a1fs_unlink,
	.utimens  = a1fs_utimens,
	.truncate = a1fs_truncate,
	.read     = a1fs_read,
	.write    = a1fs_write,
};

int main(int argc, char *argv[])
{
	a1fs_opts opts = {0};// defaults are all 0
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	if (!a1fs_opt_parse(&args, &opts)) return 1;

	fs_ctx fs = {0};
	if (!a1fs_init(&fs, &opts)) {
		fprintf(stderr, "Failed to mount the file system\n");
		return 1;
	}

	return fuse_main(args.argc, args.argv, &a1fs_ops, &fs);
}
