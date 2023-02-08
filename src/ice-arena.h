#ifndef _RNO_G_ICE_ARENA_H
#define _RNO_G_ICE_ARENA_H

/** 
 * Thread-Safe Arena 
 *
 * We use this to share memory between the acq and proc threads so we don't have to copy stuff around
 *
 */ 

struct ice_arena; 
typedef struct ice_arena ice_arena_t; 


// initialize a new arena with space for at least nmemb, each of which is membsize. 
// Note that this will be rounded up to the next multiple of 64
ice_arena_t * ice_arena_init(size_t nmemb, size_t membsize, const char * name); 

// grab a chunk of memory from the arena (will be membsize big). This will block if no memory is available. 
void * ice_arena_getmem(ice_arena_t * arena); 

int ice_arena_clear(ice_arena_t * arena, void * item); 
size_t ice_arena_capacity(ice_arena_t * arena); 
size_t ice_arena_occupancy(ice_arena_t * arena); 


#endif
