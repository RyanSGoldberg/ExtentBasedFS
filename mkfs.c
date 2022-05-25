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
 * CSC369 Assignment 1 - a1fs formatting tool.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "fs_ctx.h"
#include "a1fs.h"
#include "map.h"
#include "fs_utils.h"
#include "util.h"

/** Command line options. */
typedef struct mkfs_opts {
	/** File system image file path. */
	const char *img_path;
	/** Number of inodes. */
	size_t n_inodes;

	/** Print help and exit. */
	bool help;
	/** Overwrite existing file system. */
	bool force;
	/** Zero out image contents. */
	bool zero;

} mkfs_opts;

static const char *help_str = "\
Usage: %s options image\n\
\n\
Format the image file into a1fs file system. The file must exist and\n\
its size must be a multiple of a1fs block size - %zu bytes.\n\
\n\
Options:\n\
    -i num  number of inodes; required argument\n\
    -h      print help and exit\n\
    -f      force format - overwrite existing a1fs file system\n\
    -z      zero out image contents\n\
";

static void print_help(FILE *f, const char *progname)
{
	fprintf(f, help_str, progname, A1FS_BLOCK_SIZE);
}


static bool parse_args(int argc, char *argv[], mkfs_opts *opts)
{
	char o;
	while ((o = getopt(argc, argv, "i:hfvz")) != -1) {
		switch (o) {
			case 'i': opts->n_inodes = strtoul(optarg, NULL, 10); break;

			case 'h': opts->help  = true; return true;// skip other arguments
			case 'f': opts->force = true; break;
			case 'z': opts->zero  = true; break;

			case '?': return false;
			default : assert(false);
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "Missing image path\n");
		return false;
	}
	opts->img_path = argv[optind];

	if (opts->n_inodes == 0) {
		fprintf(stderr, "Missing or invalid number of inodes\n");
		return false;
	}
	return true;
}


/** Determine if the image has already been formatted into a1fs. */
static bool a1fs_is_present(void *image)
{
	//Checks if the image already contains a valid a1fs superblock
	a1fs_superblock * superblock   = (a1fs_superblock *)(image + A1FS_BLOCK_SIZE);

	// Check the the correct magic number is used
	if (A1FS_MAGIC != superblock->magic) return false;
	
	// Checks that the pointers to the different blocks are correct
	uint32_t num_total_blocks = superblock->size / A1FS_BLOCK_SIZE;
	uint32_t num_inode_blocks = Ceil((superblock->num_inodes * sizeof(a1fs_inode)), (A1FS_BLOCK_SIZE));
	uint32_t num_data_blocks = num_total_blocks - num_inode_blocks - 2;
	uint32_t num_data_bitmap_blocks = Ceil(num_data_blocks, 8*A1FS_BLOCK_SIZE);

	if(2 != superblock->data_bitmap) return false;
	if(2+num_data_bitmap_blocks != superblock->inode_table) return false;
	if(2+num_data_bitmap_blocks+num_inode_blocks != superblock->data_blk) return false;
	if((2+num_data_bitmap_blocks+num_inode_blocks != superblock->data_blk+num_data_blocks)
		*A1FS_BLOCK_SIZE != superblock->size) return false;
	return true;
}


/**
 * Format the image into a1fs.
 *
 * NOTE: Must update mtime of the root directory.
 *
 * @param image  pointer to the start of the image.
 * @param size   image size in bytes.
 * @param opts   command line options.
 * @return       true on success;
 *               false on error, e.g. options are invalid for given image size.
 */
static bool mkfs(void *image, size_t size, mkfs_opts *opts)
{
	uint32_t num_total_blocks = size / A1FS_BLOCK_SIZE;

	// The number of inode blocks, return false if they are leq 0
	uint32_t num_inode_blocks = Ceil((opts->n_inodes * sizeof(a1fs_inode)), (A1FS_BLOCK_SIZE));
	if (num_inode_blocks <= 0) return false;
	
	// The number of blocks needed to hold the bitmap for the data blocks
	uint32_t num_data_blocks        = num_total_blocks - num_inode_blocks - 2;
	uint32_t num_data_bitmap_blocks = Ceil(num_data_blocks, 8*A1FS_BLOCK_SIZE); // Since there are 8 bits to the byte

	// Make sure the disk is big enough of this many inodes and the metadata blocks before it (and the reserved block 0)
	if (num_total_blocks < num_inode_blocks+num_data_bitmap_blocks + 2) return false;
	

	// Initialize block 0 as the super block
	a1fs_superblock *superblock   = (a1fs_superblock *)(image + A1FS_BLOCK_SIZE);
	superblock->magic             = A1FS_MAGIC;
	superblock->size              = size;
	superblock->num_inodes        = opts->n_inodes;
	superblock->num_free_inodes   = opts->n_inodes;
	superblock->num_tot_dblocks   = num_data_blocks - num_data_bitmap_blocks;
	superblock->num_free_dblocks  = num_data_blocks - num_data_bitmap_blocks;
	superblock->data_bitmap       = 2;
	superblock->inode_table       = 2+num_data_bitmap_blocks;
	superblock->data_blk          = 2+num_data_bitmap_blocks+num_inode_blocks;
	
	// Initialize the inode table to be empty
	for(uint32_t blk = 0; blk < num_inode_blocks; blk++){
		for(uint32_t i = 0; i < NUM_INODES_PER_BLOCK; i++){
			a1fs_inode *inode = (a1fs_inode *)(image+(superblock->inode_table+blk) * A1FS_BLOCK_SIZE
				+ i * sizeof(a1fs_inode));
			// IMPORTANT: Since all allocated inodes must have at least 1 link (parent or
			//  in the case of root, 1 it itself) having 0 links means an inode is not allocated
			inode->links = 0;
		}
	}

	// Initialize the data bitmap
	memset(image+(superblock->data_bitmap * A1FS_BLOCK_SIZE), 0, num_data_bitmap_blocks * A1FS_BLOCK_SIZE);
    
    // Initialize the root directory with index 0 and 2 links. The size and number of extents start at 0.
	// The superblock is updated accordingly
    // NOTE: the mode of the root directory inode should be set to S_IFDIR | 0777
	return init_inode(0, S_IFDIR | 0777, 2, image);
}


int main(int argc, char *argv[])
{
	mkfs_opts opts = {0};// defaults are all 0
	if (!parse_args(argc, argv, &opts)) {
		// Invalid arguments, print help to stderr
		print_help(stderr, argv[0]);
		return 1;
	}
	if (opts.help) {
		// Help requested, print it to stdout
		print_help(stdout, argv[0]);
		return 0;
	}

	// Map image file into memory
	size_t size;
	void *image = map_file(opts.img_path, A1FS_BLOCK_SIZE, &size);
	if (image == NULL) return 1;

	// Check if overwriting existing file system
	int ret = 1;
	if (!opts.force && a1fs_is_present(image)) {
		fprintf(stderr, "Image already contains a1fs; use -f to overwrite\n");
		goto end;
	}

	if (opts.zero) memset(image, 0, size);
	if (!mkfs(image, size, &opts)) {
		fprintf(stderr, "Failed to format the image\n");
		goto end;
	}

	ret = 0;
end:
	munmap(image, size);
	return ret;
}
