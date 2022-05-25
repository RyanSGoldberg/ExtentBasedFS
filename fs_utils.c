#include <stdbool.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#include "a1fs.h"
#include "fs_ctx.h"
#include "util.h"
#include "fs_utils.h"

typedef struct a1fs_tuple{
    int start;
    int end;
} a1fs_tuple;

int find_empty_inode(fs_ctx *fs)
{
    // Iterate through the inodes table looking for an inode with 0 links (free).
    // It must therefore not be in use. 
    for(uint32_t i = 0; i < fs->superblock->num_inodes; i++)
    {
        if(0 == fs->inode_table[i].links) return i;
    }
    return -1;
}

bool init_inode(a1fs_ino_t index, mode_t mode, uint32_t links, void* image)
{
    if(!image) return false;
    a1fs_superblock *superblock = (a1fs_superblock *)(image + A1FS_BLOCK_SIZE);

    a1fs_inode *inode = (a1fs_inode *)(image + superblock->inode_table * A1FS_BLOCK_SIZE +
                                        index * sizeof(a1fs_inode));
    // Set the inode's fields, note that the mtime is set to the current time
    inode->mode = mode;
    inode->links = links;
    inode->size = 0;
    if(clock_gettime(CLOCK_REALTIME, &inode->mtime) < 0) return false;
    inode->num_extents = 0;
    memset(inode->direct_extents, 0, A1FS_NUM_DIRECT_EXTENT*sizeof(a1fs_extent));
    inode->indirect_extent_blk = 0;
    superblock->num_free_inodes--;

    return true;
}

int path_lookup(const char *unmodified_path, fs_ctx *fs) 
{
    if(VERBOSE) printf("\t path_lookup(%s). Inodes accessed: 0 ", unmodified_path);
    char buf[A1FS_PATH_MAX];
    strncpy(buf, unmodified_path, A1FS_PATH_MAX);
    char *path = buf;

    if(path[0] != '/') 
    {
        if(VERBOSE) printf("\n");
        return -ENOENT;
    } // The path must be absolute
    
    int cur_inode_num = 0; // Start at the root
    // Iterate over the components (/ seperated values) of the path
    for(char *component; NULL != (component = strtok(path, "/")); path = NULL)
    {
        if(cur_inode_num < 0) return -ENOENT; // A component was not found in the last iteration
        
        // A pointer to the inode currently being checked
        a1fs_inode *inode = &(fs->inode_table[cur_inode_num]);
        if (!S_ISDIR(inode->mode)) return -ENOTDIR;
        cur_inode_num = -1;

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
                // Check if the name of the cur entry is equal to the component, we're searching for
                if(0 == strcmp(cur_entry->name, component))
                {                        
                    cur_inode_num = cur_entry->ino;
                }
            }
        }
        if(VERBOSE) printf("%d ", cur_inode_num);
    }
    if(VERBOSE) printf("\n");
    return cur_inode_num >= 0 ? cur_inode_num : -ENOENT;
}

a1fs_extent *get_extent(a1fs_inode *inode, int index, fs_ctx *fs)
{
    // If the index is less than A1FS_NUM_DIRECT_EXTENT, get the extent from the inode, 
    // otherwise get it from the indirect block
    if(index < A1FS_NUM_DIRECT_EXTENT)
    {
        return &(inode->direct_extents[index]);
    }else
    {
        return (fs->data_blks + inode->indirect_extent_blk * A1FS_BLOCK_SIZE +
					(index - A1FS_NUM_DIRECT_EXTENT) * sizeof(a1fs_extent));
    }
}

/**
 * Find the first sequence of blocks which can hold the needed number of blocks, and if there are none long enough
 *  return the longest sequence that exists.
 * 
 * Assume:
 *   superblock->num_tot_dblocks has been checked, and there is at least 1 free data block
 * 
 * @param  needed     the number of blocks needed to find
 * @param  tuple      a pointer to a tuple in which to put the start and end of the sequence.
 *                     set start and end to -1 if there is no sequence
 * @param  fs         a pointer to the context
*/
void first_free_sequence(int needed, a1fs_tuple *tuple, fs_ctx *fs)
{
    int max_s, max_e, start, end;
    max_s = max_e = start = end = 0;

    // Iterate over the entire bitmap
    for(uint32_t i = 0; i < fs->superblock->num_tot_dblocks; i++)
    {
        // If the sequence of allocated blocks breaks i.e a new sequence of free blocks can begin
        if (0 != (fs->d_bitmap[i / 8] & (1 << (i % 8))))
        {
            // Once a sequence long enough is found, return that sequence
            // Note: we don't have end-start+1 since the last end++ should not be counted to this sequence so end-start+1-1
            if(end-start == needed)
            {
                tuple->start = start;
                tuple->end  = end;
                return;
            }

            // If the last found sequnce is longer than the max, update the max
            if(end-start > max_e-max_s)
            {
                max_e = end-1;
                max_s = start;
            }
            start = end+1;
            end = start;
        }
        else
        { 
            end +=1;
        }
    }
    // If the last sequence, which reached the end without breaking, is longer update max
    if(end-start > max_e-max_s)
    {
        max_e = end-1;
        max_s = start;
    }
    tuple->start = max_s;
    tuple->end  = max_e;
    if(max_e-max_s >= needed)
    {
        tuple->start = start;
        tuple->end  = start+needed-1;
        return;
    }

}

/**
 * Compute the number of consecutive free blocks starting at start
 * 
 * @param  start      the index (relative to the group) to start checking for free blocks
 * @param  fs         a pointer to the context
 * @
*/
int tail_length(uint32_t start, fs_ctx *fs)
{
    int len =  0;
    for(uint32_t i = start; i < fs->superblock->num_tot_dblocks; i++)
    {
        if (0 != (fs->d_bitmap[i / 8] & (1 << (i % 8)))) return len;
        len++;   
    }
    return len;
}

/**
 * Update the last extent of an inode 
 * @param  inode     a pointer to the ionode
 * @param  start     the start of the extent. Should be the same as the original last extent's start
 * @param  count     the total number of blocks in the extent. Both old and new
 * @param  fs        a pointer to the context
*/
void update_last_extent(a1fs_inode *inode, int start, int count, fs_ctx *fs)
{
    a1fs_extent *extent;

    // Mark the bitmap (This is done first so we can call allocate if a new inidrect block is needed)
    for(int b = start; b < start+count; b++){
        int byte = b/8;
        int bit = b % 8;
        fs->d_bitmap[byte] = fs->d_bitmap[byte] | (1 << bit);
    }

    // We need to allocate a new indirect block
    if(inode->num_extents == A1FS_NUM_DIRECT_EXTENT+1)
    {
        a1fs_tuple indirect_block;
        // Find a free block and mark the bitmap
        first_free_sequence(1, &indirect_block, fs);
        fs->d_bitmap[indirect_block.start / 8] = fs->d_bitmap[indirect_block.start / 8] | (1 << (indirect_block.start % 8));

        if(VERBOSE) print_data_block_bitmap("Indirect Block Alocation Complete", fs);
        fs->superblock->num_free_dblocks -= 1;
        inode->indirect_extent_blk = indirect_block.start;
    }
    extent = get_extent(inode, inode->num_extents-1, fs);

    // Update the inode and the extent's content
    extent->start = start;
    // Only decrement the new count from the superblock free dblocks count
    int count_additional_blocks = count;
    if (0 != extent->count) count_additional_blocks = count - extent->count;
    extent->count = count;

    fs->superblock->num_free_dblocks -= count_additional_blocks;
}

int allocate_data_blocks(a1fs_inode *inode, uint64_t size, fs_ctx *fs)
{
    // We need to fill the last block before we start allocating new ones, to prevent holes
    // So if the last block is not full, we'll use that before allocating new memory
    if(0 != inode->size % A1FS_BLOCK_SIZE)
    {
        size -= (inode->size % A1FS_BLOCK_SIZE);
    }
    // The number of blocks needed to allocate
    uint32_t blks_needed = Ceil(size, A1FS_BLOCK_SIZE);
    
    if(0 == blks_needed) return 0;

    // Make sure there is enough space for the needed blocks
    if(fs->superblock->num_free_dblocks < blks_needed) return -ENOSPC;

    int remainder = blks_needed;
    // Try and extend the last extent before allocating more blocks
    if(0 != inode->num_extents) 
    {
        a1fs_extent *last_extent = get_extent(inode, inode->num_extents-1, fs);
        // The number of free blocks  after the end of the last extent, which could be expanded into
        uint32_t room_for_growth = tail_length(last_extent->start+last_extent->count, fs);

        a1fs_tuple extention;
        extention.start = last_extent->start+last_extent->count; // Start right after the end of the original extent
        if(0 != room_for_growth)
        {
            if(room_for_growth >= blks_needed){
                extention.end = extention.start+blks_needed-1;
            }else{
                remainder = blks_needed - room_for_growth;
                extention.end = extention.start+blks_needed-remainder-1;
            }
            update_last_extent(inode, last_extent->start, extention.end-last_extent->start+1, fs);
        }
    }
    a1fs_tuple new_extent_info;
    while (0 < remainder)
    {
        // Find the longest free sequence
        first_free_sequence(remainder, &new_extent_info, fs);
        remainder = remainder - (new_extent_info.end-new_extent_info.start+1);
        // Mark the new extent.
        // Note: We assume that we never need more than 512 extents
        if(512 == inode->num_extents) return -ENOSPC;
        inode->num_extents++;
        
        update_last_extent(inode, new_extent_info.start, new_extent_info.end-new_extent_info.start+1, fs);
    }
    if(VERBOSE) print_data_block_bitmap("Alocation Complete", fs);
    return 0;
}

int add_dir_entry(const char *unmodified_path, mode_t mode, uint32_t links, fs_ctx *fs)
{
    char path[A1FS_PATH_MAX];
	strncpy(path, unmodified_path, A1FS_PATH_MAX);

	if (0 == fs->superblock->num_free_inodes) return -ENOSPC;

	// Find the last slash, the following string is the new directory name
	char * file_name = strrchr(path, '/')+1;
    if(strlen(file_name) > A1FS_NAME_MAX) return -ENAMETOOLONG;

	// The first part of the path is the path to the parent inode 
	char *parent_path;
	if (file_name-1 != path){ // If the last slash found is the root dir, then we don't want to set it to NULL term
		*(file_name-1) = '\0';
		parent_path = path;
	}else{
		parent_path = "/";
	}
		
	a1fs_inode *par_inode = &fs->inode_table[path_lookup(parent_path, fs)]; 

	// If the new file is a directory, it has a link to the parent
	if(S_ISDIR(mode)) par_inode->links++;

    a1fs_block_iterator b_iter;
    block_iterator_init(par_inode, &b_iter, fs);

	a1fs_dentry *cur_entry;
    void *cur_blk;
    // Iterate over the inodes data blocks
    while(NULL != (cur_blk = block_iterator_next_blk(&b_iter, fs))){
        // Iterate over the NUM_DENTRY_PER_BLOCK dir_entries in a block
        for(uint32_t d_ind = 0; d_ind < NUM_DENTRY_PER_BLOCK; d_ind++)
        {
            cur_entry = (a1fs_dentry *) (cur_blk + d_ind * sizeof(a1fs_dentry));
            // Check for an empty a1fs_dentry
            if('\0' == *cur_entry->name)
            {
                strncpy(cur_entry->name, file_name, A1FS_NAME_MAX);
                cur_entry->ino = find_empty_inode(fs);
                init_inode(cur_entry->ino, mode, links, fs->image);
                return 0;
            }
        }
    }
	
    // There was no room in any of the allocated blocks for the entry, so a new block is needed
	if (0 != allocate_data_blocks(par_inode, A1FS_BLOCK_SIZE, fs)) return -ENOSPC;
    par_inode->size += A1FS_BLOCK_SIZE;

    // Get the last block of the last extent
	a1fs_extent *cur_extent = get_extent(par_inode, par_inode->num_extents-1, fs);
	cur_entry  = (a1fs_dentry *)(fs->data_blks + (cur_extent->start + cur_extent->count - 1) * A1FS_BLOCK_SIZE);
	strncpy(cur_entry->name, file_name, A1FS_NAME_MAX);
	cur_entry->ino = find_empty_inode(fs);
	init_inode(cur_entry->ino, mode, links, fs->image);
	return 0;
}

void remove_dir_entry(const char *unmodified_path, fs_ctx *fs)
{
    char path[A1FS_PATH_MAX];
	strncpy(path, unmodified_path, A1FS_PATH_MAX);

	// Find the last slash, the following string is the new directory name
	char * file_name = strrchr(path, '/')+1;
	// The first part of the path is the path to the parent inode 
	char *parent_path;
	if (file_name-1 != path){ // If the last slash found is the root dir, then we don't want to set it to NULL term
		*(file_name-1) = '\0';
		parent_path = path;
	}else{
		parent_path = "/";
	}
		
	a1fs_inode *par_inode = &fs->inode_table[path_lookup(parent_path, fs)];
    a1fs_inode *inode     = &fs->inode_table[path_lookup(unmodified_path, fs)];

	if(S_ISDIR(inode->mode)) // If the file is a directory
    {
        inode->links--;     // Remove the link to itself (.)
        par_inode->links--; // Remove the link to its parent (..)
    }
    inode->links--; // Remove the link from the parent to the file


    a1fs_block_iterator b_iter;
    block_iterator_init(par_inode, &b_iter, fs);

	a1fs_dentry *cur_entry;
    void *cur_blk;
    // Iterate over the inodes data blocks
    while(NULL != (cur_blk = block_iterator_next_blk(&b_iter, fs))){
        // Iterate over the NUM_DENTRY_PER_BLOCK dir_entries in a block
        for(uint32_t d_ind = 0; d_ind < NUM_DENTRY_PER_BLOCK; d_ind++)
        {
            cur_entry = (a1fs_dentry *) (cur_blk + d_ind * sizeof(a1fs_dentry));
            // Check for file_name
            if(0 == strcmp(cur_entry->name, file_name))
            {
                *cur_entry->name = '\0';
            }
        }
    }

    if(0 == inode->links)
    { // The inode is now unallocated so mark unallocate its data blocks
        fs->superblock->num_free_inodes++;
        
        block_iterator_init(par_inode, &b_iter, fs);

        a1fs_extent *cur_extent;
        // Deallocate the data blocks
        // Iterate over the inodes data blocks
        for(uint32_t i = 0; i < inode->num_extents; i++)
        {
            cur_extent = get_extent(inode, i, fs);
            for(uint32_t b = cur_extent->start; b < cur_extent->start+cur_extent->count; b++){
                fs->d_bitmap[b/8] = fs->d_bitmap[b/8] & ~(1 << (b % 8));
                fs->superblock->num_free_dblocks += cur_extent->count;
            }
        }
        if (VERBOSE) print_data_block_bitmap("Dealocation Complete", fs);
    }
}

void block_iterator_init(a1fs_inode *inode, a1fs_block_iterator *b_iter, fs_ctx *fs)
{
    b_iter->inode = inode;
    if (0 != inode->num_extents)
    {
        b_iter->cur_extent = get_extent(inode, 0, fs);
    }else
    {
        b_iter->cur_extent = NULL;
    }
    b_iter->extent_index = 0;
    b_iter->blk_in_extent_index = 0;
}

void *block_iterator_next_blk(a1fs_block_iterator *b_iter, fs_ctx *fs)
{   
    // If there are no allocated extents
    if(NULL == b_iter->cur_extent) return NULL;

    // blk_in_extent_index is equal to the count then the extent is done, and we should go to the next one
    if(b_iter->blk_in_extent_index == b_iter->cur_extent->count){
        
        // If we're past the last extent return NULL since there are no blocks left
        b_iter->extent_index++;
        if(b_iter->extent_index == b_iter->inode->num_extents) return NULL;
        
        // Otherwise set current extent to the next extent and reset blk_in_extent_index to 0
        b_iter->cur_extent = get_extent(b_iter->inode, b_iter->extent_index, fs);
        b_iter->blk_in_extent_index = 0;
    }

    // Get the value of the block and then increment the blk_in_extent_index
    void *ptr = fs->data_blks + (b_iter->cur_extent->start + b_iter->blk_in_extent_index) * A1FS_BLOCK_SIZE;
    b_iter->blk_in_extent_index++;
    return ptr;
}

int copy_between_buf_and_fs(a1fs_inode *inode, char *buf, size_t size, off_t offset, bool to_fs, fs_ctx *fs)
{
    a1fs_block_iterator b_iter;
    block_iterator_init(inode, &b_iter, fs);

    void *cur_blk;
    off_t cur_offset = 0;
    off_t offset_within_blk;
    size_t bytes_written_so_far = 0;


    // Iterate over the inode's data blocks and check that size is not 0, meaning all the bytes have been written
    while(NULL != (cur_blk = block_iterator_next_blk(&b_iter, fs)) && size != 0)
    {
        if(cur_offset - offset <= A1FS_BLOCK_SIZE)
        {
            // We are in a block we want to write.read into/from (Either from the start or from an offset within the block)
            offset_within_blk = offset > cur_offset ? (offset - cur_offset) : 0;

            size_t bytes_to_write_in_blk = Min(A1FS_BLOCK_SIZE, size);
            if(to_fs)
            { // Write from buf to the file system (write)
                memcpy(cur_blk+offset_within_blk, buf+bytes_written_so_far, bytes_to_write_in_blk);
            }else
            { // Write from the file system to the buf (read)
                memcpy(buf+bytes_written_so_far, cur_blk+offset_within_blk, bytes_to_write_in_blk);
            }
            
            size -= bytes_to_write_in_blk;
            bytes_written_so_far += bytes_to_write_in_blk;
        }
        cur_offset += A1FS_BLOCK_SIZE; 
    }
    return bytes_written_so_far;
}

void print_data_block_bitmap(const char *msg, fs_ctx *fs)
{
    printf("-----%s----\n", msg);
    for(uint32_t i = 0; i < fs->superblock->num_tot_dblocks; i++)
    {
        printf("%d",0 != (fs->d_bitmap[i / 8] & (1 << (i % 8))));
    }
    printf("\n----------------\n");
}