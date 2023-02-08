#define _GNU_SOURCE
#include <stdlib.h> 
#include <stdio.h>
#include <stdint.h> 
#include "ice-buf.h"
#include <pthread.h>
#include <string.h>
#include <semaphore.h> 
#include <assert.h> 

static size_t arena_count = 0; 

typedef struct ice_arena
{
  size_t nmemb; 
  size_t membsize; 
  size_t nallocated; 
  size_t nfreed; 
  size_t free_map_size; 
  size_t index; 
  char * name; 
  char * mem; 
  uint64_t * free_map;  // bit set = free 
  pthread_mutex_t lock; 
  sem_t sem; 
} ice_arena_t; 



size_t ice_arena_capacity(const ice_arena_t *b)
{
  return b->nmemb; 
}

size_t ice_arena_occupancy(const ice_arena_t *b)
{
  return b->nallocated - b->nfreed; 
}


ice_arena_t * ice_arena_init (size_t nmemb, size_t membsize, const char * name)  
{
  //round up to next multiple of 64 

  nmemb = (nmemb + 63) & ~63; 
  
  
  //first try allocating the backing storage, since that's the biggest thing 
  void * mem = calloc(nmemb,membsize); 
  if (!mem) 
  {
    fprintf(stderr,"Couldn't allocate backing storage for arena\n"); 
    return NULL; 
  }

  size_t free_map_size = nmemb >> 6; 
  void * free_map = malloc(free_map_size*sizeof(uint64_t)); 

  if (!free_map) 
  {
    fprintf(stderr,"Couldn't allocate backing storage for free map\n"); 
    free(mem); 
    return NULL; 
  }
  
  //set to all free 
  memset(free_map, 0xff, free_map_size * sizeof(uint64_t)); 


  ice_arena_t * arena = calloc(1, sizeof(ice_arena_t)); 
  if (!arena) 
  {
    fprintf(stderr, "Couldn't allocate arena\n"); 
    free(mem); 
    free(free_map); 
    return NULL; 
  }

  arena->nmemb = nmemb; 
  arena->membsize = membsize; 
  arena->nallocated = 0; 
  arena->nfreed = 0; 
  arena->free_map_size = free_map_size; 
  arena->index = arena_count++; 
  if (name) arena->name = strdup(name) ; 
  else asprintf(&arena->name, "arena_%zud\n", arena->index); 
  arena->mem = mem; 
  arena->free_map = free_map; 
  pthread_mutex_init(&arena->lock, 0); 
  sem_init(&arena->sem,0, arena->nmemb); 

  return arena; 
}

void * ice_arena_getmem(ice_arena_t * arena) 
{
  //block until we have free memory 
  sem_wait(&arena->sem); 

  // make sure we're the only ones touching the arena
  pthread_mutex_lock(&arena->lock); 

  //make sure we don't have a bad bug 
  assert(ice_arena_occupancy(arena) < ice_arena_capacity(arena)); 

  //let's find a good chunk 
  ssize_t idx = -1; 
  for (size_t i = 0; i < arena->free_map_size; i++) 
  {
    if (arena->free_map[i]) //woohoo, we found something! 
    {
      int ctz = __builtin_ctzll(arena->free_map[i]); 
      idx = ctz + i * 64; 
      arena->free_map[i] &= ~(1 <<ctz);
    }
    //we should never get here on the last iteration
  }

  assert (idx > -1); 
  void * mem = arena->mem + arena->membsize * idx; 
  arena->nallocated++; 

  pthread_mutex_unlock(&arena->lock); 
 
  return mem; 
}

int ice_arena_clear(ice_arena_t * arena,  void * item) 
{
  //do a validity check first by checking if the address lines up 
  char * p = (char*) item; 
  size_t idx = (p-arena->mem)/arena->membsize; 
  if (p < arena->mem || idx >= arena->nmemb || p!= arena->mem + idx * arena->membsize) 
  {
    fprintf(stderr,"Uh oh, pointer 0x%p does not point to a member in arena %s, not clearing.\n", p, arena->name); 
    return 1; 
  }


  pthread_mutex_lock(&arena->lock); 
  
  //now check to see that it's not double freed? 

  int ret = 0; 
  if ( (arena->free_map[idx >> 6] & (1 << (idx & 0x3f))))
  {
    fprintf(stderr,"Are we double freeing 0x%p in arena %s?\n", p, arena->name); 
    ret =2;
  }
  else 
  {
    arena->nfreed++;
    arena->free_map[idx >> 6] |= ( 1 << (idx & 0x3f)); 
    // we made room! 
    sem_post(&arena->sem); 
  }

  pthread_mutex_unlock(&arena->lock); 

  return ret; 
}
