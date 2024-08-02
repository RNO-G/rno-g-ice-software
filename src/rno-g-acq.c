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
#include <sys/file.h> 
#include <sys/types.h> 
#include <sys/sendfile.h> 
#include <sys/sysinfo.h>
#include <zlib.h>
#include <inttypes.h>
#include <math.h> 
#include <errno.h> 

#include <systemd/sd-daemon.h> 

#include "radiant.h" 
#include "flower.h" 
#include "rno-g.h" 
#include "rno-g-cal.h" 
#include "ice-config.h" 
#include "ice-serve.h"
#include "ice-buf.h"
#include "ice-common.h"
#include "ice-version.h"

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
char * cfgpath = NULL; 

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
static pthread_t the_sck_thread; 

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
static uint32_t radiant_trig_chan = 0; 

/** flower handle */ 
static flower_dev_t * flower = 0; 

uint8_t flower_codes[RNO_G_NUM_LT_CHANNELS]; 

/** radiant pedestals*/ 
static rno_g_pedestal_t * pedestals = 0; 

//File descriptor for pedestal shared mem file 
static int pedestal_fd; 


/** Shared daq status */ 
static rno_g_daqstatus_t * ds = 0; 


//File descriptor for daqstatus shared mem file
static int shared_ds_fd; 

/** calib handle */ 
static rno_g_cal_dev_t * calpulser = 0;  

//acq ring buffer 
static ice_buf_t *acq_buffer; 

//mon ring buffer 
static ice_buf_t *mon_buffer; 

static FILE * file_list = 0; 
static int file_list_fd = 0; 

static FILE * runinfo = 0; 

static time_t last_watchdog; 
static double runfile_partition_free = 0;
static double output_partition_free = 0;

///// PROTOTYPES  /////  

static int radiant_configure();
static int flower_configure();
static int flower_update_pps_offset(); 
static int calpulser_configure(); 
static int teardown(); 
static int please_stop(); 
static void fail(const char *); 
static int add_to_file_list(const char * path); 
static void feed_watchdog(time_t * now) ; 

struct timespec precise_start_time;

static uint32_t delay_clock_estimate = 10000000; 

static pthread_rwlock_t current_status_lock;
static pthread_rwlock_t current_status_text_lock;
static char current_status_text[4096];
static int current_status_text_len;
static char * tmp_current_state_file = 0;

static struct
{
  const char * state;
  struct timespec run_start;
  struct timespec sys_last_updated;
  int num_events; //-1 if not started
  int num_events_last_cycle;
  int last_cycle_length; 
  int num_force_events;
  struct timespec event_last_updated;
  int current_run; // -1 if not started
  float runfile_partition_free;
  float output_partition_free;
  float mem_free;
  float mem_buf;
  float mem_shared;
  float load_avgs[3];
  int nprocs;
  long uptime;
} current_status =  {.num_events = -1, .current_run = -1, .state = "initializing" };

// fills in system part of current_status
static void fill_current_status_sys()
{

  struct sysinfo info;
  sysinfo(&info);
  double runfile_partition_free = get_free_MB_by_path(cfg.output.runfile);
  double output_partition_free = get_free_MB_by_path(cfg.output.base_dir);

  pthread_rwlock_wrlock(&current_status_lock);

  current_status.runfile_partition_free = runfile_partition_free;
  current_status.output_partition_free = output_partition_free;

  current_status.mem_free =info.freeram;
  current_status.mem_buf =info.bufferram;
  current_status.mem_shared =info.sharedram;

  current_status.mem_free *= info.mem_unit / (1024*1024.);
  current_status.mem_buf *= info.mem_unit / (1024*1024.);
  current_status.mem_shared *= info.mem_unit / (1024*1024.);
  current_status.nprocs = info.procs;
  current_status.uptime = info.uptime;
  current_status.load_avgs[0] = info.loads[0]/65536.;
  current_status.load_avgs[1] = info.loads[1]/65536.;
  current_status.load_avgs[2] = info.loads[2]/65536.;
  clock_gettime(CLOCK_REALTIME,&current_status.sys_last_updated);

  pthread_rwlock_unlock(&current_status_lock);
}

static void maybe_update_current_status_text()
{
  static time_t last_updated= 0;
  //update at most once a second
  time_t now;
  time(&now);
  if (now == last_updated)
  {
    return;
  }

  pthread_rwlock_rdlock(&current_status_lock);
  pthread_rwlock_wrlock(&current_status_text_lock);

  last_updated = now;


  current_status_text_len = snprintf(
  current_status_text, sizeof(current_status_text),
  "{\n"
  "  \"state\":\"%s\",\n"
  "  \"run_start\":%ld.%09ld,\n"
  "  \"sys_last_updated\":%ld.%09ld,\n"
  "  \"event_last_updated\":%ld.%09ld,\n"
  "  \"current_run\":%d,\n"
  "  \"num_events\":%d,\n"
  "  \"num_last_cycle\":%d,\n"
  "  \"last_cycle_length\":%d,\n"
  "  \"num_force_events\":%d,\n"
  "  \"runfile_partition_free\":%f,\n"
  "  \"output_partition_free\":%f,\n"
  "  \"mem_free\":%f,\n"
  "  \"mem_buf\":%f,\n"
  "  \"mem_shared\":%f,\n"
  "  \"load_avg\":[%f,%f,%f],\n"
  "  \"nprocs\":%d,\n"
  "  \"uptime\":%ld\n"
  "}",
  current_status.state,
  current_status.run_start.tv_sec, current_status.run_start.tv_nsec,
  current_status.sys_last_updated.tv_sec, current_status.sys_last_updated.tv_nsec,
  current_status.event_last_updated.tv_sec, current_status.event_last_updated.tv_nsec, 
  current_status.current_run,
  current_status.num_events,
  current_status.num_events_last_cycle,
  current_status.last_cycle_length,
  current_status.num_force_events,
  current_status.runfile_partition_free,
  current_status.output_partition_free,
  current_status.mem_free,
  current_status.mem_buf,
  current_status.mem_shared,
  current_status.load_avgs[0], current_status.load_avgs[1], current_status.load_avgs[2],
  current_status.nprocs,
  current_status.uptime
  );

  pthread_rwlock_unlock(&current_status_text_lock);
  pthread_rwlock_unlock(&current_status_lock);
}




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

  //try to load the same cfgpath each time, if possible. 
  char * found_config = 0; 
  char * renamed_cfg = 0; 
  FILE * fptr =  find_config("acq.cfg", cfgpath, &found_config, &renamed_cfg) ; 


  if (!fptr) 
  {
    if (first_time) 
    {
      fprintf(stderr,"!!! This means we are using the default cfg. Hopefully it works for you?\n"); 
    }
  }
  else
  {
    printf("Using%s config file %s\n", renamed_cfg ? " one-time": "", found_config); 

    // try to use the config again the next reread if not onetime? 
    if (!renamed_cfg) cfgpath = found_config; 
    else free(found_config); 
    if (read_acq_config(fptr, &cfg))
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
    time_t now; 
    time(&now); 
    asprintf(&ofname,"%s/cfg/acq.%d.%lu.cfg", output_dir, config_counter,now); 
    FILE * of = fopen(ofname,"w"); 
    dump_acq_config(of, &cfg); 
    fclose(of); 
    add_to_file_list(ofname); 
    free(ofname); 
  }

  if (tmp_current_state_file) free(tmp_current_state_file);
  asprintf(&tmp_current_state_file,"%s.tmp", cfg.output.current_state_location);

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

    if (memcpy(&old_cfg.calib, &cfg.calib, sizeof(cfg.calib)))
    {
      calpulser_configure(); 
    }
  }

}

int add_to_file_list(const char *path) 
{
  flock(file_list_fd, LOCK_EX); 
  fprintf(file_list,"%s\n", path); 
  fflush(file_list); 
  flock(file_list_fd, LOCK_UN); 
  return 0; 
}

//Feeds the systemd watchdog
void feed_watchdog(time_t * now) 
{
  time_t when;
  if (!now) time(&when) ; 
  else when = *now; 
  sd_notify(0,"WATCHDOG=1"); 
  last_watchdog = when; 
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

  uint16_t sampling_rate=radiant_get_sample_rate(radiant);

  int maybe_rf0_clock_delay = round(cfg.radiant.trigger.RF[0].readout_delay*sampling_rate/(128.*1000));
  uint8_t rf0_clock_delay = maybe_rf0_clock_delay < 0 ?  0 :
                    maybe_rf0_clock_delay > 0x7f ? 0x7f :
                    maybe_rf0_clock_delay;

  int maybe_rf1_clock_delay = round(cfg.radiant.trigger.RF[1].readout_delay*sampling_rate/(128.*1000));
  uint8_t rf1_clock_delay = maybe_rf1_clock_delay < 0 ?  0 :
                    maybe_rf1_clock_delay > 0x7f ? 0x7f :
                    maybe_rf1_clock_delay;

  radiant_set_delay_settings(radiant,rf0_clock_delay,rf1_clock_delay,
                      cfg.radiant.trigger.RF[0].readout_delay_mask,cfg.radiant.trigger.RF[1].readout_delay_mask);

  radiant_set_scaler_period(radiant, cfg.radiant.scalers.use_pps ? 0 : cfg.radiant.scalers.period); 

  for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++)
  {
    radiant_set_prescaler(radiant,i, cfg.radiant.scalers.prescal_m1[i]); 
  }

  uint32_t global_mask = 
    ((!!cfg.radiant.trigger.RF[0].enabled) * cfg.radiant.trigger.RF[0].mask) |
    ((!!cfg.radiant.trigger.RF[1].enabled) * cfg.radiant.trigger.RF[1].mask);

  int ret = radiant_set_global_trigger_mask(radiant, global_mask); 

  radiant_trig_chan = 0; 

  ret += radiant_configure_rf_trigger(radiant, RADIANT_TRIG_A, 
      cfg.radiant.trigger.RF[0].enabled ? cfg.radiant.trigger.RF[0].mask  : 0, 
      cfg.radiant.trigger.RF[0].num_coincidences, cfg.radiant.trigger.RF[0].window); 

  if (cfg.radiant.trigger.RF[0].enabled) radiant_trig_chan |= cfg.radiant.trigger.RF[0].mask; 

  ret += radiant_configure_rf_trigger(radiant, RADIANT_TRIG_B, 
      cfg.radiant.trigger.RF[1].enabled ? cfg.radiant.trigger.RF[1].mask  : 0, 
      cfg.radiant.trigger.RF[1].num_coincidences, cfg.radiant.trigger.RF[1].window); 

  if (cfg.radiant.trigger.RF[1].enabled) radiant_trig_chan |= cfg.radiant.trigger.RF[1].mask; 


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

static void set_calpulser_atten(float atten) 
{
    if (atten < 0) atten = 0; 
    if (atten > 31.5) atten = 31.5; 
    atten = round(atten*2); 
    rno_g_cal_set_atten(calpulser,(uint8_t) atten); 
}

int calpulser_configure() 
{
  pthread_rwlock_rdlock(&cfg_lock); 
  if (cfg.calib.enable_cal && !calpulser) 
  {
    // figure out the rev
    char rev ='E';  

    //check if calib.rev is a file 
    if (cfg.calib.rev[0]=='/') 
    {
      FILE * frev = fopen(cfg.calib.rev,"r"); 
      if (!frev) 
      {
        fprintf(stderr,"WARNING: calib.rev looks like a file but we can't open it!\n"); 
      }
      else
      {
        int nread = fread(&rev, 1,1,frev); 
        if (!nread || rev == '\n') 
        {
          fprintf(stderr,"WARNING: calib.rev is a file but it seems to be empty! Assuming REVE\n"); 
          rev = 'E'; 
        }
        fclose(frev); 
      }
    }
    else
    {
      rev = cfg.calib.rev[0]; 
    }
    calpulser = rno_g_cal_open(cfg.calib.i2c_bus, cfg.calib.gpio, rev) ;
    if (!calpulser) 
    {
      fprintf(stderr,"Could not open calpulser\n"); 
      return 1; 
    }
    //enable the calpulser and initialize it
    rno_g_cal_enable(calpulser); 
    rno_g_cal_wait_ready(calpulser); 
    rno_g_cal_setup(calpulser); 
  }
  else if (calpulser && !cfg.calib.enable_cal)
  {
    //forget everything if the calpulser is not enabled 
    rno_g_cal_disable(calpulser); 
    rno_g_cal_close(calpulser); 
    calpulser = NULL; 
  }

  //now set the rest of the stuff 
  if (calpulser) 
  {
    rno_g_cal_select(calpulser, cfg.calib.channel); 
    rno_g_cal_set_pulse_mode(calpulser,cfg.calib.type); 
    set_calpulser_atten(cfg.calib.atten); 
  }
  pthread_rwlock_unlock(&cfg_lock); 
  return 0; 
}





int write_gain_codes(char * buf) 
{
  if (!flower) return -1; 
  static int gain_codes_counter = 0; 
  time_t now; 
  time(&now); 

  sprintf(buf, "%s/aux/flower_gain_codes.%d.txt", output_dir, gain_codes_counter++); 
  FILE * of = fopen(buf,"w"); 
  if (!of) return 1; 
  fprintf(of,"# Flower gain codes, station=%d, run=%d,  time=%lu\n", station_number, run_number, now); 
  for (int i = 0; i < RNO_G_NUM_LT_CHANNELS; i++) 
  {
    fprintf(of, "%u%s", flower_codes[i], i < RNO_G_NUM_LT_CHANNELS -1 ? " " : "\n"); 
  }
  fclose(of); 
  add_to_file_list(buf); 
  return 0; 
}


/** this configures the flower trigger. It holds the flower write lock (and acquires the config read lock)*/ 
int flower_configure() 
{
  if (!flower) return -1; 

  pthread_rwlock_wrlock(&flower_lock); 
  pthread_rwlock_rdlock(&cfg_lock); 
  rno_g_lt_simple_trigger_config_t ltcfg; 
  ltcfg.window = cfg.lt.trigger.window; 
  ltcfg.vpp_mode = cfg.lt.trigger.vpp; 
  ltcfg.num_coinc =cfg.lt.trigger.enable_rf_trigger ?  cfg.lt.trigger.min_coincidence-1 : 4; 
  int ret = flower_configure_trigger(flower, ltcfg); 

  flower_trigger_enables_t trig_enables = {
    .enable_coinc=cfg.lt.trigger.enable_rf_trigger, 
    .enable_pps = 0, 
    .enable_ext = 0
  };

  flower_trigout_enables_t trigout_enables = {
    .enable_rf_sysout=cfg.lt.trigger.enable_rf_trigger_sys_out, 
    .enable_rf_auxout=cfg.lt.trigger.enable_rf_trigger_sma_out, 
    .enable_pps_sysout=cfg.lt.trigger.enable_pps_trigger_sys_out, 
    .enable_pps_auxout=cfg.lt.trigger.enable_pps_trigger_sma_out
  };


  

  if (!cfg.lt.gain.auto_gain) 
  {
    flower_set_gains(flower, cfg.lt.gain.fixed_gain_codes); 
    memcpy(flower_codes, cfg.lt.gain.fixed_gain_codes, sizeof(flower_codes));
  }

if (cfg.lt.trigger.enable_pps_trigger_sys_out || cfg.lt.trigger.enable_pps_trigger_sma_out)
  {
    flower_update_pps_offset(); 
  }

  flower_set_trigger_enables(flower,trig_enables);
  flower_set_trigout_enables(flower,trigout_enables);


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
  if (!flower) return -1; 

  //do the auto gain if asked to 
  if (cfg.lt.gain.auto_gain) 
  {
    float target = cfg.lt.gain.target_rms; 
    //disable the coincident trigger momentarily 
    flower_trigger_enables_t trig_enables = {.enable_coinc=0, .enable_pps = 0, .enable_ext = 0};
    flower_set_trigger_enables(flower,trig_enables);
    flower_equalize(flower, target,flower_codes,FLOWER_EQUALIZE_VERBOSE); 
  }
  

  flower_set_thresholds(flower,  ds->lt_trigger_thresholds, ds->lt_servo_thresholds, 0xf); 
  //then the rest of the configuration; 
  flower_configure(); 


  return 0; 
}


const char * bias_scan_tmpfile = "/tmp/bias_scan.dat.gz"; 
static int did_bias_scan = 0; 

static int do_bias_scan() 
{

  printf("Starting bias scan...\n");
  pthread_rwlock_wrlock(&current_status_lock);
  current_status.state = "bias scan";
  pthread_rwlock_unlock(&current_status_lock);

  //write to a temporary file, then we'll move ite
  rno_g_file_handle_t hbias; 
  if (rno_g_init_handle(&hbias,bias_scan_tmpfile, "w"))
  {  
    fprintf(stderr,"Trouble opening %s for writing\n. Skipping bias scan.", bias_scan_tmpfile); 
    return 1; 
  }


  //apply attenuation
  if (cfg.radiant.bias_scan.apply_attenuation) 
  {
     for (int ichan = 0; ichan < RNO_G_NUM_RADIANT_CHANNELS; ichan++) 
     {
       radiant_set_attenuator(radiant, ichan, RADIANT_ATTEN_SIG, clamp(cfg.radiant.bias_scan.attenuation,0,31.75)*4); 
     }
  }
   

 //make sure we apply the lab4 vbias in this case, otherwise it will be lost! 
  cfg.radiant.analog.apply_lab4_vbias = 1; 

  rno_g_pedestal_t ped; 
  ped.station = station_number; 

  for (int val = cfg.radiant.bias_scan.min_val; 
      val <= cfg.radiant.bias_scan.max_val; 
      val+= cfg.radiant.bias_scan.step_val)
  {
    printf("Setting bias to %d\n", val);
    radiant_set_dc_bias(radiant, val, val); 
    usleep(1e6*cfg.radiant.bias_scan.sleep_time); 

    feed_watchdog(0); //don't get killed by watchdog 
    radiant_compute_pedestals(radiant, 0xffffff, cfg.radiant.bias_scan.navg_per_step, &ped); 

    rno_g_pedestal_write(hbias, &ped); 
  }

  rno_g_close_handle(&hbias); 
  did_bias_scan =1; 

  //TODO: there's no way we can restore, is there? 
  if (cfg.radiant.bias_scan.apply_attenuation) 
  {
     for (int ichan = 0; ichan < RNO_G_NUM_RADIANT_CHANNELS; ichan++) 
     {
       radiant_set_attenuator(radiant, ichan, RADIANT_ATTEN_SIG, 0); 
     }
  }

  printf("Done with bias scan\n");

  return 0; 
}
 

/* Initial radiant config, including potential pedestal taking and even bias scans! 
 *
 * this happens before threads start while holding config lock. 
 *  
 *
 * */ 
int radiant_initial_setup() 
{



  if (!radiant) return -1; 
  //just in case 
  radiant_labs_stop(radiant); 
  radiant_sync(radiant); //try to reset counters

  radiant_set_internal_triggers_per_cycle(radiant, cfg.radiant.pedestals.ntriggers_per_cycle, cfg.radiant.pedestals.sleep_per_cycle); 

  //bias scan first, if we do it
  if (cfg.radiant.bias_scan.enable_bias_scan && ((cfg.radiant.bias_scan.skip_runs < 2) || ((run_number % cfg.radiant.bias_scan.skip_runs) == 0)))
  {
    do_bias_scan(); 
  }
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
      radiant_set_td_bias(radiant, i, (int) (cfg.radiant.analog.diode_vbias[i]*2000)); 
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
         radiant_set_attenuator(radiant, ichan, RADIANT_ATTEN_SIG, clamp(cfg.radiant.pedestals.attenuation,0,31.75)*4); 
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

    pedestals->station = station_number; 

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
         radiant_set_attenuator(radiant, ichan, RADIANT_ATTEN_SIG, clamp(cfg.radiant.analog.digi_attenuation[ichan],0,31.75)*4); 
         radiant_set_attenuator(radiant, ichan, RADIANT_ATTEN_TRIG, clamp(cfg.radiant.analog.trig_attenuation[ichan],0,31.75)*4); 
       }
  }




  //set thresholds 
  radiant_set_trigger_thresholds(radiant, 0, RNO_G_NUM_RADIANT_CHANNELS-1, ds->radiant_thresholds); 

  //set up DMA correctly 
  radiant_reset_fifo_counters(radiant); 
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
  pthread_rwlock_wrlock(&current_status_lock);
  current_status.state = "acquiring";
  pthread_rwlock_unlock(&current_status_lock);


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
      if (flower) flower_fill_header(flower, &mem->hd); 
      mem->hd.run_number = run_number;
      mem->wf.run_number = run_number;
      mem->hd.station_number = station_number;
      mem->wf.station= station_number;
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

    if (cfg.radiant.servo.use_log)
    {
      st->value[chan] = log10(cfg.radiant.servo.log_offset + st->value[chan]);
    }

    st->last_error[chan] = st->error[chan]; 
    st->error[chan] = (st->value[chan] - cfg.radiant.servo.scaler_goals[chan]); 
    st->sum_error[chan] += st->error[chan]; 
    if (fabs(st->sum_error[chan]) > cfg.radiant.servo.max_sum_err)
    {
      st->sum_error[chan] = st->sum_error[chan] < 0 ? -cfg.radiant.servo.max_sum_err: cfg.radiant.servo.max_sum_err; 
    }

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


  const rno_g_lt_scaler_group_t * fast = &ds->lt_scalers.s_100Hz;
  const rno_g_lt_scaler_group_t * slow = &ds->lt_scalers.s_1Hz;
  const rno_g_lt_scaler_group_t * slow_gated = &ds->lt_scalers.s_1Hz_gated;

  int sub = cfg.lt.servo.subtract_gated; 
  static float fast_factor = 0; 
  if (!fast_factor) 
  {

    uint8_t rev, major, minor; 
    flower_get_fwversion(flower, &major,&minor,&rev,0,0,0); 

    if (!major && !minor && rev < 6) fast_factor = 1000; 
    else fast_factor = 100; 
  }
  
  for (int i = 0; i < RNO_G_NUM_LT_CHANNELS; i++)
  {

    float val =  fw * fast_factor*fast->servo_per_chan[i]+ sw *(slow->servo_per_chan[i]-sub*slow_gated->servo_per_chan[i]);
    st->last_value[i] = st->value[i]; 
    st->value[i] = val; 
    st->last_error[i] = st->error[i]; 
    st->error[i] = (val-cfg.lt.servo.scaler_goals[i]); 
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

    if (state->scaler_v_mem) 
    {
      free(state->scaler_v_mem); 
      memset(state,0, sizeof(*state)); 
    }
    state->scaler_v_mem = malloc(sizeof(int) * max_periods * RNO_G_NUM_RADIANT_CHANNELS); 
    state->max_periods = max_periods; 
    for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++) 
    {
      state->scaler_v[i]  = state->scaler_v_mem + max_periods * i; 
    }
  }


  memcpy(state->nscaler_periods_per_servo_period, cfg.radiant.servo.nscaler_periods_per_servo_period, sizeof(int) * NUM_SERVO_PERIODS); 
  memcpy(state->period_weights, cfg.radiant.servo.period_weights, sizeof(float) * NUM_SERVO_PERIODS); 


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


  //initial configuration of the calpulser 
  calpulser_configure(); 

  float sweep_atten = cfg.calib.sweep.start_atten; 

  float sweep_time = 0; 
  if (cfg.calib.sweep.enable) 
  {
    set_calpulser_atten(sweep_atten); 
    sweep_time = start.tv_sec + 1e-9*start.tv_nsec; 
  }

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
  float flower_float_thresh[RNO_G_NUM_LT_CHANNELS]; 
  for (int i = 0; i < RNO_G_NUM_LT_CHANNELS; i++) flower_float_thresh[i] = ds->lt_servo_thresholds[i];
  uint32_t min_rad_thresh = 0; 
  uint32_t max_rad_thresh = 0; 
  uint32_t max_rad_change = 0;
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
    float diff_sweep = nowf - sweep_time; 


    //re set up the RADIANT 
    if (config_counter > last_cfg_counter) 
    {
      last_cfg_counter = config_counter; 
      setup_radiant_servo_state(&rad_servo_state); 
      setup_flower_servo_state(&flwr_servo_state); 
      min_rad_thresh = cfg.radiant.thresholds.min * 16777215/2.5; 
      max_rad_thresh = cfg.radiant.thresholds.max * 16777215/2.5; 
      max_rad_change = cfg.radiant.servo.max_thresh_change * 16777215/2.5; 
      for (int i = 0; i < RNO_G_NUM_LT_CHANNELS; i++) flower_float_thresh[i] = ds->lt_servo_thresholds[i];
    }

    //Hold the config read lock to avoid values getting take from underneath us
    pthread_rwlock_rdlock(&cfg_lock);

    if (next_sw_trig  < 0) 
    {
      next_sw_trig = calc_next_sw_trig(nowf); 
    }
    //do we need to send a soft trigger? 
    if (cfg.radiant.trigger.soft.enabled && nowf > next_sw_trig) 
    {
      radiant_soft_trigger(radiant); 
      next_sw_trig = calc_next_sw_trig(nowf); 
    }


    //do we need radiant scalers? 
    if (cfg.radiant.servo.scaler_update_interval && cfg.radiant.servo.scaler_update_interval < diff_scalers_radiant)  
    {
      pthread_rwlock_wrlock(&ds_lock);
      while (1) 
      {
        //read twice and make sure equal
        static rno_g_daqstatus_t ds0 = {0}; 
        memcpy(&ds0, ds, sizeof(ds0)); // copy the flower stuff so it doesn't get overwritten
        static uint16_t scaler_check[RNO_G_NUM_RADIANT_CHANNELS]= {0}; 
        int ok = radiant_read_daqstatus(radiant, &ds0)+ radiant_get_scalers(radiant,0,RNO_G_NUM_RADIANT_CHANNELS-1, scaler_check); 

        if (ok) fprintf(stderr,"Problem reading daqstatus\n"); 

        if (!memcmp(ds0.radiant_scalers, scaler_check, sizeof(ds0.radiant_scalers)))
        {
            memcpy(ds, &ds0, sizeof(ds0));
            break; 
        }

        printf("WARNING: Unequal sequential DAQStatus, trying again\n"); 
      }

      pthread_rwlock_unlock(&ds_lock); //we don't need to hold the READ lock in this thread

      //update the running averages for the radiant 
      update_radiant_servo_state(&rad_servo_state, ds); 
      last_scalers_radiant = nowf; 
    }

    // do we need to servo radiant? 
    if (cfg.radiant.servo.enable && cfg.radiant.servo.servo_interval
        && cfg.radiant.servo.scaler_update_interval < diff_servo_radiant)  
    {
      pthread_rwlock_wrlock(&ds_lock);
      for (int ch = 0; ch < RNO_G_NUM_RADIANT_CHANNELS; ch++) 
      {
         //only servo channels that are part of the trigger? 
        if ( 0 == (radiant_trig_chan & (1 << ch))) continue; 

         double dthreshold = cfg.radiant.servo.P * rad_servo_state.error[ch] + 
                             cfg.radiant.servo.I * rad_servo_state.sum_error[ch] + 
                             cfg.radiant.servo.D * (rad_servo_state.error[ch] - rad_servo_state.last_error[ch]); 

         if (max_rad_thresh && fabs(dthreshold) > max_rad_change)
         {
           dthreshold = (dthreshold < 0)  ? -max_rad_change : max_rad_change; 
         }

         ds->radiant_thresholds[ch] -= dthreshold; 
         if (ds->radiant_thresholds[ch] < min_rad_thresh)  ds->radiant_thresholds[ch] = min_rad_thresh; 
         if (ds->radiant_thresholds[ch] > max_rad_thresh)  ds->radiant_thresholds[ch] = max_rad_thresh; 
      }

      pthread_rwlock_unlock(&ds_lock);

      //set the thresholds

      radiant_set_trigger_thresholds(radiant, 0, RNO_G_NUM_RADIANT_CHANNELS-1, ds->radiant_thresholds); 
      last_servo_radiant = nowf; 
    }


    // do we need LT scalers? 
    if (cfg.lt.servo.scaler_update_interval && cfg.lt.servo.scaler_update_interval < diff_scalers_lt && flower)   
    {
      pthread_rwlock_wrlock(&ds_lock);
      flower_fill_daqstatus(flower, ds); 
      pthread_rwlock_unlock(&ds_lock);

      update_flower_servo_state(&flwr_servo_state, ds); 
      //if cycle counter is in the right realm, use it... 
      if (ds->lt_scalers.cycle_counter > 100e6 && ds->lt_scalers.cycle_counter < 136e6) 
      {
        delay_clock_estimate =  ds->lt_scalers.cycle_counter/ 11.8;  //118 MHz clock vs. 10 MHz clock
        //if we have the pps trigger out and it's not 0, let's update our estimate
        if ((cfg.lt.trigger.enable_pps_trigger_sys_out || cfg.lt.trigger.enable_pps_trigger_sma_out) 
            && cfg.lt.trigger.pps_trigger_delay)
        {
          flower_update_pps_offset(); 
        }
      }
      last_scalers_lt = nowf; 
    }

    // do we need to servo LT? 

    if (cfg.lt.servo.enable && cfg.lt.servo.servo_interval
        && cfg.lt.servo.scaler_update_interval < diff_servo_lt && flower)  
    {
      pthread_rwlock_wrlock(&ds_lock);
      for (int ch = 0; ch < RNO_G_NUM_LT_CHANNELS; ch++) 
      {
         double d_servo_threshold = cfg.lt.servo.P * flwr_servo_state.error[ch] + 
                                    cfg.lt.servo.I * flwr_servo_state.sum_error[ch] + 
                                    cfg.lt.servo.D * (flwr_servo_state.error[ch] - flwr_servo_state.last_error[ch]); 

         
         flower_float_thresh[ch] = clamp(flower_float_thresh[ch] + d_servo_threshold,4,120); 
         ds->lt_servo_thresholds[ch] = flower_float_thresh[ch]; 
         ds->lt_trigger_thresholds[ch] = clamp( (flower_float_thresh[ch] - cfg.lt.servo.servo_thresh_offset) / cfg.lt.servo.servo_thresh_frac, 4, 120);
      }
      pthread_rwlock_unlock(&ds_lock);

      flower_set_thresholds(flower,  ds->lt_trigger_thresholds, ds->lt_servo_thresholds, 0xf); 
      last_servo_lt = nowf; 
    }
    

    //do we need to write out the DAQ status? 

    if (cfg.output.daqstatus_interval && cfg.output.daqstatus_interval < diff_last_daqstatus_out)  
    {
      //make sure the station is set correctly 
      ds->station = station_number; 

      // fill in calpulser info 
      if (!calpulser)  // just zero 
      {
        memset(&ds->cal,0,sizeof(ds->cal)); 
      }
      else
      {
        rno_g_cal_fill_info(calpulser, &ds->cal);
      }
 
      mon_buffer_item_t * mem = ice_buf_getmem(mon_buffer); 
      memcpy(&mem->ds,ds, sizeof(rno_g_daqstatus_t)); 
      ice_buf_commit(mon_buffer); 
      last_daqstatus_out = nowf; 
    }


    //do we need to change the calpulser attenuation? 
    if (cfg.calib.sweep.enable && diff_sweep  > cfg.calib.sweep.step_time)
    {
      if (cfg.calib.sweep.stop_atten < cfg.calib.sweep.start_atten) 
      {
        sweep_atten -= fabs(cfg.calib.sweep.atten_step); 
        if (sweep_atten < cfg.calib.sweep.stop_atten) sweep_atten = cfg.calib.sweep.start_atten; 
      }
      else
      {
        sweep_atten += fabs(cfg.calib.sweep.atten_step); 
        if (sweep_atten > cfg.calib.sweep.stop_atten) sweep_atten = cfg.calib.sweep.start_atten; 
      }
      set_calpulser_atten(sweep_atten);
      sweep_time = nowf; 
    }

    //release cfg lock
    pthread_rwlock_unlock(&cfg_lock); 

    float sleep_amt = 0.1; //maximum sleep amount
    
    //sleep less if we need to send a soft trigger sooner
    if ( cfg.radiant.trigger.soft.enabled  && next_sw_trig - nowf < sleep_amt) sleep_amt = (next_sw_trig - nowf)*3./4; 

    usleep(sleep_amt *1e6); 
  }

  //mostly to suppress warnings
  if (rad_servo_state.scaler_v_mem) free(rad_servo_state.scaler_v_mem);

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
    add_to_file_list(final_path); 
    free(final_path); 
  }
  else
  {
    add_to_file_list(path); 
  }
  free(path);
  return ret;
}


static void unlock_wrapper(void *v)
{
  pthread_rwlock_unlock((pthread_rwlock_t*)v);
}

static int request_handler(const ice_serve_request_t * req, ice_serve_response_t * resp, void *udata)
{
  (void) udata;
  if (!strcmp(req->resource,"/"))
  {
    resp->code = ICE_SERVE_OK;
    maybe_update_current_status_text();
    pthread_rwlock_rdlock(&current_status_text_lock);
    resp->content = current_status_text;
    resp->content_length = current_status_text_len;
    resp->content_type = "application/json";
    resp->free_fun = unlock_wrapper;
    resp->free_fun_arg = &current_status_text_lock;
  }
  return 0;
}

static void * sck_thread(void *vctx)
{
  ice_serve_ctx_t * ctx = (ice_serve_ctx_t*) vctx;
  ice_serve_run(ctx);
  ice_serve_destroy(ctx);
  return NULL;
}

static void * wri_thread(void* v) 
{
  (void) v; 
  time_t start_time = time(0); 
  time_t last_print_out = start_time; 
  time_t last_current_state = start_time;


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

  if (!bigbuf) 
  {
    fail("Could not allocate buffer... that's not good!"); 
    return 0; 
  }

  int num_events = 0;
  int num_force = 0;
  int num_events_last_cycle = 0;
  int num_events_this_cycle = 0; 
  int last_cycle_length = 0;

  int ds_i = 0; 

  //let's make the output directories
  mkdir_if_needed(output_dir); 

  make_dirs_for_output(output_dir); 

  //open the file list 
  sprintf(bigbuf,"%s/aux/acq-file-list.txt", output_dir); 
  file_list = fopen(bigbuf, "w"); 
  add_to_file_list(bigbuf); 

  //open the run info and start filling it in
  sprintf(bigbuf,"%s/aux/runinfo.txt", output_dir); 
  runinfo = fopen(bigbuf,"w"); 
  if (runinfo) 
  {
    add_to_file_list(bigbuf); 
    fprintf(runinfo, "STATION = %d\n", station_number);
    fprintf(runinfo, "RUN = %d\n", run_number);
    fprintf(runinfo, "RUN-START-TIME =  %ld.%09ld\n",precise_start_time.tv_sec, precise_start_time.tv_nsec); 
    fprintf(runinfo, "LIBRNO-G-GIT-HASH = %s\n", rno_g_get_git_hash()); 
    fprintf(runinfo, "RNO-G-ICE-SOFTWARE-GIT-HASH = %s\n", get_ice_software_git_hash()); 
    fprintf(runinfo, "FREE-SPACE-MB-OUTPUT-PARTITION = %f\n", output_partition_free); 
    fprintf(runinfo, "FREE-SPACE-MB-RUNFILE-PARTITION = %f\n", runfile_partition_free); 
    
    //write down radiant info to runinfo 
    uint8_t fwmajor, fwminor, fwrev, fwyear, fwmon, fwday; 
    radiant_get_fw_version(radiant, DEST_FPGA,  &fwmajor, &fwminor, &fwrev, &fwyear, &fwmon, &fwday); 
    fprintf(runinfo, "RADIANT-FWVER = %02u.%02u.%02u\n", fwmajor, fwminor, fwrev); 
    fprintf(runinfo, "RADIANT-FWDATE = 20%02u-%02u.%02u\n", fwyear, fwmon, fwday); 

    radiant_get_fw_version(radiant, DEST_MANAGER,  &fwmajor, &fwminor, &fwrev, &fwyear, &fwmon, &fwday); 
    fprintf(runinfo, "RADIANT-BM-FWVER = %02u.%02u.%02u\n", fwmajor, fwminor, fwrev); 
    fprintf(runinfo, "RADIANT-BM-FWDATE = 20%02u-%02u.%02u\n", fwyear, fwmon, fwday); 

    uint16_t sample_rate= radiant_get_sample_rate(radiant); 
    fprintf(runinfo, "RADIANT-SAMPLERATE = %u\n", sample_rate); 
   

    uint16_t flower_fwyear;
    if (flower) 
    {
      flower_get_fwversion(flower, &fwmajor, &fwminor, &fwrev, &flower_fwyear, &fwmon, &fwday); 
      fprintf(runinfo, "FLOWER-FWVER = %02u.%02u.%02u\n", fwmajor, fwminor, fwrev); 
      fprintf(runinfo, "FLOWER-FWDATE = %02u-%02u.%02u\n", flower_fwyear, fwmon, fwday); 
    }
    else
    {
      fprintf(runinfo, "FLOWER-FWVER = 0.0.0\n"); 
      fprintf(runinfo, "FLOWER-FWDATE = 0000-00.00\n"); 
    }
    fflush(runinfo); 
  }
  else
  {
    fprintf(stderr,"Yikes, couldn't write to %s\n", bigbuf); 
  }

  //save comment 
  sprintf(bigbuf,"%s/aux/comment.txt",output_dir); 
  FILE * fcomment = fopen(bigbuf,"w"); 
  if (fcomment) 
  {
    fprintf(fcomment, cfg.output.comment); 
    if (!flower) fprintf(fcomment, " !!FLOWER NOT DETECTED!!"); 
    fclose(fcomment); 
    add_to_file_list(bigbuf); 
  }
  else
  {
    fprintf(stderr,"Yikes, couldn't write to %s\n", bigbuf); 
  }

  //write gain codes
  write_gain_codes(bigbuf); 


  //now let's dump the configuration file to the cfg dir 
  sprintf(bigbuf,"%s/cfg/acq.cfg", output_dir); 
  FILE * of = fopen(bigbuf,"w"); 
  if (!of) 
  {
    fprintf(stderr,"Could not open %s\n", bigbuf); 
  }
  else
  {
    dump_acq_config(of,&cfg); 
    fclose(of); 
    add_to_file_list(bigbuf); 
  }

  //now we can release the cfg lock, for a bit 
  pthread_rwlock_unlock(&cfg_lock); 

  //if we have pedestals, write them out 
  if (pedestals) 
  {
    snprintf(bigbuf,bigbuflen,"%s/pedestals.dat.gz", output_dir); 
    rno_g_file_handle_t ped_h; 
    rno_g_init_handle(&ped_h, bigbuf,"w");  
    rno_g_pedestal_write(ped_h, pedestals); 
    rno_g_close_handle(&ped_h); 
    add_to_file_list(bigbuf); 
  }

  if (did_bias_scan) 
  {
    snprintf(bigbuf,bigbuflen,"%s/bias_scan.dat.gz", output_dir); 
    if (!mv_file(bias_scan_tmpfile, bigbuf))
    {
      add_to_file_list(bigbuf); 
    }
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
      if (acq_item.hd.trigger_type & RNO_G_TRIGGER_SOFT) num_force++;
      num_events++;
      if (!pthread_rwlock_trywrlock(&current_status_lock))
      {
        current_status.num_events = num_events;
        current_status.num_force_events = num_force;
        current_status.num_events_last_cycle = num_events_last_cycle;
        current_status.last_cycle_length = last_cycle_length;
        clock_gettime(CLOCK_REALTIME,&current_status.event_last_updated);
        pthread_rwlock_unlock(&current_status_lock);
      }

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
      printf("-------S%d/R%d after %u seconds-----------\n", station_number, run_number, (unsigned) (now - start_time)); 
      printf("  total events written: %d\n", num_events); 
      printf("  write rate:  %g Hz\n", (num_events == 0) ? 0. :  ((float) num_events_this_cycle) / (now - last_print_out)); 
      printf("  write buffer occupancy: %d/%d\n", acq_occupancy , cfg.runtime.acq_buf_size); 


      num_events_last_cycle = num_events_this_cycle;
      last_cycle_length = now-last_print_out;

      if (pthread_rwlock_trywrlock(&current_status_lock))
      {
        clock_gettime(CLOCK_REALTIME,&current_status.event_last_updated);
        current_status.num_events_last_cycle = num_events_last_cycle;
        current_status.last_cycle_length = last_cycle_length;
        pthread_rwlock_unlock(&current_status_lock);
      }
      num_events_this_cycle = 0; 
      rno_g_daqstatus_t tmp_ds;
      pthread_rwlock_rdlock(&ds_lock);
      memcpy(&tmp_ds,ds, sizeof(tmp_ds));
      pthread_rwlock_unlock(&ds_lock); 
      rno_g_daqstatus_dump(stdout, ds); 
      last_print_out = now;
    }

    if (cfg.output.current_state_interval > 0 && now - last_current_state > cfg.output.current_state_interval)
    {

      FILE * fcurrent = fopen(tmp_current_state_file,"w");
      maybe_update_current_status_text();
      pthread_rwlock_rdlock(&current_status_text_lock);
      fwrite(current_status_text,current_status_text_len,1,fcurrent);
      pthread_rwlock_unlock(&current_status_text_lock);
      fclose(fcurrent);
      rename(tmp_current_state_file,cfg.output.current_state_location);
    }

    //feed the watchdog in the write thread, since it might wait longer at the end
    if (now - last_watchdog > 10) 
    {
      feed_watchdog(&now); 
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

    else
    {
      //do we need to grab the cfg rd lock here? not sure it matters. 

      if (have_data) 
      {
        if ( !wf_file_name || 
             (cfg.output.max_kB_per_file > 0  &&  wf_file_size >= cfg.output.max_kB_per_file) ||
             (cfg.output.max_events_per_file > 0 && wf_file_N >= cfg.output.max_events_per_file) ||
             (cfg.output.max_seconds_per_file > 0 && now - wf_file_time >= cfg.output.max_seconds_per_file ) )
        {
          if (wf_file_name) do_close(wf_handle, wf_file_name); 

           snprintf(bigbuf,bigbuflen,"%s/waveforms/%06u.wf.dat.gz%s", output_dir, acq_item.hd.event_number, tmp_suffix ); 
           wf_handle.type = RNO_G_GZIP; 
           wf_handle.handle.gz = gzopen(bigbuf,"w"); 
           gzsetparams(wf_handle.handle.gz,3,Z_FILTERED); 

           wf_file_name = strdup(bigbuf); 
           wf_file_size = 0; 
           wf_file_N = 0; 
           wf_file_time = now; 


           if (hd_file_name) do_close(hd_handle, hd_file_name); 
           snprintf(bigbuf,bigbuflen,"%s/header/%06u.hd.dat.gz%s", output_dir, acq_item.hd.event_number, tmp_suffix ); 
           hd_handle.type = RNO_G_GZIP; 
           hd_handle.handle.gz = gzopen(bigbuf,"w"); 
           hd_file_name = strdup(bigbuf); 
        }

        wf_file_size += rno_g_waveform_write(wf_handle, &acq_item.wf); 
        rno_g_header_write(hd_handle, &acq_item.hd); 
        wf_file_N++; 
      }

      if (have_status) 
      {
        if ( !ds_file_name || 
             (cfg.output.max_kB_per_file > 0  &&  ds_file_size >= cfg.output.max_kB_per_file) ||
             (cfg.output.max_daqstatuses_per_file > 0 && ds_file_N >= cfg.output.max_daqstatuses_per_file) ||
             (cfg.output.max_seconds_per_file > 0 && now - ds_file_time >= cfg.output.max_seconds_per_file ) )
       
        {

      
          if (ds_file_name) do_close(ds_handle, ds_file_name); 
          snprintf(bigbuf,bigbuflen,"%s/daqstatus/%05d.ds.dat.gz%s", output_dir, ds_i, tmp_suffix ); 
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


void fail(const char * why)
{
  fprintf(stderr,"FAIL!: %s\n", why); 
  please_stop(); 
}


static int initial_setup() 
{

  clock_gettime(CLOCK_REALTIME, &precise_start_time); 

  memcpy(&current_status.run_start, &precise_start_time, sizeof(precise_start_time));


  /** Initialize config lock and try to read the config */ 
  pthread_rwlock_init(&cfg_lock,NULL); 
  read_config();

  pthread_rwlock_init(&current_status_lock,NULL); 
  pthread_rwlock_init(&current_status_text_lock,NULL); 

  // initialize the server and start the socket thread

  if (cfg.output.current_state_port)
  {
    ice_serve_setup_t s = {.port = cfg.output.current_state_port, .handler = request_handler, .exit_sentinel = &quit};
    ice_serve_ctx_t * ctx = ice_serve_init(&s);
    if (ctx)
    {
      pthread_create(&the_sck_thread,NULL,sck_thread,ctx);
    }
  }


  fill_current_status_sys();
  // Check that there is sufficient free space before proceeding any farther; 


  while ( cfg.output.min_free_space_MB_runfile_partition && current_status.runfile_partition_free  < cfg.output.min_free_space_MB_runfile_partition) 
  {
    fprintf(stderr,"Insufficient free space on runfile partition (%f MB free,  %d). Waiting ~300 seconds before trying again\n", runfile_partition_free, cfg.output.min_free_space_MB_runfile_partition); 

    //avoid getting killed by watchdog
    for (int i = 0; i < 15; i++) 
    {
      sleep(20); 
      fill_current_status_sys();
      feed_watchdog(0); 
    }
  }

  while ( cfg.output.min_free_space_MB_output_partition && current_status.output_partition_free  < cfg.output.min_free_space_MB_output_partition) 
  {
    fprintf(stderr,"Insufficient free space on output partition (%f MB free,  %d). Waiting ~300 seconds before trying again\n", output_partition_free, cfg.output.min_free_space_MB_output_partition); 

    //avoid getting killed by watchdog
    for (int i = 0; i < 15; i++) 
    {
      sleep(20); 
      feed_watchdog(0); 
      fill_current_status_sys();
    }
  }


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

  //Read the runfile,
  FILE * frun = fopen(cfg.output.runfile,"r"); 
  if (!frun) 
  {
    fprintf(stderr,"NO RUN FILE FOUND at %s, setting run to 0\n", cfg.output.runfile); 
    run_number = 0; 
    asprintf(&output_dir, "%s/run%d/", cfg.output.base_dir, run_number); 
  }
  else
  {
    fscanf(frun,"%d\n", &run_number); 

    // make sure run number is positive
    if (run_number < 0) 
    {
      fprintf(stderr,"NEGATIVE RUN NUMBER FOUND (%d), aborting.\n", run_number); 
      fclose(frun); 
      return 1; 
    }

    //our output dir is going to be the base_dir + run%d/ 
    asprintf(&output_dir, "%s/run%d/", cfg.output.base_dir, run_number); 


    //avoid overwriting rundir (note that run 0 may still be overwritten if there is no run file, but that's ok.) 
    if (!cfg.output.allow_rundir_overwrite) 
    {
      //dir exists! 
      while (!access(output_dir, F_OK))
      {
        fprintf(stderr,"DIR %s exists, incrementing run number\n", output_dir); 
        run_number++; 
        free(output_dir); 
        asprintf(&output_dir, "%s/run%d/", cfg.output.base_dir, run_number); 
      }
    }
  }
  pthread_rwlock_wrlock(&current_status_lock);
  current_status.current_run = run_number;
  pthread_rwlock_unlock(&current_status_lock);




  //make sure calpulser is turned off (in case we didn't exit cleanly!) since we don't want it on during pedestal taking and such 
  rno_g_cal_disable_no_handle(cfg.calib.gpio); 

  int need_to_copy_radiant_thresholds = 1; 
  int need_to_copy_lt_thresholds = 1; 
  //open the shared status file, if it's there. 
  //need to do this before opening the radiant/flower since we need to laod thresholds, potentially 
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

       if (cfg.lt.thresholds.load_from_threshold_file && file_size == sizeof(rno_g_daqstatus_t)) 
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
      ds->radiant_thresholds[i] = cfg.radiant.thresholds.initial[i] * 16777215/2.5;   
    }
  }

  if (need_to_copy_lt_thresholds) 
  { 
    for (int i = 0;  i <  RNO_G_NUM_LT_CHANNELS; i++) 
    {
      ds->lt_trigger_thresholds[i] = cfg.lt.thresholds.initial[i]; 
      ds->lt_servo_thresholds[i] = 
        clamp(cfg.lt.thresholds.initial[i] * cfg.lt.servo.servo_thresh_frac + cfg.lt.servo.servo_thresh_offset, 0, 255); 
    }
  }
  pthread_rwlock_init(&ds_lock,NULL); 



  //initialize the radiant lock
  pthread_rwlock_init(&radiant_lock,NULL); 

  int nattempts = 0;
  //open the radiant
  do
  {
    radiant  = radiant_open(cfg.radiant.device.spi_device,
                             cfg.radiant.device.uart_device,
                             cfg.radiant.device.poll_gpio,
                             cfg.radiant.device.spi_enable_gpio);

    if (!radiant)
    {
      fprintf(stderr,"COULD NOT OPEN RADIANT. Attemping to drop caches in case kernel fragmentation is the issue.");
      if (nattempts++ > 3)
      {
        fprintf(stderr,"Giving up...\n");
        return 1;
      }
      sleep(1);
      system("/rno-g/bin/bbb-drop-caches");
    }

    if (radiant && nattempts > 0)
    {
      fprintf(stderr,"Ok, we could open it! Yay!\n");
    }
  } while (!radiant);


  //open the flower before doing radiant_initial_setup so we fail faster
  pthread_rwlock_init(&flower_lock,NULL);

  flower = flower_open(cfg.lt.device.spi_device, cfg.lt.device.spi_enable_gpio); 
  if (!flower && cfg.lt.device.required) 
  {
    fprintf(stderr,"COULD NOT OPEN FLOWER. Waiting 20 seconds before quitting"); 
    sleep(20);
    return 1; 
  }

  feed_watchdog(0); 

  //intitial configure of the radiant, bail if can't open
  if (radiant_initial_setup()) return 1; 
  feed_watchdog(0); 


  //and the flower, bail if can't open  and required 
  if (flower_initial_setup() && cfg.lt.device.required) return 1; 
  feed_watchdog(0); 

  //update the run file 
  if (frun) 
  {
    fclose(frun); 


    char * tmp_run_file = 0;
    asprintf(&tmp_run_file, "%s.tmp", cfg.output.runfile); 
    frun = fopen(tmp_run_file,"w"); 
    if (!frun) 
    {
      fprintf(stderr,"Could not open temporary run file: %s\n", tmp_run_file); 
      return 1; 
    }
    if ( 0 > fprintf(frun,"%d\n", run_number+1) || 0 != fclose(frun))
    {
      fprintf(stderr,"Problem writing temporary run file %s\n", tmp_run_file); 
      return 1; 
    }
    if (rename(tmp_run_file, cfg.output.runfile))
    {
      fprintf(stderr,"Problem moving %s to %s\n", tmp_run_file, cfg.output.runfile); 
      return 1; 
    }
    free(tmp_run_file); 
  }

 
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
  feed_watchdog(0); 

  //hold the cfg lock until the write thread is done writing the config 
  pthread_rwlock_rdlock(&cfg_lock); 

  pthread_create(&the_wri_thread,NULL, wri_thread, NULL); 

  return 0; 
}




int please_stop()
{
  printf("Stopping...\n"); 
  quit = 1; 
  return 0; 
}




int main(int nargs, char ** args) 
{
   if (nargs >1 ) cfgpath = args[1]; 

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
     fill_current_status_sys();

     if (cfg.output.min_free_space_MB_output_partition > 0 ) 
     {
       double MBfree = current_status.output_partition_free;
       if (MBfree < cfg.output.min_free_space_MB_output_partition) 
       {
         fprintf(stderr,"Output partition free space is just %f MB, smaller than minimum %d MB\n", MBfree, cfg.output.min_free_space_MB_output_partition); 
         please_stop(); 
         continue; 
       }
     }

     clock_gettime(CLOCK_MONOTONIC_COARSE,&now); 
     if (now.tv_sec - start_time.tv_sec > cfg.output.seconds_per_run)
     {
       please_stop(); 
     }
     usleep(1e6);
     sched_yield(); 
   }

  return teardown(); 
}

int teardown() 
{
  pthread_join(the_acq_thread,0);
  pthread_join(the_mon_thread,0);
  pthread_join(the_wri_thread,0);

  //no need to join the sck thread.

  //disable the trigger OVLD
  radiant_trigger_enable(radiant,0,0); 
  radiant_labs_stop(radiant); 
  radiant_close(radiant); 
  if (flower) 
    flower_close(flower); 
  fclose(file_list); 
  struct timespec end_time; 
  clock_gettime(CLOCK_REALTIME, &end_time); 
  if (runinfo) 
  {
    fprintf(runinfo,"RUN-END-TIME = %ld.%09ld\n", end_time.tv_sec, end_time.tv_nsec); 
    fclose(runinfo); 
  }


  //turn off the calpulser on teardown, if it's on?  
  if (calpulser) 
  {
    if (cfg.calib.turn_off_at_exit)
    {
      rno_g_cal_disable(calpulser); 
    }
    rno_g_cal_close(calpulser); 
  }

  if (shared_ds_fd) 
  {
    munmap(ds,sizeof(rno_g_daqstatus_t)); 
    close(shared_ds_fd); 
  }

  return 0; 
}

// you should be holding a flower lock while calling this
int flower_update_pps_offset() 
{
  float wanted_delay = cfg.lt.trigger.pps_trigger_delay; 


  // clamp to a second
  if (fabs(wanted_delay) >= 1e6) wanted_delay =   (wanted_delay*1e-6 - ((int) (wanted_delay*1e-6)))*1e6;

  int delay_cycles = round(wanted_delay * delay_clock_estimate/1e6); 
  if (delay_cycles < 0) delay_cycles += delay_clock_estimate; 
  return flower_set_delayed_pps_delay(flower,delay_cycles);
}
