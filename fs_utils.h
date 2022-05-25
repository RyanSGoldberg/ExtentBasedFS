/**
 * CSC369 Assignment 1 - File system utilities header file.
 *  These functions are called by the functions in a1fs, to implement the file system functionality
 *  with minimal duplicated code. 
 */

#pragma once

/**
 * Initialize an inode. 
 * 
 * Assume: The index has already been checked and is valid for an inode
 * 
 * @param index  the index of the inode to be initialized
 * @param mode   the mode of the inode
 * @param links  the number of links to the inodes
 * @param image  pointer to the start of the image
 * @return       true on success; false on failure (e.g. invalid superblock).
 */

bool init_inode(a1fs_ino_t index, mode_t mode, uint32_t links, void* image);

/**
 * Find the first unused inode in the inode index
 * 
 * @param  superblock  a pointer to the super block
 * @param  fs          a pointer to the context
 * @return             the inode number of the first unused inode; -1 on failure
*/
int find_empty_inode(fs_ctx *fs);

/** 
 * Lookup the inode number assosiated with a path
 * Errors:
 *   ENOENT    a component of the path does not exist.
 *   ENOTDIR   a component of the path prefix is not a directory
 * 
 * @param  path        path to a file or directory.
 * @param  fs          a pointer to the context
 * @return             inode number on sucsess; -errno on error;
*/
int path_lookup(const char *path, fs_ctx *fs);

/**
 * Get a pointer to the extent at index in the inode
 * 
 * @param  inode      a pointer to the inode
 * @param  index      the index of the extent
 * @param  fs         a pointer to the context
 * @return            a pointer to the indexth extent of the inode
*/
a1fs_extent *get_extent(a1fs_inode *inode, int index, fs_ctx *fs);

/**
 * Allocate the data blocks needed to write size bytes to the d-blocks 
 * for the inode.
 * 
 * Errors:
 *   ENOSPC  not enough free space in the file system.
 * 
 * @param inode      the inode which will have additional data written
 * @param size       the number of additional bytes needed
 * @param fs         a pointer to the context
 * @return           0 on sucsess, -errno of failure
*/
int allocate_data_blocks(a1fs_inode *inode, uint64_t size, fs_ctx *fs);

/**
 * An entry for the new file (Reg file or directory) to be created
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
 * @param  path     path to the file to create.
 * @param  mode     file mode bits.
 * @param  links    the initial number of links to the inode
 * @param  fs       a pointer to the context
 * @return          0 on success; -errno on error.
*/
int add_dir_entry(const char *path, mode_t mode, uint32_t links, fs_ctx *fs);

/** 
 * Remove a directory entry and free up the resources
 * 
 * @param   unmodified_path  path to the file to create.
 * @param   fs               a pointer to the context
*/
void remove_dir_entry(const char *unmodified_path, fs_ctx *fs);

/**
 * A struct used to keep track of the state of the traversal of the data blocks pointed to by an inode
*/
typedef struct a1fs_block_iterator{
    a1fs_inode  *inode; 
    a1fs_extent *cur_extent;          // A pointer to the current extent being travered
    uint32_t     extent_index;        // The index of the extent within the inode
    uint32_t     blk_in_extent_index; // The index of the block within the extent
}a1fs_block_iterator;

/**
 * Initialize a a1fs_block_iterator for the inode to traverse
 * 
 * @param inode   a pointer to the inode
 * @param b_iter  a pointer to the a1fs_block_iterator keeping track of the state of the traversal
 * @param fs      a pointer to the context
*/
void block_iterator_init(a1fs_inode *inode, a1fs_block_iterator *b_iter, fs_ctx *fs);

/**
 * Return a pointer the the start of the next data block to be traversed
 * @param   b_iter  a pointer to the a1fs_block_iterator keeping track of the state of the traversal
 * @param   fs      a pointer to the context
 * @return  a pointer to the next data block, NULL if there are no more blocks to traverse
*/
void *block_iterator_next_blk(a1fs_block_iterator *b_iter, fs_ctx *fs);

/**
 * Copy between a buffer and data blocks on the disk
 * 
 * @param inode   a pointer to the inode whos data blocks are being accessed
 * @param buf     a buffer in the user space, which is either being read from or written to
 * @param size    buffer size (number of bytes requested).
 * @param offset  offset from the beginning of the file to write to.
 * @param to_fs   true if we are writing to the file system from buf, false if reading from the file system into buf
 * @param fs      a pointer to the context
 * @return        number of bytes written
*/
int copy_between_buf_and_fs(a1fs_inode *inode, char *buf, size_t size, off_t offset, bool to_fs, fs_ctx *fs);

/**
 * Print out the data block bitmap
 * @param msg  a message to print
 * @param fs   a pointer to the context 
*/
void print_data_block_bitmap(const char *msg, fs_ctx *fs);