#define _GNU_SOURCE
#include <stdlib.h> 
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h> 
#include "ice-buf.h"


//TODO: is this really necessary on a single-core ARM? 
#define MEMORY_FENCE __sync_synchronize(); 

static size_t buffer_count = 0; 

struct ice_buf
{
  volatile size_t produced_count; 
  volatile size_t consumed_count; 
  size_t memb_size;
  size_t capacity; 
  size_t index; 
  char * name; 
  unsigned int sleep_amt; 
  char mem[]; //flexible size array 
}; 



ice_buf_t* ice_buf_init(size_t max_capacity, size_t memb_size, const char * name) 
{
  ice_buf_t * b = calloc(1, sizeof(struct ice_buf) + memb_size * max_capacity); 

  if (!b) 
  {
    fprintf(stderr,"Can't allocate buffer. Are we out of memory!?"); 
    return 0; 
  }

  b->produced_count = 0; 
  b->consumed_count = 0; 
  b->capacity = max_capacity; 
  b->memb_size = memb_size; 
  b->index = buffer_count++; 
  if (name)
  {
    b->name = strdup(name); 
  }
  else
  {
    asprintf(&b->name, "icebuf_%zd", b->index); 
  }
  return b; 
}


size_t ice_buf_capacity(const ice_buf_t *b)
{
  return b->capacity; 
}

size_t ice_buf_occupancy(const ice_buf_t *b)
{
  return b->produced_count - b->consumed_count; 
}

void ice_buf_set_sleep_amt(ice_buf_t * b, unsigned int amt) 
{
  b->sleep_amt = amt; 

}
void* ice_buf_getmem(ice_buf_t *b )
{

  int warned = 0;
  while(ice_buf_capacity(b) == ice_buf_occupancy(b))
  {
    if (!warned) 
    {
      fprintf(stderr,"WARNING: Buffer %s is full!\n", b->name); 
      warned++; 
    }
    usleep(b->sleep_amt); //sleep for 500 us to give the other thread a chance; 
  }

  return (b->mem)  + b->memb_size * (b->produced_count % b->capacity); 
}

void ice_buf_commit(ice_buf_t * b) 
{
  MEMORY_FENCE
  b->produced_count++;
}

void ice_buf_push(ice_buf_t *b, const void * mem)
{
  void * ptr =  ice_buf_getmem(b); 
  memcpy(ptr,mem,b->memb_size); 
  ice_buf_commit(b); 
}

void * ice_buf_peek(ice_buf_t* b) 
{
  return b->produced_count - b->consumed_count == 0 ?  NULL 
    :  ((char*)b->mem) + b->memb_size * (b->consumed_count % b->capacity); 
}

int ice_buf_pop(ice_buf_t * b, void * dest, void ** verify)
{
  if (b->produced_count - b->consumed_count == 0)
  {
    if (verify) 
    {
      fprintf(stderr,"Was passed verification pointer 0x%p to %s but the buffer is empty. Somebody screwed up.\n", *verify, b->name); 
    }
    return 1; 
  }

  void * m = NULL; 
  if (verify || dest) 
  {
    m = ice_buf_peek(b); 
  }

  if (verify && m!=*verify) 
  {
      fprintf(stderr,"WARNING: Was passed verification pointer 0x%p to %s but does not match top of buffer 0x%p. Somebody screwed up.\n", *verify, b->name,m); 
  }
 
  if (dest) 
  {
     memcpy(dest, m , b->memb_size); 
  }

  MEMORY_FENCE
  b->consumed_count++; 
  if (verify && *verify == m) 
  {
    *verify = NULL; 
  }
  return 0; 
}


int ice_buf_destroy(ice_buf_t *b) 
{
  int occupancy = ice_buf_occupancy(b); 
  free(b->name);
  free(b); 
  return occupancy; 
}




