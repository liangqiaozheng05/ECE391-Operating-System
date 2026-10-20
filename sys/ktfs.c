/*! @file ktfs.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠⁠‌​‌​‌​‌‌​⁠⁠‌
    @brief KTFS Implementation.
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef KTFS_TRACE
#define TRACE
#endif

#ifdef KTFS_DEBUG
#define DEBUG
#endif

#include "ktfs.h"

#include "cache.h"
#include "console.h"
#include "device.h"
#include "devimpl.h"
#include "error.h"
#include "filesys.h"
#include "fsimpl.h"
#include "heap.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "uio.h"
#include "uioimpl.h"

// INTERNAL TYPE DEFINITIONS
//

#define INODES_PER_BLK 16
#define ENTRIES_PER_DIR_BLK 32
#define ENTRIES_PER_INDIR_BLK 128
#define ENTRIES_PER_DINDIR_BLK 128

/// @brief File struct for a file in the Keegan Teal Filesystem
struct ktfs_file {
    // Fill to fulfill spec
    struct uio * cio;
    struct ktfs_dir_entry * dir;
    long size;
    long pos;
    const char * name;
};

/// @brief Listing struct for listing files in KTFS
struct ktfs_listing {
    struct uio *uio;
    struct ktfs_dir_entry *current_dir_entry;
    uint32_t current_dir_block_idx;    // current directory block index
    uint32_t current_entry_idx;        // current entry index within the current directory block
    uint32_t dir_blocks[KTFS_NUM_DIRECT_DATA_BLOCKS]; // cached directory block numbers
    uint32_t num_dir_blocks;           // actual number of directory blocks
    int eof;                           // whether end of listing reached
};

// Wrapper struct to associate uio with listing
struct uio_with_listing {
    struct uio uio;
    struct ktfs_listing *listing_ptr;
};

struct node {
    struct node * next;
    struct ktfs_file * file;
};

void ktfs_listing_close(struct uio* uio);
long ktfs_listing_read(struct uio* uio, void* buf, unsigned long bufsz);

// uio interface for listing
static struct uio_intf ktfs_listing_intf = {
    .close = ktfs_listing_close,
    .read = ktfs_listing_read,
};

struct ktfs_superblock * superblock;
struct filesystem * fs;
struct cache * cash;
struct node * open_fs_head;

// Bitmap helper functions
static int inode_bitmap_get(uint32_t ino, int *used) {
    uint32_t k = superblock->inode_bitmap_block_count;
    uint32_t bits_per_block = KTFS_BLKSZ * 8;
    uint32_t blk_idx = ino / bits_per_block;
    uint32_t bit_in_block = ino % bits_per_block;
    if (blk_idx >= k) return -ENOTSUP;

    void *bm_data;
    int check = cache_get_block(cash, (1 + blk_idx) * CACHE_BLKSZ, &bm_data);
    if (check != 0) return -ENOTSUP;
    struct ktfs_bitmap *bm = bm_data;
    uint32_t byte_idx = bit_in_block / 8;
    uint8_t mask = 1u << (bit_in_block % 8);
    *used = (bm->bytes[byte_idx] & mask) ? 1 : 0;
    cache_release_block(cash, bm_data, 0);
    return 0;
}

static int inode_bitmap_set(uint32_t ino, int used) {
    uint32_t k = superblock->inode_bitmap_block_count;
    uint32_t bits_per_block = KTFS_BLKSZ * 8;
    uint32_t blk_idx = ino / bits_per_block;
    uint32_t bit_in_block = ino % bits_per_block;
    if (blk_idx >= k) return -ENOTSUP;

    void *bm_data;
    int check = cache_get_block(cash, (1 + blk_idx) * CACHE_BLKSZ, &bm_data);
    if (check != 0) return -ENOTSUP;
    struct ktfs_bitmap *bm = bm_data;
    uint32_t byte_idx = bit_in_block / 8;
    uint8_t mask = 1u << (bit_in_block % 8);
    if (used)
        bm->bytes[byte_idx] |= mask;
    else
        bm->bytes[byte_idx] &= (uint8_t)~mask;
    cache_release_block(cash, bm_data, 1);
    return 0;
}

static int block_bitmap_get(uint32_t blkno, int *used) {
    uint32_t k = superblock->inode_bitmap_block_count;
    uint32_t b = superblock->bitmap_block_count;
    uint32_t bits_per_block = KTFS_BLKSZ * 8;
    if (blkno >= superblock->block_count) return -ENOTSUP;
    uint32_t blk_idx = blkno / bits_per_block;
    uint32_t bit_in_block = blkno % bits_per_block;
    if (blk_idx >= b) return -ENOTSUP;

    void *bm_data;
    int check = cache_get_block(cash, (1 + k + blk_idx) * CACHE_BLKSZ, &bm_data);
    if (check != 0) return -ENOTSUP;
    struct ktfs_bitmap *bm = bm_data;
    uint32_t byte_idx = bit_in_block / 8;
    uint8_t mask = 1u << (bit_in_block % 8);
    *used = (bm->bytes[byte_idx] & mask) ? 1 : 0;
    cache_release_block(cash, bm_data, 0);
    return 0;
}

static int block_bitmap_set(uint32_t blkno, int used) {
    uint32_t k = superblock->inode_bitmap_block_count;
    uint32_t b = superblock->bitmap_block_count;
    uint32_t bits_per_block = KTFS_BLKSZ * 8;
    if (blkno >= superblock->block_count) return -ENOTSUP;
    uint32_t blk_idx = blkno / bits_per_block;
    uint32_t bit_in_block = blkno % bits_per_block;
    if (blk_idx >= b) return -ENOTSUP;

    void *bm_data;
    int check = cache_get_block(cash, (1 + k + blk_idx) * CACHE_BLKSZ, &bm_data);
    if (check != 0) return -ENOTSUP;
    struct ktfs_bitmap *bm = bm_data;
    uint32_t byte_idx = bit_in_block / 8;
    uint8_t mask = 1u << (bit_in_block % 8);
    if (used)
        bm->bytes[byte_idx] |= mask;
    else
        bm->bytes[byte_idx] &= (uint8_t)~mask;
    cache_release_block(cash, bm_data, 1);
    return 0;
}

static int ktfs_alloc_inode(uint16_t *ino_out) {
    uint32_t total_inodes = superblock->inode_block_count * INODES_PER_BLK;
    for (uint32_t i = 1; i < total_inodes; i++) {
        if (i == superblock->root_directory_inode) continue;
        int used = 0;
        if (inode_bitmap_get(i, &used) != 0) continue;
        if (!used) {
            if (inode_bitmap_set(i, 1) != 0) continue;
            *ino_out = (uint16_t)i;
            return 0;
        }
    }
    return -ENOINODEBLKS;
}

static int ktfs_free_inode(uint16_t ino) {
    return inode_bitmap_set(ino, 0);
}

static int ktfs_alloc_block(uint32_t *blk_out) {
    uint32_t k = superblock->inode_bitmap_block_count;
    uint32_t b = superblock->bitmap_block_count;
    uint32_t n = superblock->inode_block_count;
    uint32_t first_data = 1 + k + b + n;
    for (uint32_t blk = first_data; blk < superblock->block_count; blk++) {
        int used = 0;
        if (block_bitmap_get(blk, &used) != 0) continue;
        if (!used) {
            if (block_bitmap_set(blk, 1) != 0) continue;
            // Zero the new block
            void *data;
            int check = cache_get_block(cash, blk * CACHE_BLKSZ, &data);
            if (check == 0) {
                memset(data, 0, CACHE_BLKSZ);
                cache_release_block(cash, data, 1);
            }
            *blk_out = blk;
            return 0;
        }
    }
    return -ENODATABLKS;
}

static struct ktfs_listing * get_listing(struct uio* uio) {
    // get listing from uio
    if (uio->intf == &ktfs_listing_intf) {
        struct uio_with_listing *wrapper = (struct uio_with_listing *)uio;
        return wrapper->listing_ptr;
    }
    return NULL;
}

static int ktfs_free_block(uint32_t blk) {
    return block_bitmap_set(blk, 0);
}

// Helper to get/create data block for logical block index in a file
static int ktfs_get_block(struct ktfs_inode *inode, long block_idx, int allocate, uint32_t *blk_out);
static int ktfs_free_logical_block(struct ktfs_inode *inode, long block_idx);


// INTERNAL FUNCTION DECLARATIONS
//

int ktfs_open(struct filesystem* fs, const char* name, struct uio** uioptr);
void ktfs_close(struct uio* uio);
int ktfs_cntl(struct uio* uio, int cmd, void* arg);
long ktfs_fetch(struct uio* uio, void* buf, unsigned long len);
long ktfs_store(struct uio* uio, const void* buf, unsigned long len);
int ktfs_create(struct filesystem* fs, const char* name);
int ktfs_delete(struct filesystem* fs, const char* name);
void ktfs_flush(struct filesystem* fs);
void ktfs_listing_close(struct uio* uio);
long ktfs_listing_read(struct uio* uio, void* buf, unsigned long bufsz);

// Helper Functions
static int search_for_dentry(uint32_t dentry_idx, const char* name, struct uio** uioptr, uint32_t inode0_block);
static struct node * get_uio(struct uio* uio);
static long ktfs_fetch_helper(uint32_t data_b, long * progress, long pos, long* bytes_read,
                              void* buf, unsigned long len, long size);
static long to_buf(uint32_t data_b, long offset, void* nowbuf, long to_read);
int ktfs_setend(struct node* cur, int cmd, long new_size);

// Static UIO Interface
static struct uio_intf ktfs_f_intf = {
    .close = ktfs_close,
    .read = ktfs_fetch,
    .write = ktfs_store,
    .cntl = ktfs_cntl
};

static uint32_t ktfs_first_data_block = 0;

/**
 * @brief Mounts the file system with associated backing cache
 * @param cache Pointer to cache struct for the file system
 * @return 0 if mount successful, negative error code if error
 */
int mount_ktfs(const char* name, struct cache* cache) {
    int check = cache_get_block(cache, 0, (void**)&superblock);
    if (check != 0){
        return check;
    }
            
    ktfs_first_data_block = 1 + superblock->inode_bitmap_block_count + 
                           superblock->bitmap_block_count + superblock->inode_block_count;

    cash = cache;
    fs = kmalloc(sizeof(struct filesystem));
    fs->open = ktfs_open;
    fs->create = ktfs_create;
    fs->delete = ktfs_delete;
    fs->flush = ktfs_flush;
    int check2 = attach_filesystem(name, fs);
    if (check2 != 0){
        cache_release_block(cache, superblock, 0);
        kfree(fs);
        return check2;
    }
    return 0;
    
}

/**
 * @brief Opens a file or ls (listing) with the given name and returns a pointer to the uio through
 * the double pointer
 * @param name The name of the file to open or "\" for listing (CP3)
 * @param uioptr Will return a pointer to a file or ls (list) uio pointer through this double
 * pointer
 * @return 0 if open successful, negative error code if error
 */
int ktfs_open(struct filesystem* fs, const char* name, struct uio** uioptr) {
    // Handle empty string for directory listing (CP3)
    if (name == NULL || strcmp(name, "") == 0) {
        // create a new listing struct
        struct ktfs_listing *listing = kmalloc(sizeof(struct ktfs_listing));
        if (listing == NULL) {
            return -ENOMEM;
        }
        
        // initialize listing fields
        memset(listing, 0, sizeof(struct ktfs_listing));
        
        // get root inode (inode 0)
        uint32_t k = superblock->inode_bitmap_block_count;
        uint32_t b = superblock->bitmap_block_count;
        uint32_t inode0_block = 1 + k + b;
        
        void* inode0_data;
        int check = cache_get_block(cash, inode0_block * CACHE_BLKSZ, &inode0_data);
        if (check != 0) {
            kfree(listing);
            return check;
        }
        
        struct ktfs_inode *inode0 = inode0_data;
        listing->num_dir_blocks = 0;
        
        // collect valid direct directory blocks
        for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++) {
            // Check if this block is within the file size
            if (i * KTFS_BLKSZ < inode0->size) {
                listing->dir_blocks[listing->num_dir_blocks] = inode0->block[i] + ktfs_first_data_block;
                listing->num_dir_blocks++;
            }
        }
        
        // reset to beginning of listing
        listing->current_dir_block_idx = 0;
        listing->current_entry_idx = 0;
        listing->current_dir_entry = NULL;
        listing->eof = (listing->num_dir_blocks == 0);
        
        cache_release_block(cash, inode0_data, 0);
        
        // connect listing with uio
        struct uio_with_listing *wrapper = kmalloc(sizeof(struct uio_with_listing));
        if (wrapper == NULL) {
            kfree(listing);
            return -ENOMEM;
        }
        
        // init UIO
        wrapper->uio.intf = &ktfs_listing_intf;
        wrapper->uio.refcnt = 1;
        wrapper->listing_ptr = listing;
        
        *uioptr = &wrapper->uio;
        
        return 0;
    }

    uint32_t k = superblock->inode_bitmap_block_count;
    uint32_t b = superblock->bitmap_block_count;   
    uint32_t inode0_block = 1+k+b;
    
    void* inode0_data;
    int check = cache_get_block(cash, inode0_block * CACHE_BLKSZ, &inode0_data);
    if (check != 0){
        return check;
    }

    struct ktfs_inode * inode0 = inode0_data;
    for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++){
        uint32_t blk = inode0->block[i];
        if (i * KTFS_BLKSZ < inode0->size) {
             blk += ktfs_first_data_block;
        } else {
             if (blk == 0) continue; // Skip empty
             blk += ktfs_first_data_block; // Should not happen if size is correct
        }

        check = search_for_dentry(blk, name , uioptr, inode0_block);
        if (check == 0){
            cache_release_block(cash, inode0_data, 0);
            return 0;
        }
        else if (check < 0){
            cache_release_block(cash, inode0_data, 0);
            return check;
        }
    } 
    if (inode0->indirect != 0){
        void * indirect_data;
        check = cache_get_block(cash, inode0->indirect * CACHE_BLKSZ, &indirect_data);
        
        if (check != 0){
            cache_release_block(cash, inode0_data, 0);
            return check;
        }
        
        uint32_t * indirect_b = indirect_data;

        for (int j = 0; j < ENTRIES_PER_INDIR_BLK; j++){
            check = search_for_dentry(indirect_b[j], name , uioptr, inode0_block);
            if (check == 0){

                cache_release_block(cash, indirect_data, 0);
                cache_release_block(cash, inode0_data, 0);
                return 0;
            }
            else if (check<0){
                cache_release_block(cash, indirect_data, 0);
                cache_release_block(cash, inode0_data, 0);
                return check;
            }

        }
        cache_release_block(cash, indirect_data, 0);
    }
    for (int k = 0; k < KTFS_NUM_DINDIRECT_BLOCKS; k++){      
        if (inode0->dindirect[k] == 0){
            debug("REACHING HERE, bro\n"); // DEBUG
            debug("%s: inode->dindirect[%d] (at %p)", __func__, k, &(inode0->dindirect[k])); // DEBUG
            break;
        }      
        void * dindirect_data;
        check = cache_get_block(cash, inode0->dindirect[k] * CACHE_BLKSZ, &dindirect_data);       
        if (check != 0){
            cache_release_block(cash, inode0_data, 0);
            return check;
        }
        uint32_t * dindirect_b = dindirect_data;
        for (int x = 0; x < ENTRIES_PER_DINDIR_BLK; x++){
            if (dindirect_b[x] == 0){
                break;
            }
            void * ind_data;
            check = cache_get_block(cash, dindirect_b[x] * CACHE_BLKSZ, &ind_data);
            
            if (check != 0){
                cache_release_block(cash, dindirect_data, 0);
                cache_release_block(cash, inode0_data, 0);
                return check;
            }
            uint32_t * indirect_b_from_dind = ind_data;
            for (int y = 0; y < ENTRIES_PER_INDIR_BLK; y++){
                check = search_for_dentry(indirect_b_from_dind[y], name, uioptr, inode0_block);
                debug("ktfs_open: check = %d\n", check); // DEBUG
                if (check == 0){
                    cache_release_block(cash, ind_data, 0);
                    cache_release_block(cash, dindirect_data, 0);
                    cache_release_block(cash, inode0_data, 0);
                    return 0;
                }
                else if (check < 0){
                    cache_release_block(cash, ind_data, 0);
                    cache_release_block(cash, dindirect_data, 0);
                    cache_release_block(cash, inode0_data, 0);
                    debug("ktfs_open: check: %s\n", error_name(check)); // DEBUG
                    return check;
                }
            }
            cache_release_block(cash, ind_data, 0);
        }
        cache_release_block(cash, dindirect_data, 0);
    }
    cache_release_block(cash, inode0_data, 0);
    return -ENOENT;
}

// Helper function
static int search_for_dentry(uint32_t dentry_idx, const char* name, struct uio ** uioptr, uint32_t inode0_block){   
    if (dentry_idx==0){
        return 1;
    }
    void* root_block_data;     
    int check = cache_get_block(cash, dentry_idx * CACHE_BLKSZ, &root_block_data);   
    if (check != 0){
        return check;
    }
    struct ktfs_dir_entry * dentries = root_block_data;
    for (int j = 0; j < ENTRIES_PER_DIR_BLK; j++){
        if (dentries[j].inode != 0 && strcmp(dentries[j].name, name) == 0){
            struct node * tempnode = open_fs_head;
            while (tempnode != NULL){
                if (strcmp(tempnode->file->name, name) == 0){
                    cache_release_block(cash, root_block_data, 0);
                    return -EBUSY;
                }
                tempnode = tempnode->next;
            }
            uint16_t file_inode = dentries[j].inode;
            uint32_t inode_idx = file_inode / INODES_PER_BLK;
            void* inode_f_data;
            check = cache_get_block(cash, (inode0_block+inode_idx) * CACHE_BLKSZ, &inode_f_data);
            if (check != 0){
                cache_release_block(cash, root_block_data, 0);
                return -ENOTSUP;
            }         
            uint32_t inode_b_offset = file_inode % INODES_PER_BLK;          
            struct ktfs_inode * f_inode = (struct ktfs_inode *)((uint8_t*)inode_f_data + (inode_b_offset*KTFS_INOSZ));         
            struct ktfs_file * new_f = kmalloc(sizeof(struct ktfs_file));
            struct uio * new_uio = kmalloc(sizeof(struct uio));
            new_f->dir = kmalloc(sizeof(struct ktfs_dir_entry));

            new_f->cio = uio_init1(new_uio, &ktfs_f_intf);
            *new_f->dir = dentries[j];
            new_f->size = f_inode->size;
            new_f->pos = 0;
            new_f->name = new_f->dir->name;
            struct node * insert = kmalloc(sizeof(struct node));
            insert->file = new_f;
            insert->next = open_fs_head;
            open_fs_head = insert;

            *uioptr = new_uio;          
            cache_release_block(cash, inode_f_data, 0);
            cache_release_block(cash, root_block_data, 0);
            return 0;
        }
    }
    cache_release_block(cash, root_block_data, 0);
    return 1;
}

void ktfs_close(struct uio* uio) {
    // Handle listing close
    if (uio->intf == &ktfs_listing_intf) {
        struct ktfs_listing *listing = get_listing(uio);
        if (listing != NULL) {
            // Free listing resources
            listing->current_dir_entry = NULL;
            listing->uio = NULL;
            kfree(listing);
        }
        kfree(uio);
        return;
    }

    if (open_fs_head != NULL){
        struct node * cur = open_fs_head;
        struct node * prev = NULL;
        int close_head = 0;
        int f_found = 0;
        while (cur != NULL){
            if (cur->file->cio == uio){
                if (cur == open_fs_head){
                    close_head = 1;
                }
                f_found = 1;
                break;
            }
            prev = cur;
            cur = cur->next;
        }
        if (f_found != 1){
            return;
        }
        if (close_head == 1){
            open_fs_head = open_fs_head->next;
        }
        else{
            prev->next = cur->next;
        }
        kfree(cur->file->dir);
        kfree(cur->file->cio);
        kfree(cur->file);
        kfree(cur);
    }
    return;
}

static struct node * get_uio(struct uio* uio){
    struct node * cur = open_fs_head;
    while(cur != NULL){
        if (cur->file->cio == uio){
            break;
        }
        cur = cur->next;
    }
    return cur;
}
/**
 * @brief Reads data from file attached to uio into provided argument buffer
 * @param uio uio of file to be read
 * @param buf Buffer to be filled
 * @param len Number of bytes to read
 * @return Number of bytes read if successful, negative error code if error
 */
long ktfs_fetch(struct uio* uio, void* buf, unsigned long len) {
    uint32_t k = superblock->inode_bitmap_block_count;
    uint32_t b = superblock->bitmap_block_count;
    uint32_t inode0_block = 1 + k + b;
    struct node * cur = get_uio(uio);
    if (cur == NULL){
        return -EINVAL;
    }
    struct ktfs_file * file_to_read = cur->file;

    
    void * inode_data;
    int check = cache_get_block(cash, (inode0_block+(file_to_read->dir->inode / INODES_PER_BLK)) * CACHE_BLKSZ, &inode_data);
        
    if (check != 0){
        return check;
    }
    uint32_t inode_offset = file_to_read->dir->inode % INODES_PER_BLK;
    struct ktfs_inode * inode_to_read = (struct ktfs_inode *)((uint8_t*) inode_data + (inode_offset*KTFS_INOSZ));
    long progress = 0;
    long bytes_read = 0;
    int done = 0;

    for (int i = 0; (i < KTFS_NUM_DIRECT_DATA_BLOCKS) && (done == 0); i++){
        uint32_t blk = inode_to_read->block[i];
        if (blk != 0 || (i * KTFS_BLKSZ < inode_to_read->size)) {
            blk += ktfs_first_data_block;
        }
        long check2 = ktfs_fetch_helper(blk, &progress, file_to_read->pos,
                                        &bytes_read, buf, len, file_to_read->size);
        if (check2 < 0){
            cache_release_block(cash, inode_data, 0);
            return check2;
        }
        if (check2 == 1){
            done = 1;
        }

    }
    if (inode_to_read->indirect != 0 && done == 0){
        void* indirect_b;
        uint32_t indirect_blk = inode_to_read->indirect + ktfs_first_data_block;
        check = cache_get_block(cash, indirect_blk * CACHE_BLKSZ, &indirect_b);
        if (check != 0){
            cache_release_block(cash, inode_data, 0);
            return -ENOTSUP;
        }
        uint32_t * indirect_data = indirect_b;
        for (int j = 0; j < ENTRIES_PER_INDIR_BLK && done == 0; j++){
            uint32_t blk = indirect_data[j];
            if (blk != 0){
                blk += ktfs_first_data_block;
            }
            long check3 = ktfs_fetch_helper(blk, &progress, file_to_read->pos, 
                                            &bytes_read, buf, len, file_to_read->size);
            if (check3 < 0){
                cache_release_block(cash, indirect_b, 0);
                cache_release_block(cash, inode_data, 0);
                return check3;
            }
            if (check3 == 1){
                done = 1;
            }
        }
        cache_release_block(cash, indirect_b, 0);
    }
    for (int k = 0; k < KTFS_NUM_DINDIRECT_BLOCKS && done == 0; k++){
        if (inode_to_read->dindirect[k] == 0) {
            continue;
        }
        void* dindirect_b;
        uint32_t dindirect_blk = inode_to_read->dindirect[k] + ktfs_first_data_block;
        check = cache_get_block(cash, dindirect_blk * CACHE_BLKSZ, &dindirect_b);
        if (check != 0){
            cache_release_block(cash, inode_data, 0);
            return check;
        }
        
        uint32_t * dindirect_data = dindirect_b;
        for (int a = 0; a < ENTRIES_PER_DINDIR_BLK && done==0; a++){
            if (dindirect_data[a] == 0) {
                continue;
            }
            void* indirect_b_from_din;
            uint32_t ind_blk = dindirect_data[a] + ktfs_first_data_block;
            check = cache_get_block(cash, ind_blk * CACHE_BLKSZ, &indirect_b_from_din);
            
            if (check != 0){
                cache_release_block(cash, dindirect_b, 0);
                cache_release_block(cash, inode_data, 0);
                return -ENOTSUP;
            }
            uint32_t * indirect_data_from_dindirect = indirect_b_from_din;

            for (int b = 0; b< ENTRIES_PER_INDIR_BLK && done == 0; b++){
                if (indirect_data_from_dindirect[b] == 0) {
                    continue;
                }
                uint32_t blk = indirect_data_from_dindirect[b] + ktfs_first_data_block;
                long check2 = ktfs_fetch_helper(blk, &progress, file_to_read->pos,
                                                &bytes_read, buf, len, file_to_read->size);
                if (check2<0){
                    cache_release_block(cash, indirect_b_from_din, 0);
                    cache_release_block(cash, dindirect_b, 0);
                    cache_release_block(cash, inode_data, 0);
                    return check2;
                }
                if (check2 == 1){
                    done = 1;
                }
            }
            cache_release_block(cash, indirect_b_from_din, 0);


        }
        cache_release_block(cash, dindirect_b, 0);
    }
    file_to_read->pos += bytes_read;
    cache_release_block(cash, inode_data, 0);
    

    return bytes_read;
}

// Helper function for ktfs fetch
static long ktfs_fetch_helper(uint32_t data_b, long * progress, long pos, long* bytes_read, void* buf, unsigned long len, long size){
    if (*progress + CACHE_BLKSZ <= pos){
        *progress += CACHE_BLKSZ;
        return 0;
    }

    if (*bytes_read >= len || pos + *bytes_read >= size){
        return 1;
    }
    
    long offset = 0;

    if (*progress < pos){
        offset = pos - *progress;
    }
    
    long to_read = CACHE_BLKSZ - offset;

    if (to_read > len - *bytes_read){
        to_read = len - *bytes_read;
    }
    long remains = size - (pos+ *bytes_read);
    if (to_read>remains){
        to_read = remains;
    }
    void * nowbuf = (uint8_t*)buf + *bytes_read;
    long check = to_buf(data_b, offset, nowbuf, to_read);

    if (check < 0){
        return check;
    }

    *bytes_read += to_read;
    *progress += CACHE_BLKSZ;

    return 0;

}

// Helper function
static long to_buf(uint32_t data_b, long offset, void* nowbuf, long to_read){
    void * actual_data;
    int check = cache_get_block(cash, data_b * CACHE_BLKSZ, &actual_data);
    if (check != 0){
        return -ENOTSUP;
    }
    uint8_t * the_data_with_offset = (uint8_t*)actual_data + offset;
    memcpy(nowbuf, the_data_with_offset, to_read);
    cache_release_block(cash, actual_data, 0);
    return to_read;
}

// Map logical block index -> data block (optionally allocate)
static int ktfs_get_block(struct ktfs_inode *inode, long block_idx, int allocate, uint32_t *blk_out) {
    if (block_idx < 0) return -EINVAL;

    // Direct blocks
    if (block_idx < KTFS_NUM_DIRECT_DATA_BLOCKS) {
        uint32_t blk = inode->block[block_idx];
        if (blk == 0 && allocate == KTFS_ALLOCATE) {
            uint32_t newblk;
            int err = ktfs_alloc_block(&newblk);
            if (err != 0) return err;
            inode->block[block_idx] = newblk - ktfs_first_data_block;
            blk = newblk - ktfs_first_data_block;
        }
        
        if (blk != 0 || (block_idx * KTFS_BLKSZ < inode->size)) {
            *blk_out = blk + ktfs_first_data_block;
        } else {
            *blk_out = 0;
        }
        return 0;
    }

    block_idx -= KTFS_NUM_DIRECT_DATA_BLOCKS;

    // Indirect region
    if (block_idx < ENTRIES_PER_INDIR_BLK) {
        if (inode->indirect == 0 && allocate == KTFS_ALLOCATE) {
            uint32_t newblk;
            int err = ktfs_alloc_block(&newblk);
            if (err != 0) return err;
            inode->indirect = newblk - ktfs_first_data_block;
        }
        if (inode->indirect == 0 && inode->size <= KTFS_NUM_DIRECT_DATA_BLOCKS * KTFS_BLKSZ) {
            *blk_out = 0;
            return 0;
        }
        
        // Indirect block index is also relative
        uint32_t indirect_blk_abs = inode->indirect + ktfs_first_data_block;
        
        void *ind_data;
        if (cache_get_block(cash, indirect_blk_abs * CACHE_BLKSZ, &ind_data) != 0) return -ENOTSUP;
        uint32_t *ind_blocks = ind_data;
        uint32_t blk = ind_blocks[block_idx];
        if (blk == 0 && allocate == KTFS_ALLOCATE) {
            uint32_t newblk;
            int err = ktfs_alloc_block(&newblk);
            if (err != 0) {
                cache_release_block(cash, ind_data, 0);
                return err;
            }
            ind_blocks[block_idx] = newblk - ktfs_first_data_block;
            blk = newblk - ktfs_first_data_block;
            cache_release_block(cash, ind_data, 1);
        } else {
            cache_release_block(cash, ind_data, 0);
        }
        
        if (blk != 0 || ((KTFS_NUM_DIRECT_DATA_BLOCKS + block_idx) * KTFS_BLKSZ < inode->size)) {
             *blk_out = blk + ktfs_first_data_block;
        } else {
             *blk_out = 0;
        }
        return 0;
    }

    block_idx -= ENTRIES_PER_INDIR_BLK;

    // Double-indirect region
    long per_dind = ENTRIES_PER_DINDIR_BLK * ENTRIES_PER_INDIR_BLK;
    for (int d = 0; d < KTFS_NUM_DINDIRECT_BLOCKS; d++) {
        if (block_idx >= per_dind) {
            block_idx -= per_dind;
            continue;
        }
        // work inside d-th dindirect
        if (inode->dindirect[d] == 0 && allocate == KTFS_ALLOCATE) {
            uint32_t newblk;
            int err = ktfs_alloc_block(&newblk);
            if (err != 0) return err;
            inode->dindirect[d] = newblk;
            debug("%s: inode->dindirect[%d] (at %p) is set as %d", __func__, d, &(inode->dindirect[d]), newblk); // DEBUG
        }
        if (inode->dindirect[d] == 0) {
            *blk_out = 0;
            return 0;
        }
        void *dind_data;
        if (cache_get_block(cash, inode->dindirect[d] * CACHE_BLKSZ, &dind_data) != 0) return -ENOTSUP;
        uint32_t *dind_blocks = dind_data;

        long idx1 = block_idx / ENTRIES_PER_INDIR_BLK;
        long idx2 = block_idx % ENTRIES_PER_INDIR_BLK;

        if (idx1 >= ENTRIES_PER_DINDIR_BLK) {
            cache_release_block(cash, dind_data, 0);
            *blk_out = 0;
            return 0;
        }

        if (dind_blocks[idx1] == 0 && allocate == KTFS_ALLOCATE) {
            uint32_t newblk;
            int err = ktfs_alloc_block(&newblk);
            if (err != 0) {
                cache_release_block(cash, dind_data, 0);
                return err;
            }
            dind_blocks[idx1] = newblk;
            cache_release_block(cash, dind_data, 1);
        } else {
            cache_release_block(cash, dind_data, 0);
        }

        if (dind_blocks[idx1] == 0) {
            *blk_out = 0;
            return 0;
        }

        void *ind_data;
        if (cache_get_block(cash, dind_blocks[idx1] * CACHE_BLKSZ, &ind_data) != 0) return -ENOTSUP;
        uint32_t *ind_blocks = ind_data;
        uint32_t blk = ind_blocks[idx2];
        if (blk == 0 && allocate == KTFS_ALLOCATE) {
            uint32_t newblk;
            int err = ktfs_alloc_block(&newblk);
            if (err != 0) {
                cache_release_block(cash, ind_data, 0);
                return err;
            }
            ind_blocks[idx2] = newblk;
            blk = newblk;
            cache_release_block(cash, ind_data, 1);
        } else {
            cache_release_block(cash, ind_data, 0);
        }
        *blk_out = blk;
        return 0;
    }

    *blk_out = 0;
    return 0;
}

// Free mapping for logical block index and underlying data/indirect blocks if empty
static int ktfs_free_logical_block(struct ktfs_inode *inode, long block_idx) {
    if (block_idx < 0) return -EINVAL;

    // Direct blocks
    if (block_idx < KTFS_NUM_DIRECT_DATA_BLOCKS) {
        uint32_t blk = inode->block[block_idx];
        if (blk != 0) {
            ktfs_free_block(blk);
            inode->block[block_idx] = 0;
        }
        return 0;
    }

    block_idx -= KTFS_NUM_DIRECT_DATA_BLOCKS;

    // Indirect blocks
    if (block_idx < ENTRIES_PER_INDIR_BLK) {
        if (inode->indirect == 0) return 0;
        void *ind_data;
        if (cache_get_block(cash, inode->indirect * CACHE_BLKSZ, &ind_data) != 0) return -ENOTSUP;
        uint32_t *ind_blocks = ind_data;
        uint32_t blk = ind_blocks[block_idx];
        if (blk != 0) {
            ktfs_free_block(blk);
            ind_blocks[block_idx] = 0;
        }
        // Check if indirect block is now empty
        int empty = 1;
        for (int i = 0; i < ENTRIES_PER_INDIR_BLK; i++) {
            if (ind_blocks[i] != 0) {
                empty = 0;
                break;
            }
        }
        cache_release_block(cash, ind_data, empty ? 1 : 1);
        if (empty) {
            ktfs_free_block(inode->indirect);
            inode->indirect = 0;
        }
        return 0;
    }

    block_idx -= ENTRIES_PER_INDIR_BLK;

    // Double-indirect
    long per_dind = ENTRIES_PER_DINDIR_BLK * ENTRIES_PER_INDIR_BLK;
    for (int d = 0; d < KTFS_NUM_DINDIRECT_BLOCKS; d++) {
        if (block_idx >= per_dind) {
            block_idx -= per_dind;
            continue;
        }
        if (inode->dindirect[d] == 0) return 0;
        void *dind_data;
        if (cache_get_block(cash, inode->dindirect[d] * CACHE_BLKSZ, &dind_data) != 0) return -ENOTSUP;
        uint32_t *dind_blocks = dind_data;

        long idx1 = block_idx / ENTRIES_PER_INDIR_BLK;
        long idx2 = block_idx % ENTRIES_PER_INDIR_BLK;
        if (idx1 >= ENTRIES_PER_DINDIR_BLK) {
            cache_release_block(cash, dind_data, 0);
            return 0;
        }

        if (dind_blocks[idx1] != 0) {
            void *ind_data;
            if (cache_get_block(cash, dind_blocks[idx1] * CACHE_BLKSZ, &ind_data) == 0) {
                uint32_t *ind_blocks = ind_data;
                uint32_t blk = ind_blocks[idx2];
                if (blk != 0) {
                    ktfs_free_block(blk);
                    ind_blocks[idx2] = 0;
                }
                // Check if indirect block now empty
                int empty_ind = 1;
                for (int i = 0; i < ENTRIES_PER_INDIR_BLK; i++) {
                    if (ind_blocks[i] != 0) {
                        empty_ind = 0;
                        break;
                    }
                }
                cache_release_block(cash, ind_data, 1);
                if (empty_ind) {
                    ktfs_free_block(dind_blocks[idx1]);
                    dind_blocks[idx1] = 0;
                }
            }
        }

        // Check if dindirect block is now empty
        int empty_dind = 1;
        for (int i = 0; i < ENTRIES_PER_DINDIR_BLK; i++) {
            if (dind_blocks[i] != 0) {
                empty_dind = 0;
                break;
            }
        }
        cache_release_block(cash, dind_data, 1);
        if (empty_dind) {
            ktfs_free_block(inode->dindirect[d]);
            inode->dindirect[d] = 0;
        }
        return 0;
    }
    return 0;
}

/**
 * @brief Write data from the provided argument buffer into file attached to uio
 * @param uio The file to be written to
 * @param buf The buffer to be read from
 * @param len Number of bytes to write from the buffer to the file
 * @return Number of bytes written from the buffer to the file system if sucessful, negative error
 * code if error
 */
long ktfs_store(struct uio* uio, const void* buf, unsigned long len) {
    // by Xinyi Zheng
    struct node *cur = get_uio(uio);
    if (cur == NULL) {
        return -ENOTSUP;
    }
    struct ktfs_file *file_to_write = cur->file;
    long end_pos = file_to_write->pos + len;
    if (end_pos > file_to_write->size) {
        int result = ktfs_cntl(uio, FCNTL_SETEND, &end_pos);
        if (result != 0) {
            return result;
        }
    }
    uint32_t k = superblock->inode_bitmap_block_count;
    uint32_t b = superblock->bitmap_block_count;
    uint32_t inode0_block = 1 + k + b;
    void *inode_data;
    int check = cache_get_block(cash, (inode0_block + (file_to_write->dir->inode / INODES_PER_BLK)) * CACHE_BLKSZ, &inode_data);
    if (check != 0) {
        return check;
    }
    uint32_t inode_offset = file_to_write->dir->inode % INODES_PER_BLK;
    struct ktfs_inode *inode_to_write = (struct ktfs_inode *)((uint8_t*) inode_data + (inode_offset * KTFS_INOSZ));
    long bytes_written = 0;
    long current_pos = file_to_write->pos;

    while (bytes_written < len) {
        long block_idx = current_pos / CACHE_BLKSZ;
        long block_offset = current_pos % CACHE_BLKSZ;

        uint32_t data_block = 0;
        int res = ktfs_get_block(inode_to_write, block_idx, KTFS_NO_ALLOCATION, &data_block);
        if (res != 0 || data_block == 0) {
            cache_release_block(cash, inode_data, 0);
            return -ENODATABLKS;
        }
        void* block_data;
        check = cache_get_block(cash, data_block * CACHE_BLKSZ, &block_data);
        if (check != 0) {
            cache_release_block(cash, inode_data, 0);
            return check;
        }
        long bytes_to_write = MIN(len - bytes_written, CACHE_BLKSZ - block_offset);
        memcpy((uint8_t*)block_data + block_offset, (const uint8_t*)buf + bytes_written, bytes_to_write);
        cache_release_block(cash, block_data, 1);
        bytes_written += bytes_to_write;
        current_pos += bytes_to_write;
        file_to_write->pos += bytes_to_write;
    }
    cache_release_block(cash, inode_data, 0);
    return bytes_written;
}

/**
 * @brief Create a new file in the file system
 * @param fs The file system in which to create the file
 * @param name The name of the file
 * @return 0 if successful, negative error code if error
 */
int ktfs_create(struct filesystem* fs, const char* name) {
    struct uio* test_uio;
    if (ktfs_open(fs, name, &test_uio) == 0) {
        uio_close(test_uio);
        return -EEXIST;
    }   
    uint32_t k = superblock->inode_bitmap_block_count;
    uint32_t b = superblock->bitmap_block_count;
    uint32_t inode0_block = 1 + k + b;    
    void* inode0_data;
    int check = cache_get_block(cash, inode0_block * CACHE_BLKSZ, &inode0_data);
    if (check != 0){
        return check;
    }    
    struct ktfs_inode *root_inode = inode0_data;
    struct ktfs_dir_entry* free_slot = NULL;
    uint32_t directory_block = 0;
    int slot_idx = -1;

    for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS && free_slot == NULL; i++) {
        // CP3 FIX: Block 0 is always considered allocated for Root Directory
        if (i > 0 && root_inode->block[i] == 0) {
            continue;
        }      
        void* dir_block_data;
        check = cache_get_block(cash, (root_inode->block[i] + ktfs_first_data_block) * CACHE_BLKSZ, &dir_block_data);
        if (check != 0) {
            continue;
        }  
        struct ktfs_dir_entry* entries = dir_block_data;
        for (int j = 0; j < ENTRIES_PER_DIR_BLK; j++) {
            if (entries[j].inode == 0) {
                free_slot = &entries[j];
                directory_block = root_inode->block[i] + ktfs_first_data_block;
                slot_idx = j;
                break;
            }
        }
        if (free_slot == NULL) {
            cache_release_block(cash, dir_block_data, 0);
        }
    }
    if (free_slot == NULL) {
        for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++) {
            // CP3 FIX: Do NOT allocate Block 0, it exists.
            if (i > 0 && root_inode->block[i] == 0) {
                uint32_t new_dir_blk;
                int aerr = ktfs_alloc_block(&new_dir_blk);
                if (aerr != 0) {
                    cache_release_block(cash, inode0_data, 0);
                    return aerr;
                }
                directory_block = new_dir_blk;
                root_inode->block[i] = directory_block - ktfs_first_data_block;               
                void* new_dir_block;
                check = cache_get_block(cash, directory_block * CACHE_BLKSZ, &new_dir_block);
                if (check == 0) {
                    memset(new_dir_block, 0, CACHE_BLKSZ);
                    slot_idx = 0;
                    cache_release_block(cash, new_dir_block, 1);
                }
                break;
            }
        }
    }
    
    if (slot_idx == -1) {
        cache_release_block(cash, inode0_data, 0);
        return -ENODATABLKS;
    }
    uint16_t new_inode_num = 0;
    int ierr = ktfs_alloc_inode(&new_inode_num);
    if (ierr != 0) {
        cache_release_block(cash, inode0_data, 0);
        return ierr;
    }
    uint32_t inode_block_idx = new_inode_num / INODES_PER_BLK;
    uint32_t inode_offset = new_inode_num % INODES_PER_BLK;
    void* inode_block_data;
    check = cache_get_block(cash, (inode0_block + inode_block_idx) * CACHE_BLKSZ, &inode_block_data);
    if (check != 0) {
        cache_release_block(cash, inode0_data, 0);
        ktfs_free_inode(new_inode_num);
        return check;
    }
    struct ktfs_inode* inode = (struct ktfs_inode*)((uint8_t*)inode_block_data + (inode_offset * KTFS_INOSZ));
    memset(inode, 0, sizeof(struct ktfs_inode));
    inode->size = 0;
    cache_release_block(cash, inode_block_data, 1);
    void* dir_block_data;
    check = cache_get_block(cash, directory_block * CACHE_BLKSZ, &dir_block_data);
    if (check != 0) {
        cache_release_block(cash, inode0_data, 0);
        return check;
    }
    struct ktfs_dir_entry* entries = dir_block_data;
    strncpy(entries[slot_idx].name, name, sizeof(entries[slot_idx].name) - 1);
    entries[slot_idx].name[sizeof(entries[slot_idx].name) - 1] = '\0';
    entries[slot_idx].inode = new_inode_num; 
    cache_release_block(cash, dir_block_data, 1);
    // CP3 FIX: Set size to include all allocated directory blocks.
    // Assuming Block 0 is always present for root.
    uint32_t last_blk = 0;
    for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++) {
        if (root_inode->block[i] != 0) {
            last_blk = i;
        }
    }
    root_inode->size = (last_blk + 1) * KTFS_BLKSZ;
    cache_release_block(cash, inode0_data, 1);   
    return 0;
}

/**
 * @brief Deletes a certain file from the file system with the given name
 * @param fs The file system to delete the file from
 * @param name The name of the file to be deleted
 * @return 0 if successful, negative error code if error
 */
int ktfs_delete(struct filesystem* fs, const char* name) {
    struct node* temp = open_fs_head;
    while (temp != NULL) {
        if (strcmp(temp->file->name, name) == 0) {
            return -EBUSY;
        }
        temp = temp->next;
    }
    
    uint32_t k = superblock->inode_bitmap_block_count;
    uint32_t b = superblock->bitmap_block_count;
    uint32_t inode0_block = 1 + k + b;
    
    void* inode0_data;
    int check = cache_get_block(cash, inode0_block * CACHE_BLKSZ, &inode0_data);
    if (check != 0){
        return -ENOTSUP;
    }
    
    struct ktfs_inode *root_inode = inode0_data;
    struct ktfs_dir_entry* target_entry = NULL;
    uint32_t directory_block = 0;
    int entry_index = -1;
    int block_index = -1;
    for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS && target_entry == NULL; i++) {
        if (root_inode->block[i] == 0)
            continue;
        
        void* dir_block_data;
        check = cache_get_block(cash, (root_inode->block[i] + ktfs_first_data_block) * CACHE_BLKSZ, &dir_block_data);
        if (check != 0) {
            continue;
        }       
        struct ktfs_dir_entry* entries = dir_block_data;
        for (int j = 0; j < ENTRIES_PER_DIR_BLK; j++) {
            if (entries[j].inode != 0 && strcmp(entries[j].name, name) == 0) {
                target_entry = &entries[j];
                directory_block = root_inode->block[i] + ktfs_first_data_block;
                entry_index = j;
                block_index = i;
                break;
            }
        }       
        if (target_entry == NULL) {
            cache_release_block(cash, dir_block_data, 0);
        }
    }   
    if (target_entry == NULL) {
        cache_release_block(cash, inode0_data, 0);
        return -ENOENT;
    }   
    uint16_t inode_to_free = target_entry->inode;
    uint32_t target_inode_block = inode0_block + (inode_to_free / INODES_PER_BLK);
    uint32_t target_inode_offset = inode_to_free % INODES_PER_BLK;   
    void* target_inode_data;
    check = cache_get_block(cash, target_inode_block * CACHE_BLKSZ, &target_inode_data);
    if (check != 0) {
        cache_release_block(cash, inode0_data, 0);
        return -ENOTSUP;
    }
    
    struct ktfs_inode* target_inode = (struct ktfs_inode*)((uint8_t*)target_inode_data + (target_inode_offset * KTFS_INOSZ));
    for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++) {
        if (target_inode->block[i] != 0) {
            ktfs_free_block(target_inode->block[i]);
            target_inode->block[i] = 0;
        }
    }
    if (target_inode->indirect != 0) {
        void *ind_data;
        if (cache_get_block(cash, target_inode->indirect * CACHE_BLKSZ, &ind_data) == 0) {
            uint32_t *ind_blocks = ind_data;
            for (int i = 0; i < ENTRIES_PER_INDIR_BLK; i++) {
                if (ind_blocks[i] != 0) {
                    ktfs_free_block(ind_blocks[i]);
                    ind_blocks[i] = 0;
                }
            }
            cache_release_block(cash, ind_data, 1);
        }
        ktfs_free_block(target_inode->indirect);
        target_inode->indirect = 0;
    }
    for (int d = 0; d < KTFS_NUM_DINDIRECT_BLOCKS; d++) {
        if (target_inode->dindirect[d] == 0) continue;
        void *dind_data;
        if (cache_get_block(cash, target_inode->dindirect[d] * CACHE_BLKSZ, &dind_data) != 0) {
            continue;
        }
        uint32_t *dind_blocks = dind_data;
        for (int i = 0; i < ENTRIES_PER_DINDIR_BLK; i++) {
            if (dind_blocks[i] == 0) continue;
            void *ind_data;
            if (cache_get_block(cash, dind_blocks[i] * CACHE_BLKSZ, &ind_data) != 0) continue;
            uint32_t *ind_blocks = ind_data;
            for (int j = 0; j < ENTRIES_PER_INDIR_BLK; j++) {
                if (ind_blocks[j] != 0) {
                    ktfs_free_block(ind_blocks[j]);
                    ind_blocks[j] = 0;
                }
            }
            cache_release_block(cash, ind_data, 1);
            ktfs_free_block(dind_blocks[i]);
            dind_blocks[i] = 0;
        }
        cache_release_block(cash, dind_data, 1);
        ktfs_free_block(target_inode->dindirect[d]);
        target_inode->dindirect[d] = 0;
    }
    target_inode->size = 0;
    cache_release_block(cash, target_inode_data, 1);
    ktfs_free_inode(inode_to_free);
    void* dir_block_data;
    check = cache_get_block(cash, directory_block * CACHE_BLKSZ, &dir_block_data);
    if (check != 0) {
        cache_release_block(cash, inode0_data, 0);
        return -ENOTSUP;
    } 
    struct ktfs_dir_entry* entries = dir_block_data;
    for (int i = entry_index; i < ENTRIES_PER_DIR_BLK - 1; i++) {
        entries[i] = entries[i + 1];
    }
    memset(&entries[ENTRIES_PER_DIR_BLK - 1], 0, sizeof(struct ktfs_dir_entry));
    int block_empty = 1;
    for (int i = 0; i < ENTRIES_PER_DIR_BLK; i++) {
        if (entries[i].inode != 0) {
            block_empty = 0;
            break;
        }
    }
    if (block_empty && block_index >= 0) {
        root_inode->block[block_index] = 0;
    }
    
    cache_release_block(cash, dir_block_data, 1);
    uint32_t last_blk = 0;
    for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++) {
        if (root_inode->block[i] != 0) {
            last_blk = i;
        }
    }
    root_inode->size = (last_blk + 1) * KTFS_BLKSZ;
    cache_release_block(cash, inode0_data, 1);
    
    return 0;
}

/**
 * @brief Given a file io object, a specific command, and possibly some arguments, execute the
 * corresponding functions
 * @details Any commands such as (FCNTL_GETEND, FCNTL_GETPOS, ...) should pass back through the arg
 * variable. Do not directly return the value.
 * @details FCNTL_GETEND should pass back the size of the file in bytes through the arg variable.
 * @details FCNTL_SETEND should set the size of the file to the value passed in through arg.
 * @details FCNTL_GETPOS should pass back the current position of the file pointer in bytes through
 * the arg variable.
 * @details FCNTL_SETPOS should set the current position of the file pointer to the value passed in
 * through arg.
 * @param uio the uio object of the file to perform the control function
 * @param cmd the operation to execute. KTFS should support FCNTL_GETEND, FCNTL_SETEND (CP2), 
 * FCNTL_GETPOS, FCNTL_SETPOS.
 * @param arg the argument to pass in, may be different for different control functions
 * @return 0 if successful, negative error code if error
 */
int ktfs_cntl(struct uio* uio, int cmd, void* arg) {
    struct node *cur = get_uio(uio);

    if (cur == NULL) {
        return -ENOTSUP;
    }
    switch (cmd) {
    case FCNTL_GETEND:
        *(long*)arg = cur->file->size; 
        return 0;
    
    case FCNTL_GETPOS:
        *(long*)arg = cur->file->pos; 
        return 0;
    
    case FCNTL_SETPOS:
        cur->file->pos = *(long*)arg;
        return 0;
    
    case FCNTL_SETEND:
        return ktfs_setend(cur, cmd, *(long*)arg);

    default:
        return -ENOTSUP;
    }
}

/**
 * @brief Helper function to handle command FCNTL_SETEND
 * @return 0 if successful, negative error code if error
 */
int ktfs_setend(struct node* cur, int cmd __attribute__((unused)), long new_size) {
    if (new_size < 0) {
        return -EINVAL;
    }

    struct ktfs_file *file = cur->file;
    uint32_t k = superblock->inode_bitmap_block_count;
    uint32_t b = superblock->bitmap_block_count;
    uint32_t inode0_block = 1 + k + b;

    // Load inode
    void *inode_data;
    int check = cache_get_block(cash, (inode0_block + (file->dir->inode / INODES_PER_BLK)) * CACHE_BLKSZ, &inode_data);
    if (check != 0) {
        return -ENOTSUP;
    }
    uint32_t inode_offset = file->dir->inode % INODES_PER_BLK;
    struct ktfs_inode *file_inode = (struct ktfs_inode *)((uint8_t*) inode_data + (inode_offset * KTFS_INOSZ));

    long old_size = file->size;
    long old_blocks = (old_size + CACHE_BLKSZ - 1) / CACHE_BLKSZ;
    long new_blocks = (new_size + CACHE_BLKSZ - 1) / CACHE_BLKSZ;

    // Shrink: free blocks above new end
    if (new_blocks < old_blocks) {
        for (long i = old_blocks - 1; i >= new_blocks; i--) {
            ktfs_free_logical_block(file_inode, i);
        }
    }

    // Expand: allocate new blocks as needed
    if (new_blocks > old_blocks) {
        for (long i = old_blocks; i < new_blocks; i++) {
            uint32_t blkno = 0;
            int res = ktfs_get_block(file_inode, i, KTFS_ALLOCATE, &blkno);
            if (res != 0) {
                cache_release_block(cash, inode_data, 1);
                return res;
            }
        }
    }

    // Update sizes and clamp file position
    file_inode->size = new_size;
    file->size = new_size;
    if (file->pos > new_size) {
        file->pos = new_size;
    }

    cache_release_block(cash, inode_data, 1);
    return 0;
}

/**
 * @brief Flushes the cache to the backing device
 * @return 0 if flush successful, negative error code if error
 */
void ktfs_flush(struct filesystem* fs) {
    cache_flush(cash);
    return;
}

/**
 * @brief Closes the listing device represented by the uio pointer
 * @param uio The uio pointer of ls
 * @return None
 */
void ktfs_listing_close(struct uio* uio) {
    // Handled in ktfs_close
    return;
}

/**
 * @brief Reads all of the files names in the file system using ls and copies them into the
 * providied buffer
 * @param uio The uio pointer of ls
 * @param buf The buffer to copy the file names to
 * @param bufsz The size of the buffer
 * @return The size written to the buffer
 */
long ktfs_listing_read(struct uio* uio, void* buf, unsigned long bufsz) {
    if (bufsz == 0) {
        return 0;
    }
    
    // get listing state
    struct ktfs_listing *listing = get_listing(uio);
    if (listing == NULL || listing->eof) {
        return 0;  // no more entries / no entry
    }
    
    char *output = (char *)buf;
    unsigned long written = 0;
    
    while (written < bufsz && !listing->eof) {
        // visit all dir blocks to find next valid entry
        if (listing->current_dir_entry == NULL) {
            while (listing->current_dir_block_idx < listing->num_dir_blocks) {
                uint32_t dir_block = listing->dir_blocks[listing->current_dir_block_idx];
                
                void* dir_block_data;
                int check = cache_get_block(cash, dir_block * CACHE_BLKSZ, &dir_block_data);
                if (check != 0) {
                    // skip if failed to get block
                    listing->current_dir_block_idx++;
                    listing->current_entry_idx = 0;
                    continue;
                }
                
                struct ktfs_dir_entry *entries = dir_block_data;
                
                // search for next valid entry in current dir block
                while (listing->current_entry_idx < ENTRIES_PER_DIR_BLK) {
                    if (entries[listing->current_entry_idx].inode != 0) {
                        listing->current_dir_entry = &entries[listing->current_entry_idx];
                        listing->current_entry_idx++;
                        break;
                    }
                    listing->current_entry_idx++;
                }
                
                cache_release_block(cash, dir_block_data, 0);
                
                if (listing->current_dir_entry != NULL) {   //valid entry found
                    break;
                }
                
                // next dir block
                listing->current_dir_block_idx++;
                listing->current_entry_idx = 0;
            }
            
            // reached end of all dir blocks
            if (listing->current_dir_entry == NULL) {
                listing->eof = 1;
                break;
            }
        }
        
        // copy to output buffer
        if (listing->current_dir_entry != NULL) {
            const char *filename = listing->current_dir_entry->name;
            size_t filename_len = strlen(filename);
            
            // enough space?
            if (written + filename_len + 1 > bufsz) {
                break;
            }
            
            // copy filename
            strncpy(output + written, filename, filename_len);
            written += filename_len;
            
            // 'backslash n'
            output[written] = '\n';
            written++;
            
            // ready for the next one
            listing->current_dir_entry = NULL;
        }
    }
    
    return written;
}
