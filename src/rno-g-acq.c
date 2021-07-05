/** 
 *
 *  The main RNO-G Acquisition daemon. 
 *
 *  Cosmin Deaconu <cozzyd@kicp.uchicago.edu>  2021 
 *
 *  This is a multi-threaded design, with the following responsibilities:
 *
 *   - main thread:  sets things up, listens for signals, terminates. 
 *   - acq  thread:  records data from the digitizer boards and puts in the write queue 
 *   - out  thread:  processes things from the write queue, eventually writing them out
 *   - mon thread:   monitors the scalers and adjusts thresholds 
 *
 *    The BBB is single-threaded, so in practice only one thread is happening at once anyway, 
 *    but this design is simpler to understand. 
 *
 *    A SIGUSR1 will cause the main thread to reread the configuration, potentially changing
 *    various things. Not all things take effect on such an update (e.g. output_dir or runfile) .
 *
 *
 *    Because of the multiple threads, we need to be careful about locking. 
 *
 *    There are a few rwlocks: 
 *
 *      cfg_lock: Locks the global acq configuration 
   *       * readers hold this when using the config for something, but should release sometimes
 *         * will be held as a write lock when th econfig is being updated (from a signal telling us to reread) 
 *
 *      radiant_lock:  
 *          * the acq thread and the mon thread are readers for this. The  acq thread only uses SPI and the mon thread only uses UART so it should be fine. 
 *          * the write lock must be held when configuring the radiant (e.g. the trigger options, 
 *             not just the thresholds changed). In practice this may be completely redundant with the cfg lock 
 *             and be removed. 
 *      
 *    
 */ 


///// INCLUDES ////// 
#define _GNU_SOURCE
#include <stdio.h> 
#include <stdlib.h>
#include <fcntl.h> 
#include <pthread.h>
#include <string.h>
#include <signal.h> 
#include <unistd.h> 
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <zlib.h>
#include <inttypes.h>
#include <math.h> 


#include "radiant.h" 
#include "flower.h" 
#include "rno-g.h" 
#include "ice-config.h" 
#include "ice-buf.h"
#include "ice-common.h"

/////// TYPES //////////

/* An item in the acq buffer */ 
typedef struct acq_buffer_item
{
  rno_g_waveform_t wf; 
  rno_g_header_t hd; 
} acq_buffer_item_t; 


typedef struct mon_buffer_item 
{
  rno_g_daqstatus_t ds; 
} mon_buffer_item_t; 


///// GLOBALS ////// /

/** This is the acq config, globally shared. It should only be modified by the MAIN thread. 
 *
 *  It is initialized at startup, and potentially can be modified on the receipt of some signals. 
 *
 **/ 
static acq_config_t cfg; 

/*read-write lock for the config */ 
static pthread_rwlock_t cfg_lock; 

/*read-write lock for cofiguring the radiant */ 
static pthread_rwlock_t radiant_lock; 

/*read-write lock for cofiguring the flower */ 
static pthread_rwlock_t flower_lock; 

/**read-write lock for the daq status */ 
static pthread_rwlock_t ds_lock; 

static pthread_t the_acq_thread; 
static pthread_t the_mon_thread; 
static pthread_t the_wri_thread; 

/** This counts how many times the config has been read */ 
static volatile int config_counter;  

/** This is the current run number */ 
static int run_number = -1; 

/** This is the station number */ 
static int station_number = -1; 

/** The output directories */ 
static char * output_dir = NULL; 


/** This is set to 1 when it's time to quit.*/ 
static volatile int quit = 0; 
static volatile int cfg_reread = 0; 


/** radiant handle*/ 
static radiant_dev_t * radiant = 0; 

/** flower handle */ 
static flower_dev_t * flower = 0; 


/** radiant pedestals*/ 
static rno_g_pedestal_t * pedestals = 0; 

//File descriptor for pedestal shared mem file 
static int pedestal_fd; 


/** Shared daq status */ 
static rno_g_daqstatus_t * ds = 0; 

//File descriptor for daqstatus shared mem file
static int shared_ds_fd; 

//acq ring buffer 
ice_buf_t *acq_buffer; 

//mon ring buffer 
ice_buf_t *mon_buffer; 

///// PROTOTYPES  /////  

static int radiant_configure();
static int flower_configure();
static int teardown(); 
static int please_stop(); 


///// Implementations /////

/** This, unsurprisingly, reads the config file.
 **  It will hold a write lock on the config .
 the config is checked in 3 places, in order: 
   cwd 
   $RNO_G_INSTALL_DIR/cfg
   /rno-g/cfg

   The first time this is called  the cfg will be default-inited before reading. 

   The config can be read multiple times during the run, as some settings can be changed. 
   A SIGUSR1 signal will force a read of the config. 
 
 */ 
static void read_config() 
{
  static int config_counter; 
  int first_time = !config_counter; 

  //Acquire a 
  pthread_rwlock_wrlock(&cfg_lock); 

  acq_config_t old_cfg; 
  if (first_time)  
  {
    init_acq_config(&cfg); 
  }
  else
  {
    printf("Rereading config..."); 
    memcpy(&old_cfg,&cfg,sizeof(cfg)); 
  }

  FILE * fptr = NULL; 

  //First try CWD
  if (!access("acq.cfg", R_OK))
  {
    fptr = fopen("acq.cfg", "r"); 
  }
  else //try $RNO_G_INSTALL_DIR/cfg/acq.cfg  or /rno-g/cfg/acq.cfg
  {

    const char * install_dir = getenv("RNO_G_INSTALL_DIR"); 
    if (install_dir)
    {
      char * fname = 0; 
      asprintf(&fname,"%s/cfg/acq.cfg", install_dir); 
      if (!access(fname,R_OK))
      {
        fptr = fopen(fname,"r"); 
        free(fname); 
      }
    }

    //still don't have it, let's try /rno-g/cfg/acq.cfg
    if (!fptr) 
    {
      if (!access("/rno-g/cfg/acq.cfg",R_OK))
      {
        fptr = fopen("/rno-g/cfg/acq.cfg","r"); 
      }
    }
  }

  if (!fptr) 
  {
    fprintf(stderr,"!!!Could not find acq.cfg in ., $RNO_G_INSTALL_DIR/cfg/ or /rno-g/cfg\n"); 
    if (first_time) 
    {
      fprintf(stderr,"!!! This means we are using the default cfg. Hopefully it works for you?\n"); 
    }
  }
  else
  {
    if (!read_acq_config(fptr, &cfg))
    {
      fprintf(stderr,"!!! Errors while reading acq config\n"); 
    }

    fclose(fptr); 
  }

  //write the updated config (can't do this first time since output_dir hasn't been made
  //by initial_setup yet) 
  if (!first_time) 
  {
    char * ofname; 
    asprintf(&ofname,"%s/cfg/acq.%d.cfg", output_dir, config_counter); 
    FILE * of = fopen(ofname,"w"); 
    dump_acq_config(of, &cfg); 
    fclose(of); 
    free(ofname); 
  }

  //incremente the config counter (so threads know config may have changed) 
  config_counter++; 
  //release the write lock 
  pthread_rwlock_unlock(&cfg_lock); 


  //apply new configuration to radiant/flower if they have changed
  if (!first_time) 
  {
    if (memcmp(&old_cfg.radiant, &cfg.radiant, sizeof(cfg.radiant)))
    {
      radiant_configure(); 
    }

    if (memcmp(&old_cfg.lt, &cfg.lt, sizeof(cfg.lt)))
    {
      flower_configure(); 
    }
  }

}


/** This configures the radiant. It holds the radiant write lock (and acquires the config read lock)*/ 
int radiant_configure() 
{

  pthread_rwlock_wrlock(&radiant_lock); 
  pthread_rwlock_rdlock(&cfg_lock);



  radiant_pps_config_t pps_cfg = {.pps_holdoff = cfg.radiant.pps.pps_holdoff,
                                  .enable_sync_out= cfg.radiant.pps.sync_out,
                                  .use_internal_pps = cfg.radiant.pps.use_internal}; 

  radiant_set_pps_config(radiant,pps_cfg); 




  radiant_set_scaler_period(radiant, cfg.radiant.scalers.use_pps ? 0 : cfg.radiant.scalers.period); 

  for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++)
  {
    radiant_set_prescaler(radiant,i, cfg.radiant.scalers.prescal_m1[i]); 
  }

  uint32_t global_mask = 
    ((!!cfg.radiant.trigger.RF[0].enabled) * cfg.radiant.trigger.RF[0].mask) |
    ((!!cfg.radiant.trigger.RF[1].enabled) * cfg.radiant.trigger.RF[1].mask);

  int ret = radiant_set_global_trigger_mask(radiant, global_mask); 

  ret += radiant_configure_rf_trigger(radiant, RADIANT_TRIG_A, 
      cfg.radiant.trigger.RF[0].enabled ? cfg.radiant.trigger.RF[0].mask  : 0, 
      cfg.radiant.trigger.RF[0].num_coincidences-1, cfg.radiant.trigger.RF[0].window); 

  ret += radiant_configure_rf_trigger(radiant, RADIANT_TRIG_B, 
      cfg.radiant.trigger.RF[1].enabled ? cfg.radiant.trigger.RF[1].mask  : 0, 
      cfg.radiant.trigger.RF[1].num_coincidences-1, cfg.radiant.trigger.RF[1].window); 

  //make sure the labs are started before setting enables 
  radiant_labs_start(radiant); 
  int enables = RADIANT_TRIG_EN; 

  if (cfg.radiant.trigger.output_enabled)
  {
    enables |= RADIANT_TRIGOUT_EN; 
  }

  if ( cfg.radiant.trigger.ext.enabled) 
  {
    enables |= RADIANT_TRIG_EXT; 
  }

  if (cfg.radiant.trigger.pps.enabled) 
  {
    enables |= RADIANT_TRIG_PPS; 
    if (cfg.radiant.trigger.pps.output_enabled) 
    {
      enables |= RADIANT_TRIGOUT_PPS; 
    }
  }

  if (cfg.radiant.trigger.soft.output_enabled) 
  {
    enables |= RADIANT_TRIGOUT_SOFT; 
  }

  radiant_trigger_enable(radiant,enables,0); 


  pthread_rwlock_unlock(&cfg_lock);
  pthread_rwlock_unlock(&radiant_lock); 
  return 0; 
}


/** this configures the flower trigger. It holds the flower write lock (and acquires the config read lock)*/ 
int flower_configure() 
{

  pthread_rwlock_wrlock(&flower_lock); 
  pthread_rwlock_rdlock(&cfg_lock); 
  rno_g_lt_simple_trigger_config_t ltcfg; 
  ltcfg.window = cfg.lt.trigger.window; 
  ltcfg.vpp_mode = cfg.lt.trigger.vpp; 
  ltcfg.num_coinc =cfg.lt.trigger.enable ?  cfg.lt.trigger.min_coincidence-1 : 4; 
  int ret = flower_configure_trigger(flower, ltcfg); 

  pthread_rwlock_unlock(&cfg_lock); 
  pthread_rwlock_unlock(&flower_lock); 

  return ret; 
}


static float clamp(float val, float min, float max) 
{
  if (val > max) return max; 
  if (val < min) return min; 
  return val; 
}
int flower_initial_setup() 
{
  flower = flower_open(cfg.lt.device.spi_device, cfg.lt.device.spi_enable_gpio); 
  if (!flower) return -1; 
  //then the rest of the configuration; 
  flower_configure(); 
  return 0; 
}


/* Initial radiant config, including potential pedestal taking 
 *
 * this happens before threads start while holding config lock. 
 * */ 
int radiant_initial_setup() 
{


   radiant  = radiant_open(cfg.radiant.device.spi_device, 
                           cfg.radiant.device.uart_device, 
                           cfg.radiant.device.poll_gpio, 
                           cfg.radiant.device.spi_enable_gpio); 

  if (!radiant) return -1; 

  //just in case 
  radiant_labs_stop(radiant); 


  int wait_for_analog_settle=0; 
  if (cfg.radiant.analog.apply_lab4_vbias) 
  {
    
    int ibias_left = cfg.radiant.analog.lab4_vbias[0] / 3.3 * 4095; 
    int ibias_right = cfg.radiant.analog.lab4_vbias[1] / 3.3 * 4095; 
    radiant_set_dc_bias(radiant,ibias_left,ibias_right); 
    wait_for_analog_settle = 1; 
  }
 
  if (cfg.radiant.analog.apply_diode_vbias) 
  {
    wait_for_analog_settle = 1; 
    for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++) 
    {
      radiant_set_td_bias(radiant, i, cfg.radiant.analog.diode_vbias[i]/2.*4095); 
    }
  }

  if (wait_for_analog_settle) 
  {
    usleep(cfg.radiant.analog.settle_time*1e6); 
  }

  int have_peds = 0; 
  if (cfg.radiant.pedestals.pedestal_file) 
  {

    pedestal_fd = open(cfg.radiant.pedestals.pedestal_file, O_CREAT | O_RDWR, 0755); 

    if (pedestal_fd == -1) 
    {
      fprintf(stderr,"Could not open %s\n", cfg.radiant.pedestals.pedestal_file); 
    }
    else
    {
      //measure size 
      size_t fsize = lseek(pedestal_fd, 0 , SEEK_END); 
      //rewind 
      lseek(pedestal_fd,0,SEEK_SET); 

      //truncate to right size if not already right
      if (fsize!= sizeof(rno_g_pedestal_t))
      {
        ftruncate(pedestal_fd, sizeof(rno_g_pedestal_t)); 
      }


      pedestals = mmap(0, sizeof(rno_g_pedestal_t), PROT_READ | PROT_WRITE, MAP_SHARED, pedestal_fd, 0); 

      //valid to read, maybe! 
      if (pedestals ==MAP_FAILED) 
      {
        //ruhroh. 
        fprintf(stderr, "Could not mmap pedestals. Will not be cached\n"); 
        munmap(pedestals, sizeof(rno_g_pedestal_t)); 
        close(pedestal_fd); 
        pedestals = 0; 
      }

      else if(fsize != sizeof(rno_g_pedestal_t)) 
      {
        memset(pedestals,0, sizeof(rno_g_pedestal_t)); 
      }
      else
      {
        have_peds = 1; 
      }
    }
  }

  if (cfg.radiant.pedestals.compute_at_start) 
  {

    if (cfg.radiant.pedestals.apply_attenuation) 
    {
       for (int ichan = 0; ichan < RNO_G_NUM_RADIANT_CHANNELS; ichan++) 
       {
         radiant_set_attenuator(radiant, ichan, RADIANT_ATTEN_SIG, cfg.radiant.pedestals.attenuation*4); 
       }
    }

    //in case we didn't get mmaped 
    if (!pedestals) 
    {
      pedestals = calloc(sizeof(rno_g_pedestal_t), 1); 
    }

    have_peds = !radiant_compute_pedestals(radiant, 0xffffff, 
                                            cfg.radiant.pedestals.ntriggers_per_computation,
                                            pedestals); 


    //if we have a pedestal file, let's flush it 
    if (cfg.radiant.pedestals.pedestal_file) 
    {
      msync(pedestals, sizeof(rno_g_pedestal_t), MS_SYNC); 
    }

    //TODO: there's no way we can restore, is there? 
    if (cfg.radiant.pedestals.apply_attenuation) 
    {
       for (int ichan = 0; ichan < RNO_G_NUM_RADIANT_CHANNELS; ichan++) 
       {
         radiant_set_attenuator(radiant, ichan, RADIANT_ATTEN_SIG, 0); 
       }
    }

  }


  if (cfg.radiant.pedestals.pedestal_subtract && !have_peds) 
  {

    fprintf(stderr,"!!! Can't subtract pedestals due to not having a good source. Either enable radiant.pedestals.compute_at_start or arrange to point radiant.pedestals.pedestal_file to valid pedestals.\n"); 
  }
  else if (cfg.radiant.pedestals.pedestal_subtract) 
  {
    radiant_set_pedestals(radiant, pedestals); 
    
  }

  if (cfg.radiant.analog.apply_attenuations) 
  {
       for (int ichan = 0; ichan < RNO_G_NUM_RADIANT_CHANNELS; ichan++) 
       {
         radiant_set_attenuator(radiant, ichan, RADIANT_ATTEN_SIG, cfg.radiant.analog.digi_attenuation[ichan]*4); 
         radiant_set_attenuator(radiant, ichan, RADIANT_ATTEN_TRIG, cfg.radiant.analog.trig_attenuation[ichan]*4); 
       }
  }



  //set up DMA correctly 
  radiant_reset_counters(radiant); 
  radiant_set_nbuffers_per_readout(radiant, cfg.radiant.readout.nbuffers_per_readout); 
  radiant_dma_setup_event(radiant, cfg.radiant.readout.readout_mask); 
 
  //then do the rest of the configuration 
  radiant_configure(); 
  return 0; 
}



/** The acquisition thread 
 *
 * This has sole control over the SPI interface for the RADIANT.
 * Since configuration is always done over UART, this does not need to react to config changes,
 * but it may need to temporarily pause. For this reason it acquires a read lock on the radiant_config lock. 
 *
 **/ 
void * acq_thread(void* v) 
{
  (void) v; 
  while(!quit) 
  {

    //acquire read lock on radiant, flower, and cfg 
    pthread_rwlock_rdlock(&radiant_lock);
    pthread_rwlock_rdlock(&flower_lock);
    pthread_rwlock_rdlock(&cfg_lock);


    // wait for the RADIANT to trigger
    //TODO handle clear flag, though we don't really want one
    
    if (radiant_poll_trigger_ready(radiant, cfg.radiant.readout.poll_ms)) 
    {
      // Get a buffer , and fill it
      acq_buffer_item_t * mem = ice_buf_getmem(acq_buffer); 
      radiant_read_event(radiant, &mem->hd, &mem->wf);
      flower_fill_header(flower, &mem->hd); 
      ice_buf_commit(acq_buffer); 
    }


    //release the read locks
    pthread_rwlock_unlock(&cfg_lock);
    pthread_rwlock_unlock(&flower_lock); 
    pthread_rwlock_unlock(&radiant_lock); 

  }

  return 0;
}


typedef struct flower_servo_state
{
  float value[RNO_G_NUM_LT_CHANNELS]; 
  float last_value[RNO_G_NUM_LT_CHANNELS]; 
  float error[RNO_G_NUM_LT_CHANNELS]; 
  float last_error[RNO_G_NUM_LT_CHANNELS]; 
  float sum_error[RNO_G_NUM_LT_CHANNELS]; 
} flower_servo_state_t; 


typedef struct radiant_servo_state
{
  int max_periods; 
  int nperiods_populated; 
  float period_weights[NUM_SERVO_PERIODS]; 
  int nscaler_periods_per_servo_period[NUM_SERVO_PERIODS]; 
  float * scaler_v[RNO_G_NUM_RADIANT_CHANNELS]; 
  float * scaler_v_mem; 
  float value[RNO_G_NUM_RADIANT_CHANNELS]; 
  float last_value[RNO_G_NUM_RADIANT_CHANNELS]; 
  float error[RNO_G_NUM_RADIANT_CHANNELS]; 
  float last_error[RNO_G_NUM_RADIANT_CHANNELS]; 
  float sum_error[RNO_G_NUM_RADIANT_CHANNELS]; 
  int nsum; 

} radiant_servo_state_t; 

static void update_radiant_servo_state(radiant_servo_state_t * st, const rno_g_daqstatus_t * ds) 
{

  int idx = (st->nperiods_populated++) % st->max_periods; 
  int max_idxs = st->nperiods_populated < st->max_periods ? st->nperiods_populated : st->max_periods; 

  for (int chan = 0; chan < RNO_G_NUM_RADIANT_CHANNELS; chan++) 
  {
    //calculate adjusted scaler
    float adjusted_scaler = ds->radiant_scalers[chan] * (1 + ds->radiant_prescalers[chan]) / (ds->radiant_scaler_period?:1); 

    //put in rolling window
    st->scaler_v[chan][idx] = adjusted_scaler; 


    st->last_value[chan] = st->value[chan]; 
    st->value[chan] = 0; 
    for (int j = 0; j < NUM_SERVO_PERIODS; j++) 
    {
      if (!st->period_weights[j]) continue; 
      int nthis = 0; 
      float sumthis = 0;
      for (int i = 0; i < max_idxs; i++) 
      {
        if (i < st->nscaler_periods_per_servo_period[j])
        {
          sumthis += st->scaler_v[chan][(st->nperiods_populated-1-i) % st->max_periods ]; 
          nthis++; 
        }
      }
      st->value[chan] += st->period_weights[j]*sumthis/nthis; 
    }

    st->last_error[chan] = st->error[chan]; 
    st->error[chan] = st->value[chan] - cfg.radiant.servo.scaler_goals[chan]; 
    st->sum_error[chan] += st->error[chan]; 
  }
  st->nsum++; 

  return; 
}

static void setup_flower_servo_state(flower_servo_state_t * st)
{
  memset(st, 0, sizeof(flower_servo_state_t)); 
}

static void update_flower_servo_state(flower_servo_state_t *st, const rno_g_daqstatus_t * ds) 
{

  float sw = cfg.lt.servo.slow_scaler_weight; 
  float fw = cfg.lt.servo.fast_scaler_weight; 


  const rno_g_lt_scaler_group_t * slow = &ds->lt_scalers.s_100mHz;
  const rno_g_lt_scaler_group_t * fast = &ds->lt_scalers.s_1Hz;
  const rno_g_lt_scaler_group_t * fast_gated = &ds->lt_scalers.s_1Hz_gated;

  int sub = cfg.lt.servo.subtract_gated; 
  
  for (int i = 0; i < RNO_G_NUM_LT_CHANNELS; i++)
  {

    float val =  sw * slow->servo_per_chan[i]+ fw *(fast->servo_per_chan[i]-sub*fast_gated->servo_per_chan[i]);
    st->last_value[i] = st->value[i]; 
    st->value[i] = val; 
    st->last_error[i] = st->error[i]; 
    st->error[i] = val-cfg.lt.servo.scaler_goals[i]; 
    st->sum_error[i] += st->error[i]; 
  } 
}

static void setup_radiant_servo_state(radiant_servo_state_t * state) 
{
  int max_periods = 0; 
  for (int i = 0; i < NUM_SERVO_PERIODS; i++) 
  {
    if (cfg.radiant.servo.nscaler_periods_per_servo_period[i] > max_periods) 
    {
      max_periods = cfg.radiant.servo.nscaler_periods_per_servo_period[i]; 
    }
  }


  if (state->max_periods < max_periods)
  {

    if (state->max_periods) 
      free(state->scaler_v_mem); 
    state->scaler_v_mem = malloc(sizeof(int) * max_periods * RNO_G_NUM_RADIANT_CHANNELS); 
    state->max_periods = max_periods; 
    for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++) 
    {
      state->scaler_v[i]  = state->scaler_v_mem + max_periods * i; 
    }
  }


  memset(state,0, sizeof(*state)); 
  memcpy(state->nscaler_periods_per_servo_period, cfg.radiant.servo.nscaler_periods_per_servo_period, sizeof(*state->nscaler_periods_per_servo_period)); 
  memcpy(state->period_weights, cfg.radiant.servo.period_weights, sizeof(*state->period_weights)); 


}

static struct drand48_data sw_rand; 
double calc_next_sw_trig(float now)
{
  if (!cfg.radiant.trigger.soft.enabled) return 0; 

  double interval = cfg.radiant.trigger.soft.interval; 
  double  u; 
  if (cfg.radiant.trigger.soft.interval_jitter) 
  {
    drand48_r(&sw_rand,&u); 
    interval += 2*cfg.radiant.trigger.soft.interval_jitter*(u-0.5); 
  }

  if (cfg.radiant.trigger.soft.use_exponential_distribution) 
  {
    drand48_r(&sw_rand,&u); 
    return  now-log(u)*interval; 
  }
  else return now+interval; 
}


/** This is the monitor thread 
 *  This is responsible for force triggers and servoing. 
 *
 * */ 
static void * mon_thread(void* v) 
{
  (void) v;

  //start time
  struct timespec start; 
  clock_gettime(CLOCK_MONOTONIC, &start); 


  //last monitor time 
  double last_scalers_radiant = 0;
  double last_scalers_lt = 0;

  //last srevo time
  double last_servo_radiant = 0;
  double last_servo_lt = 0;
  

  //last output time 
  double last_daqstatus_out = 0; 
  static int last_cfg_counter = -1; 

  double next_sw_trig = -1; 
  radiant_servo_state_t rad_servo_state = {0}; 
  flower_servo_state_t flwr_servo_state = {0}; 
  while(!quit) 
  {
    struct timespec now; 
    clock_gettime(CLOCK_MONOTONIC, &now); 
    double nowf = now.tv_sec + 1e-9 * now.tv_nsec; 

    //figure out how long it's been since we got statuses and sent a sw trig
    float diff_scalers_radiant = nowf - last_scalers_radiant;
    float diff_scalers_lt = nowf - last_scalers_lt; 
    float diff_servo_radiant = nowf - last_servo_radiant; 
    float diff_servo_lt = nowf - last_servo_lt;
    float diff_last_daqstatus_out = nowf - last_daqstatus_out; 

    if (next_sw_trig  < 0) 
    {
      next_sw_trig = calc_next_sw_trig(nowf); 
    }

    //re set up the RADIANT 
    if (config_counter > last_cfg_counter) 
    {
      last_cfg_counter = config_counter; 
      setup_radiant_servo_state(&rad_servo_state); 
      setup_flower_servo_state(&flwr_servo_state); 
    }

    //Hold the config read lock to avoid values getting take from underneath us
    pthread_rwlock_rdlock(&cfg_lock);

    //do we need radiant scalers? 
    if (cfg.radiant.servo.scaler_update_interval && cfg.radiant.servo.scaler_update_interval < diff_scalers_radiant)  
    {
      int ok = radiant_read_daqstatus(radiant, ds); 
      if (!ok) fprintf(stderr,"Problem reading daqstatust"); 

      //update the running averages for the radiant 
      update_radiant_servo_state(&rad_servo_state, ds); 
      last_scalers_radiant = nowf; 
    }

    // do we need to servo radiant? 
    if (cfg.radiant.servo.enable && cfg.radiant.servo.servo_interval
        && cfg.radiant.servo.scaler_update_interval < diff_servo_radiant)  
    {
      for (int ch = 0; ch < RNO_G_NUM_RADIANT_CHANNELS; ch++) 
      {
         double dthreshold = cfg.radiant.servo.P * rad_servo_state.error[ch] + 
                             cfg.radiant.servo.I * rad_servo_state.sum_error[ch] + 
                             cfg.radiant.servo.D * (rad_servo_state.error[ch] - rad_servo_state.last_error[ch]); 

         if (cfg.radiant.servo.max_thresh_change && fabs(dthreshold) > cfg.radiant.servo.max_thresh_change)
         {
           dthreshold = (dthreshold < 0)  ? -cfg.radiant.servo.max_thresh_change : cfg.radiant.servo.max_thresh_change; 
         }

         ds->radiant_thresholds[ch] += dthreshold; 
      }

      //set the thresholds

      radiant_set_trigger_thresholds(radiant, 0, RNO_G_NUM_RADIANT_CHANNELS-1, ds->radiant_thresholds); 
      last_servo_radiant = nowf; 
    }


    // do we need LT scalers? 
    if (cfg.lt.servo.scaler_update_interval && cfg.lt.servo.scaler_update_interval < diff_scalers_lt)  
    {
      flower_fill_daqstatus(flower, ds); 
      update_flower_servo_state(&flwr_servo_state, ds); 
      last_scalers_lt = nowf; 
    }

    // do we need to servo LT? 

    if (cfg.lt.servo.enable && cfg.lt.servo.servo_interval
        && cfg.lt.servo.scaler_update_interval < diff_servo_lt)  
    {
      for (int ch = 0; ch < RNO_G_NUM_LT_CHANNELS; ch++) 
      {
         double d_servo_threshold = cfg.lt.servo.P * flwr_servo_state.error[ch] + 
                                    cfg.lt.servo.I * flwr_servo_state.sum_error[ch] + 
                                    cfg.lt.servo.D * (flwr_servo_state.error[ch] - flwr_servo_state.last_error[ch]); 

         ds->lt_servo_thresholds[ch] = clamp( ds->lt_servo_thresholds[ch] + d_servo_threshold, 0, 255);
         ds->lt_trigger_thresholds[ch] = clamp( (ds->lt_servo_thresholds[ch] - cfg.lt.servo.servo_thresh_offset) / cfg.lt.servo.servo_thresh_frac, 0, 255);
      }

      flower_set_thresholds(flower,  ds->lt_trigger_thresholds, ds->lt_servo_thresholds, 7); 
      last_servo_lt = nowf; 
    }
    

    //do we need to write out the DAQ status? 

    if (cfg.output.daqstatus_interval && cfg.output.daqstatus_interval < diff_last_daqstatus_out)  
    {
      mon_buffer_item_t * mem = ice_buf_getmem(mon_buffer); 
      memcpy(&mem->ds,ds, sizeof(rno_g_daqstatus_t)); 
      ice_buf_commit(mon_buffer); 
    }



    //do we need to send a soft trigger? 

    if (nowf > next_sw_trig) 
    {
      radiant_soft_trigger(radiant); 
      next_sw_trig = calc_next_sw_trig(nowf); 
    }

    //release cfg lock
    pthread_rwlock_unlock(&cfg_lock); 

    float sleep_amt = 0.1; //maximum sleep amount

    usleep(sleep_amt *1e6); 
  }
  return 0; 
}

//this makes the necessary directories for a time 
//returns 0 on success. 
static int make_dirs_for_output(const char * prefix) 
{ 
  char buf[strlen(prefix) + 512];  

  //check to see that prefix exists and is a directory
  if (mkdir_if_needed(prefix))
  {
    fprintf(stderr,"Couldn't find %s or it's not a directory. Bad things will happen!\n",prefix); 
    return 1; 
  }


  int i;
  const char * subdirs[] = {"waveforms","header","daqstatus","aux","cfg"}; 
  const int nsubdirs = sizeof(subdirs) / sizeof(*subdirs); 
  for (i = 0; i < nsubdirs; i++)
  {
    snprintf(buf,sizeof(buf), "%s/%s",prefix,subdirs[i]); 
    if (mkdir_if_needed(buf))
    {
        fprintf(stderr,"Couldn't make %s. Bad things will happen!\n",buf); 
        return 1; 
    }
  }

  return 0; 
}

const char * tmp_suffix = ".tmp"; 
const int tmp_suffix_len = 4; 


int do_close(rno_g_file_handle_t h, char *path)  
{
  int ret = rno_g_close_handle(&h);   
  int pathlen = strlen(path); 
  

  if (!strcasecmp(path + pathlen - tmp_suffix_len, tmp_suffix))
  {
    char * final_path = strdup(path);  
    final_path[pathlen-tmp_suffix_len] = 0; 
    rename(path,final_path); 
    free(final_path); 
  }
  free(path); 
  return ret; 
}


static void * wri_thread(void* v) 
{
  (void) v; 
  time_t start_time = time(0); 
  time_t last_print_out = start_time; 

  int wf_file_size = 0; 
  int ds_file_size = 0; 

  int wf_file_N = 0; 
  int ds_file_N = 0; 


  acq_buffer_item_t acq_item;
  mon_buffer_item_t mon_item;

  char * wf_file_name = NULL; 
  char * hd_file_name = NULL; 
  char * ds_file_name = NULL; 

  rno_g_file_handle_t wf_handle = {0};
  rno_g_file_handle_t hd_handle = {0};
  rno_g_file_handle_t ds_handle = {0};

  time_t wf_file_time = 0; 
  time_t ds_file_time = 0; 

  int bigbuflen = strlen(cfg.output.base_dir)+512+1; 
  char * bigbuf = calloc(bigbuflen,1); 

  int num_events = 0; 
  int num_events_this_cycle = 0; 

  int ds_i = 0; 

  snprintf(bigbuf, bigbuflen, "%s/run%d/", cfg.output.base_dir, run_number); 
  output_dir = strdup(bigbuf); 
  if (make_dirs_for_output(output_dir))
  {
    please_stop();
    return 0; 
  }

  //if we have pedestals, write them out 
  if (pedestals) 
  {
    snprintf(bigbuf,bigbuflen,"%s/pedestals.dat.gz", output_dir); 
    rno_g_file_handle_t ped_h; 
    rno_g_init_handle(&ped_h, bigbuf,"w");  
    rno_g_pedestal_write(ped_h, pedestals); 
    rno_g_close_handle(&ped_h); 
  }



  while (1) 
  {
    time_t now; 
    time(&now); 

    int have_data = 0; 
    int have_status = 0; 

    int acq_occupancy = ice_buf_occupancy(acq_buffer); 
    if (acq_occupancy)
    {
      ice_buf_pop(acq_buffer, &acq_item); 
      num_events++; 
      num_events_this_cycle++; 
      have_data = 1; 
    }

    if (ice_buf_occupancy(mon_buffer))
    {
      ice_buf_pop(mon_buffer, &mon_item); 
      have_status = 1; 
    }


    if (cfg.output.print_interval > 0 && now - last_print_out > cfg.output.print_interval) 
    {
      printf("---------after %u seconds-----------\n", (unsigned) (now - start_time)); 
      printf("  total events written: %d\n", num_events); 
      printf("  write rate:  %g Hz\n", (num_events == 0) ? 0. :  ((float) num_events_this_cycle) / (now - last_print_out)); 
      printf("  write buffer occupancy: %d/%d\n", acq_occupancy , cfg.runtime.acq_buf_size); 
      num_events_this_cycle = 0; 
      rno_g_daqstatus_dump(stdout, ds); 
      last_print_out = now; 
    }

    if (!have_data && !have_status) 
    {
      if (quit) 
      {
      if (wf_file_name) do_close(wf_handle, wf_file_name); 
      if (hd_file_name) do_close(hd_handle, hd_file_name); 
      if (ds_file_name) do_close(ds_handle, ds_file_name); 
        break; 
      }

      //no data, so sleep a bit 
      usleep(50000); 
    }


    if (have_data) 
    {
      if ( !wf_file_name || 
           (cfg.output.max_kB_per_file > 0  &&  wf_file_size > cfg.output.max_kB_per_file) ||
           (cfg.output.max_events_per_file > 0 && wf_file_N > cfg.output.max_events_per_file) ||
           (cfg.output.seconds_per_run > 0 && now - wf_file_time > cfg.output.seconds_per_run ) )
      {
        if (wf_file_name) do_close(wf_handle, wf_file_name); 

         snprintf(bigbuf,bigbuflen,"%s/waveforms/%u.wf.dat.gz%s", output_dir, acq_item.hd.event_number, tmp_suffix ); 
         wf_handle.type = RNO_G_GZIP; 
         wf_handle.handle.gz = gzopen(bigbuf,"w"); 
         wf_file_name = strdup(bigbuf); 
         wf_file_size = 0; 
         wf_file_N = 0; 
         wf_file_time = now; 


         if (hd_file_name) do_close(hd_handle, hd_file_name); 
         snprintf(bigbuf,bigbuflen,"%s/header/%u.hd.dat.gz%s", output_dir, acq_item.hd.event_number, tmp_suffix ); 
         hd_handle.type = RNO_G_GZIP; 
         hd_handle.handle.gz = gzopen(bigbuf,"w"); 
         hd_file_name = strdup(bigbuf); 
      }

      wf_file_size += rno_g_waveform_write(wf_handle, &acq_item.wf); 
      wf_file_N++; 
    }

    if (have_status) 
    {
      if ( !ds_file_name || 
           (cfg.output.max_kB_per_file > 0  &&  ds_file_size > cfg.output.max_kB_per_file) ||
           (cfg.output.max_events_per_file > 0 && ds_file_N > cfg.output.max_events_per_file) ||
           (cfg.output.seconds_per_run > 0 && now - ds_file_time > cfg.output.seconds_per_run ) )
     
      {

    
        if (ds_file_name) do_close(ds_handle, ds_file_name); 
        snprintf(bigbuf,bigbuflen,"%s/status/%d.ds.dat.gz%s", output_dir, ds_i, tmp_suffix ); 
        ds_handle.type = RNO_G_GZIP; 
        ds_handle.handle.gz = gzopen(bigbuf,"w"); 
        ds_file_name = strdup(bigbuf); 
        ds_file_size = 0; 
        ds_file_N = 0; 
        ds_file_time = now; 
      }

      memcpy(ds, &mon_item.ds, sizeof(rno_g_daqstatus_t)); 

      if (shared_ds_fd) msync(ds, sizeof(rno_g_daqstatus_t), MS_ASYNC); 


      ds_file_size+= rno_g_daqstatus_write(ds_handle, &mon_item.ds); 
      ds_file_N++; 
      ds_i++; 
    }

    if ((int) ice_buf_occupancy(acq_buffer) < cfg.runtime.acq_buf_size /3)
    {
      usleep(25000); 
    }

  }


  return 0; 
}


static void signal_handler(int signal,  siginfo_t * sinfo, void * v) 
{
  (void) sinfo; 
  (void) v; 
  if (signal == SIGUSR1) 
  {
    cfg_reread = 1; 
  }
  else
  {
    please_stop();
  }

}



static int initial_setup() 
{

  /** Initialize config lock and try to read the config */ 
  pthread_rwlock_init(&cfg_lock,NULL); 
  read_config(); 

  //since no other threads exit yet, and we haven't set up the signal handler yet, don't bother to take locks. 

  // Read the station number

  const char * station_number_file = "/STATION_ID"; 
  FILE *fstation = fopen(station_number_file,"r"); 
  fscanf(fstation,"%d\n",&station_number); 
  fclose(fstation); 
  if (station_number < 0) 
  {
    fprintf(stderr,"Could not get a station number... using 0\n"); 
    station_number = 0; 
  }


  //Read the runfile, and update it.  
  FILE * frun = fopen(cfg.output.runfile,"r"); 
  if (!frun) 
  {
    fprintf(stderr,"NO RUN FILE FOUND at %s, setting run to 0\n", cfg.output.runfile); 
    run_number = 0; 
  }
  else
  {
    fscanf(frun,"%d\n", &run_number); 
    fclose(frun); 

    //TODO: make sure we don't run out of disk space here
    const char * tmp_run_file = "/tmp/tmprunfile"; 
    frun = fopen("/tmp/tmprunfile","w"); 
    fprintf(frun,"%d\n", run_number+1); 
    fclose(frun); 
    rename(tmp_run_file, cfg.output.runfile); 
  }

  //our output dir is going to be the base_dir + run%d/ 
  asprintf(&output_dir, "%s/run%d/", cfg.output.base_dir, run_number); 
  
  //let's make the output directories
  mkdir_if_needed(output_dir); 

  const char * subdirs[] = {"event","header","daqstatus","aux","cfg"}; 
  char * strbuf = malloc(strlen(output_dir)+100); //long enough for our purposes; 
  for (unsigned i = 0; i < sizeof(subdirs)/sizeof(*subdirs); i++) 
  {
    sprintf(strbuf, "%s/%s", output_dir, subdirs[i]); 
    mkdir_if_needed(strbuf); 
  }

  int need_to_copy_radiant_thresholds = 1; 
  int need_to_copy_lt_thresholds = 1; 

  //open the shared status file, if it's there
  if (cfg.runtime.status_shmem_file && *cfg.runtime.status_shmem_file) 
  {
    shared_ds_fd = open(cfg.runtime.status_shmem_file, O_CREAT | O_RDWR,0755);

    if (shared_ds_fd <= 0) 
    {
      fprintf(stderr, "Could not open %s\n", cfg.runtime.status_shmem_file); 
      shared_ds_fd = 0; 
    }
    else
    {
       size_t file_size = lseek(shared_ds_fd,0,SEEK_END); 
       lseek(shared_ds_fd,0,SEEK_SET); 
       if (file_size != sizeof(rno_g_daqstatus_t))
       {
         ftruncate(shared_ds_fd, sizeof(rno_g_daqstatus_t)); 
       }

       ds = mmap(0, sizeof(rno_g_daqstatus_t), PROT_READ | PROT_WRITE, MAP_SHARED, shared_ds_fd,0); 

       if (cfg.radiant.thresholds.load_from_threshold_file && file_size == sizeof(rno_g_daqstatus_t)) 
         need_to_copy_radiant_thresholds = 0; 

       if (cfg.lt.servo.load_from_threshold_file && file_size == sizeof(rno_g_daqstatus_t)) 
         need_to_copy_lt_thresholds = 0; 
    }
  }

  if (!shared_ds_fd) 
  {
    ds = calloc(sizeof(rno_g_daqstatus_t),1); 
  }

  if (need_to_copy_radiant_thresholds) 
  {
    for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++) 
    {
      ds->radiant_thresholds[i] = cfg.radiant.thresholds.initial[i] * 4096/3.3;   
    }
  }

  if (need_to_copy_lt_thresholds) 
  { 
    for (int i = 0;  i <  RNO_G_NUM_LT_CHANNELS; i++) 
    {
      ds->lt_trigger_thresholds[i] = cfg.lt.servo.initial_trigger_thresholds[i]; 
      ds->lt_servo_thresholds[i] = 
        clamp(cfg.lt.servo.initial_trigger_thresholds[i] * cfg.lt.servo.servo_thresh_frac + cfg.lt.servo.servo_thresh_offset, 0, 255); 
    }
  }
  pthread_rwlock_init(&ds_lock,NULL); 

  //now let's dump the configuration file to the cfg dir 
  sprintf(strbuf,"%s/cfg/acq.cfg", output_dir); 
  FILE * of = fopen(strbuf,"w"); 
  if (!of) 
  {
    fprintf(stderr,"Could not open %s\n", strbuf); 
  }
  else
  {
    dump_acq_config(of,&cfg); 
    fclose(of); 
  }
  free(strbuf); 

  //initialie the radiant lock
  pthread_rwlock_init(&radiant_lock,NULL); 

  //intitial configure of the radiant
  radiant_initial_setup(); 

  pthread_rwlock_init(&flower_lock,NULL); 

  //and the flower
  flower_initial_setup(); 

  //set up signal handlers 
  sigset_t empty; 
  sigemptyset(&empty); 
  struct sigaction sa; 
  sa.sa_mask = empty; 
  sa.sa_flags = 0; 
  sa.sa_sigaction = signal_handler; 
  sigaction(SIGINT,&sa,0);
  sigaction(SIGTERM,&sa,0);
  sigaction(SIGUSR1,&sa,0);


  //initialize the buffers 
  acq_buffer = ice_buf_init(cfg.runtime.acq_buf_size, sizeof(acq_buffer_item_t)); 
  mon_buffer = ice_buf_init(cfg.runtime.mon_buf_size, sizeof(mon_buffer_item_t)); 

  //now let's make the threads
  pthread_create(&the_acq_thread,NULL, acq_thread, NULL); 
  pthread_create(&the_mon_thread,NULL, mon_thread, NULL); 
  pthread_create(&the_wri_thread,NULL, wri_thread, NULL); 

  return 0; 
}




int please_stop()
{
  printf("Stopping...\n"); 
  quit = 1; 
  return 0; 
}



int main(void) 
{
   if (initial_setup()) 
   {
      return 1; 
   }

   struct timespec start_time; 
   clock_gettime(CLOCK_MONOTONIC_COARSE,&start_time); 

   struct timespec now; 
   while (!quit) 
   {

     if (cfg_reread) 
     {
       cfg_reread = 0; 
       read_config(); 
     }

     //check disk space 

     if (cfg.output.min_free_space_MB > 0 ) 
     {
       struct statvfs vfs; 
       statvfs(cfg.output.base_dir,&vfs);
       double MBfree = (vfs.f_bsize * vfs.f_bavail) / ( (double) (1 << 20)); 
       if (MBfree < cfg.output.min_free_space_MB) 
       {
         please_stop(); 
         continue; 
       }
     }

     clock_gettime(CLOCK_MONOTONIC_COARSE,&now); 
     if (now.tv_sec - start_time.tv_sec > cfg.output.seconds_per_run)
     {
       please_stop(); 
     }
     usleep(500e3); 
     sched_yield(); 
   }

  return teardown(); 
}

int teardown() 
{
  pthread_join(the_acq_thread,0);
  pthread_join(the_mon_thread,0);
  pthread_join(the_wri_thread,0);

  radiant_labs_stop(radiant); 
  radiant_close(radiant); 
  flower_close(flower); 


  if (shared_ds_fd) 
  {
    munmap(ds,sizeof(rno_g_daqstatus_t)); 
    close(shared_ds_fd); 
  }

  return 0; 
}

