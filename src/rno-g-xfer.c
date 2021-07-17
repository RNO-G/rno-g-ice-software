#include "rno-g.h"  
#include "sqlite3.h" 




static pthread_t copy_thread; 
static pthread_t collect_thread;  

static acq_config_t acq_cfg; 
static xfer_config_t xfer_cfg; 

static pthread_rwlock_t cfg_lock; 
static pthread_rwlock_t db_lock; 

static volatile quit = 0; 

static void please_quit() 
{
  quit = 1; 
}


static void * copy_proc() 
{
  sqlite

  while(!quit)
  {





  }
  


  return 0; 
}


static void setup() 
{

  pthread_rwlock_init(&cfg_lock,NULL); 
}


static void teardown() 
{
  pthread_join(copy_thread,0); 
  pthread_join(collect_thread,0); 


}

static void read_config() 
{
  static int config_counter; 
  int first_time = !config_counter; 

  pthread_rwlock_wrlock(&cfg_lock); 

  acq_config_t old_acq_cfg; 
  xfer_config_t old_xfer_cfg; 

  if(first_time) 
  {
    init_acq_config(&acq_cfg); 
    init_xfer_config(&xfer_cfg); 
  }
  else
  {
    printf("Rereading configs.."); 
    memcpy(&old_acq_cfg, &acq_cfg);
    memcpy(&old_xfer_cfg, &xfer_cfg);
  }

  FILE * facq = find_config("acq.cfg"); ; 
  if (!facq || read_acq_config(facq,&acq_cfg))
  {
    fprintf(stderr,"Errors reading acq config\n"); 
  }
  fclose(facq); 

  FILE * fxfer = find_config("xfer.cfg"); ; 
  if (!fxfer || read_xfer_config(facq,&xfer_cfg))
  {
    fprintf(stderr,"Errors reading xfer config\n"); 
  }
  fclose(fxfer); 

  config_counter++; 

  pthread_rwlock_unlock(&cfg_lock); 

}


int main(int nargs, char ** args) 
{

  if (setup())
  {
    return 1; 
  }

  struct timespec start_time; 
  clock_gettime(CLOCK_MONOTONIC_COARSE,&start_time); 

  while (!quit) 
  {
     if (cfg_reread) 
     {
       cfg_reread = 0; 
       read_config(); 
     }
     sleep(1); 
  }

  return teardown(); 

}
