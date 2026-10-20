/*! @file cache.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌‍‍‍‌‌‍‌​⁠⁠‌
    @brief Block cache for a storage device.
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef CACHE_TRACE
#define TRACE
#endif

#ifdef CACHE_DEBUG
#define DEBUG
#endif


#ifndef NUM_BLOCKS
#define NUM_BLOCKS 64
#endif


#include "cache.h"

#include "conf.h"
#include "console.h"
#include "device.h"
#include "devimpl.h"
#include "error.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "string.h"
#include "thread.h"


// INTERNAL TYPE DEFINITIONS
//


struct cache_block{
    uint64_t time_block;  //the last time it is accessed
    char dirty;   // 1 when block is written, 0 when it's not written
    unsigned long long pos;  //Position in the backing storage device.
    uint64_t count;  // number of threads occupying the block
    void* cache_block_data;  //pointer to the place hold data
    char empty;  //1 means empty

};


struct cache
{   
    struct storage* disk;
    struct cache_block cache_array[NUM_BLOCKS];  // an array of pointers to cache blocks
    struct lock lock_cache;
    uint64_t time;
};


// static allocation of the cache
static struct cache my_cache;





/**
 * @brief helper function to read block from disk to an empty block in cache
 * @param cache Pointer to the cache
 * @param pos Position in the backing storage device.
 * @param slot index of the empty cache_block
 * @return number of bytes read, error code if error
 */
static int get_helper(struct cache* cache, unsigned long long pos, int slot ){
   
    long num = storage_fetch(cache->disk, pos, cache->cache_array[slot].cache_block_data, CACHE_BLKSZ );

    if(num == CACHE_BLKSZ){

        // return successfully
        cache->cache_array[slot].empty = 0;    //cache_block is not empty    
        cache->cache_array[slot].pos = pos;
        cache->cache_array[slot].count = 1;        
        cache->cache_array[slot].dirty = 0;

        cache->time++; //the cache_block is used, record the last time it is accessed
        cache->cache_array[slot].time_block = cache->time;

        return 0;
    }else{

        // if storage_fetch returns error, then reads fail
        // we don't read data to cache_block successfully
        cache->cache_array[slot].empty = 1;  //indicate this cache_block is still empty
        cache->cache_array[slot].count = 0;   
        return num;
    }
}



/**
 * @brief helper function to find which block to evict using LRU
 * @param cache Pointer to the cache
 * @return the cache_block that will be evicted from cache
 */
struct cache_block* cache_evict(struct cache* cache){
    struct cache_block* best = NULL;  //the least recently used block
    struct cache_block*  tmp = NULL;  // temporary block used to compare
    unsigned long long LRU = ~0ULL;  //the least recent time

    // loop through every cache_block
    for(int i = 0; i < NUM_BLOCKS; ++i){
        tmp = &cache->cache_array[i];

        // if the cache_block is not empty and is not occupied by a thread
        if(tmp-> empty == 0 && tmp->count == 0){
            if( tmp->time_block < LRU){
                best = tmp;
                LRU = tmp->time_block;
            }
        }
    }

    return best;
}



/**
 * @brief Creates/initializes a cache with the passed backing storage device (disk) and makes it
 * available through cptr.
 * @param disk Pointer to the backing storage device.
 * @param cptr Pointer to the cache to create.
 * @return 0 on success, negative error code if error
 */
int create_cache(struct storage* disk, struct cache** cptr) {
    // FIXME
    if (disk == NULL || cptr == NULL) return -EINVAL;  
    struct cache *cache = &my_cache;
    memset(cache, 0, sizeof(*cache));

    cache->disk = disk;
    cache->time = 0;
    lock_init(&cache->lock_cache);

    for(int i = 0; i < NUM_BLOCKS; i++){
        // allocate the struct itself 
        struct cache_block* tmp_block = &cache->cache_array[i];
        
        tmp_block->cache_block_data = kmalloc(CACHE_BLKSZ);
        if (tmp_block->cache_block_data == NULL) {

            return -ENOMEM;
        }

        tmp_block->empty = 1;     //empty initially, unsued
        tmp_block->dirty = 0;
        tmp_block->count = 0;
        tmp_block->pos = 0;
        tmp_block->time_block = 0;
    }

    *cptr = cache;
    return 0;
}

/**
 * @brief Reads a CACHE_BLKSZ sized block from the backing interface into the cache.
 * @param cache Pointer to the cache.
 * @param pos Position in the backing storage device. Must be aligned to a multiple of the block
 * size of the backing interface.
 * @param pptr Pointer to the block pointer read from the cache. Assume that CACHE_BLKSZ will always
 * be equal to the block size of the storage disk. Any replacement policy is permitted, as long as
 * your design meets the above specifications.
 * @return 0 on success, negative error code if error
 */
int cache_get_block(struct cache* cache, unsigned long long pos, void** pptr) {
    // FIXME
    if (pos % CACHE_BLKSZ != 0) return -EINVAL;  //Must be aligned to a multiple of the block* size of the backing interface.

    if(cache == NULL ||pptr == NULL ) return -EINVAL;

    lock_acquire(&cache->lock_cache);

    
    // loop over every cache_blocks
    struct cache_block* target_ptr=NULL;
    char found = 0;
    for(int i = 0; i< NUM_BLOCKS; ++i){

        // skip the cache_block if it dose not exist
        if(cache->cache_array[i].empty== 1) continue;

        // if the block you will reach is right in the cache
        if(cache->cache_array[i].pos == pos ){
            target_ptr = &cache->cache_array[i];
            found = 1;
            break;
        }
    }

    //cache hit
    if(found == 1){

        //record this time you access the block
        target_ptr -> count++;
        cache->time++;
        target_ptr->time_block = cache->time;

        *pptr = target_ptr->cache_block_data;

        lock_release(&cache->lock_cache);

        return 0; //success
    }

    //cache miss
    //whether there is an empty cache_block to use
    int slot = -1;
    for(int j = 0; j < NUM_BLOCKS; ++j){


        if(cache->cache_array[j].empty == 1){
            slot = j;
            break;
        }
    }

    // there is an empty cache_block
    if(slot != -1){
        int ret = get_helper(cache, pos, slot);

        //fail to read the block from disk to cache_block
        if(ret<0){
            lock_release(&cache->lock_cache);
            return ret;

        }

        //read the block from disk to an empty slot in cache successfully
        *pptr = cache->cache_array[slot].cache_block_data;
        lock_release(&cache->lock_cache);
        return 0;  // success
    }

    //there is not an empty cache_block;
    //evict

    struct cache_block* evicted_slot = cache_evict(cache);

    if(evicted_slot == NULL){
        lock_release(&cache->lock_cache);
        return -EBUSY;
    }



    // evicted != NULL, there is some cache_block that is evicted

    //write back if dirty
    if(evicted_slot->dirty){

        //write back
        long bytes_stored = storage_store(cache->disk, evicted_slot->pos, evicted_slot->cache_block_data, CACHE_BLKSZ);

        if(bytes_stored != CACHE_BLKSZ){
            // did no write back successfully
            lock_release(&cache->lock_cache);
            return bytes_stored;
        }
        else{

            // write back successfullt
            evicted_slot->dirty = 0;  //written back, so it's no dirty
        }

    }

    // find the idx of the evicted slot
    int evicted_idx = -1;
    for(int k = 0; k < NUM_BLOCKS; ++k){
        if(&cache->cache_array[k] == evicted_slot){
            evicted_idx = k;
            break;
        }
    }

    if(evicted_idx == -1){
        lock_release(&cache->lock_cache);
        return -EIO;  
    }
    
    // read block from disk to the empty slot in cache(just evicted)
    int ret2 = get_helper(cache, pos, evicted_idx);
    if(ret2 < 0){
        lock_release(&cache->lock_cache);
        return ret2;
    }


    // actual data
    *pptr = cache->cache_array[evicted_idx].cache_block_data;
    

    lock_release(&cache->lock_cache);
    return 0;
}





/**
 * @brief Releases a block previously obtained from cache_get_block().
 * @param cache Pointer to the cache.
 * @param pblk Pointer to a block that was made available in cache_get_block() (which means that
 * pblk == *pptr for some pptr).
 * @param dirty Indicates whether the block has been modified (1) or not (0). If dirty == 1, the
 * block has been written to. If dirty == 0, the block has not been written to.
 * @return 0 on success, negative error code if error
 */
void cache_release_block(struct cache* cache, void* pblk, int dirty) {
    // FIXME


    // return if parameter is NULL
    if(cache == NULL || pblk == NULL ) return;


    lock_acquire(&cache->lock_cache);

    // find the block to release
    struct cache_block* tmp = NULL;
    for(int i = 0; i < NUM_BLOCKS; ++i){
        if(cache->cache_array[i].cache_block_data == pblk){
            tmp = &cache->cache_array[i];
            break;
        }
    }

    // fail to find the block to release
    if(tmp == NULL){
        lock_release(&cache->lock_cache);
        return;
    }


    // record if the block is dirty(written)
    if(dirty){
        tmp->dirty = 1;

    }


    // release, so the thread is not holding the block_cache, reduce the count
    if(tmp->count > 0) tmp->count --;
    lock_release(&cache->lock_cache);
    return;
}

/**
 * @brief Flushes the cache to the backing device
 * @param cache Pointer to the cache to flush
 * @return 0 on success, error code if error
 */
int cache_flush(struct cache* cache) {
    // FIXME

    //if input is not valid
    if(cache == NULL) return -EINVAL;

    lock_acquire(&cache->lock_cache);

    for(int i = 0; i < NUM_BLOCKS; i++){
        struct cache_block *tmp = &cache->cache_array[i];

        //skip if cache block does no exist
        if(tmp == NULL) continue;


        // flush when the cache_block is not empty, not occupied and dirty
        if(tmp->empty == 0 && tmp->dirty && tmp->count == 0){
            long bytes_stored = storage_store(cache->disk, tmp->pos, tmp->cache_block_data, CACHE_BLKSZ);

            //fail
            if(bytes_stored != CACHE_BLKSZ){
                lock_release(&cache->lock_cache);
                return bytes_stored;
            }

            //flush success
            tmp->dirty = 0;
        }
    }

    lock_release(&cache->lock_cache);
    return 0;
}



















