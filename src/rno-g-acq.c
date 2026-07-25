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
 *         * readers hold this when using the config for something, but should release sometimes
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
#include <zlib.h>
#include <inttypes.h>
#include <math.h>
#include <errno.h>

#include <systemd/sd-daemon.h>

#ifdef ON_DIDAQ

#include "didaq.h"
#include "rno-g-didaq.h"

#else

#include "radiant.h"
#include "flower.h"

/* RADIANT threshold DAC: 24-bit code (2^24-1) over a 2.5V full-scale range */
#define RADIANT_THRESHOLD_COUNTS_PER_VOLT (16777215/2.5)
#endif

#include "rno-g.h"
#include "rno-g-cal.h"
#include "ice-config.h"
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
static char * cfgpath = NULL;

/*read-write lock for the config */
static pthread_rwlock_t cfg_lock;

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

//temporary buffer (TODO: replace all asprintf with this...)
static int bigbuflen = 0;
static char * bigbuf = 0;

/** This is set to 1 when it's time to quit.*/
static volatile int quit = 0;
static volatile int cfg_reread = 0;

/** Shared DAQ status struct.
 *
 *  main() mmaps it onto cfg.runtime.status_shmem_file (if configured, else
 *  a private calloc'd buffer) and seeds it with the initial radiant/flower
 *  thresholds. From then on mon_thread owns it exclusively: its servo
 *  helpers (radiant_flower_servo/didaq_servo) update the thresholds from
 *  live scalers, it's snapshotted into mon_buffer for periodic daqstatus
 *  readout, and msync()'d so external readers (e.g. another process mmap'ing
 *  or rsync'ing status_shmem_file) see current values promptly. wri_thread
 *  only reads the mon_buffer snapshot (to write it to disk); it does not
 *  touch ds.
 */
static rno_g_daqstatus_t * ds = 0;

//File descriptor for daqstatus shared mem file
static int shared_ds_fd;

//Size the daqstatus shared mem file had when we opened it (cached from setup_run_and_daqstatus)
static size_t shared_ds_file_size;

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

static int calpulser_configure();
static int teardown();
static int please_stop();
static int add_to_file_list(const char * path);
static void feed_watchdog(time_t * now) ;
static void start_threads();
static float clamp(float val, float min, float max);
static void servo_record_value(float * value, float * last_value, float * error, float * last_error,
                                float * sum_error, float new_value, float goal, float max_sum_err);
static double servo_pid_step(double P, double I, double D, float error, float sum_error, float last_error);

static struct timespec precise_start_time;
static struct timespec precise_acq_time;
static struct timespec precise_stop_time;

static uint32_t delay_clock_estimate = 10000000;

#ifdef ON_DIDAQ


static didaq_dev_t * didaq = 0;

static int didaq_configure();
static int didaq_initial_setup();

/* Mutex guarding all access to the didaq SPI bus/handle. A plain mutex (not a
 * rwlock like radiant_lock/flower_lock) because every operation on didaq --
 * event readout, scaler reads, threshold writes, trigger reconfiguration --
 * touches the same physical SPI bus and the same unprotected didaq_dev_t
 * scheduling state, so there is no "read-only, safe to share" case to give a
 * rwlock's shared mode any value; every access needs to be exclusive. */
static pthread_mutex_t didaq_lock;

//gain codes and measured RMS from the last auto-gain equalization, one per channel (mirrors
//flower_codes/flower_rms); written to disk at each run start by write_gain_codes_didaq()
static uint8_t didaq_gain_codes[RNO_G_NUM_RADIANT_CHANNELS];
static float didaq_gain_rms[RNO_G_NUM_RADIANT_CHANNELS];

#else
///// Radiant & Flower specific definitions /////

/*read-write lock for cofiguring the radiant */
static pthread_rwlock_t radiant_lock;

/*read-write lock for cofiguring the flower */
static pthread_rwlock_t flower_lock;

/** radiant pedestals*/
static rno_g_pedestal_t * pedestals = 0;

//File descriptor for pedestal shared mem file
static int pedestal_fd;

/** radiant handle*/
static radiant_dev_t * radiant = 0;
static uint32_t radiant_trig_chan = 0;

/** flower handle */
static flower_dev_t * flower = 0;

static uint8_t flower_codes[RNO_G_NUM_LT_CHANNELS];
static float flower_rms[RNO_G_NUM_LT_CHANNELS];

static uint8_t *flower_waveforms_data;
static uint8_t *flower_waveforms[RNO_G_NUM_LT_CHANNELS];
static int flower_waveforms_len;

static int radiant_configure();
static int flower_configure();
static int flower_update_pps_offset();
#endif

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
  //yet at startup)
  if (!first_time)
  {
    char * ofname;
    time_t now;
    time(&now);
    asprintf(&ofname,"%s/cfg/acq.%d.%lu.cfg", output_dir, config_counter, now);
    FILE * of = fopen(ofname,"w");
    dump_acq_config(of, &cfg);
    fclose(of);
    add_to_file_list(ofname);
    free(ofname);
  }

  //incremente the config counter (so threads know config may have changed)
  config_counter++;
  //release the write lock
  pthread_rwlock_unlock(&cfg_lock);

  //apply new configuration to radiant/flower if they have changed
  if (!first_time)
  {

#ifdef ON_DIDAQ

    if (memcmp(&old_cfg.didaq, &cfg.didaq, sizeof(cfg.didaq)))
    {
      didaq_configure();
    }

#else

    if (memcmp(&old_cfg.radiant, &cfg.radiant, sizeof(cfg.radiant)))
    {
      radiant_configure();
    }

    if (memcmp(&old_cfg.lt, &cfg.lt, sizeof(cfg.lt)))
    {
      flower_configure();
    }

#endif

    if (memcpy(&old_cfg.calib, &cfg.calib, sizeof(cfg.calib)))
    {
      calpulser_configure();
    }
  }

}

static int add_to_file_list(const char *path)
{
  flock(file_list_fd, LOCK_EX);
  fprintf(file_list,"%s\n", path);
  fflush(file_list);
  flock(file_list_fd, LOCK_UN);
  return 0;
}

//Feeds the systemd watchdog
static void feed_watchdog(time_t * now)
{
  time_t when;
  if (!now) time(&when) ;
  else when = *now;
  sd_notify(0,"WATCHDOG=1");
  last_watchdog = when;
}


#ifdef ON_DIDAQ

static int open_and_setup_didaq()
{

  didaq_setup_t setup = {
    .spi_device = cfg.didaq.device.spi_name,
    .spi_en_gpio_label = cfg.didaq.device.spi_en_label,
    .trig_ready_gpio_label = cfg.didaq.device.trig_ready_gpio_label,
  };

  didaq = didaq_open(&setup);

  if (!didaq)
  {
    fprintf(stderr, "COULD NOT OPEN DIDAQ. Giving up.");
    return 1;
  }

  pthread_mutex_init(&didaq_lock, NULL);

  feed_watchdog(0);
  if (didaq_initial_setup())
    return 1;

  feed_watchdog(0);
  return 0;
}

static int didaq_initial_setup() {
  // Runs once at startup (single thread, no need for a lock?)
  if (!didaq) return -1;

  pthread_mutex_lock(&didaq_lock);
  // //do the auto gain if asked to (mirrors flower_initial_setup()'s auto-gain block)
  // if (cfg.didaq.gain.auto_gain)
  // {
  //   //disable triggers momentarily so they don't fire spuriously during equalization
  //   didaq_trigger_setup_t disabled = {0};
  //   didaq_configure_trigger(didaq, &disabled);
  //   didaq_equalize(didaq, cfg.didaq.gain.target_rms, didaq_gain_codes, DIDAQ_EQUALIZE_VERBOSE, didaq_gain_rms);
  // }
  didaq_reset_acq(didaq);

  pthread_mutex_unlock(&didaq_lock);

  return didaq_configure();
}

static int didaq_configure()
{

  pthread_mutex_lock(&didaq_lock);
  pthread_rwlock_rdlock(&cfg_lock);

  //seed thresholds from config, unless we already have valid ones from the shmem file. This runs
  //on every (re)configure -- not just the one at startup via didaq_initial_setup() -- so that a
  //live config reread (SIGUSR1) with load_from_threshold_file turned off actually takes effect,
  //instead of ds's thresholds silently staying whatever they were before the reread.
  int need_to_copy_didaq_coin_thresholds_from_cfg = !(
    cfg.didaq.thresholds.coinc.load_from_threshold_file &&
    shared_ds_file_size == sizeof(rno_g_daqstatus_t));
  if (need_to_copy_didaq_coin_thresholds_from_cfg)
  {
    for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++)
    {
      ds->didaq_coin_thresholds[i] = cfg.didaq.thresholds.coinc.initial[i];
    }
  }

  int need_to_copy_didaq_phased_thresholds_from_cfg = !(
    cfg.didaq.thresholds.phased.load_from_threshold_file &&
    shared_ds_file_size == sizeof(rno_g_daqstatus_t));
  if (need_to_copy_didaq_phased_thresholds_from_cfg)
  {
    // cfg.didaq.thresholds.phased.initial[] has RNO_G_NUM_LT_BEAMS slots (for symmetry with
    // FLOWER's config), but DIDAQ hardware only has RNO_G_NUM_DIDAQ_BEAMS beams; the rest are unused.
    for (int i = 0; i < RNO_G_NUM_DIDAQ_BEAMS; i++)
    {
      ds->didaq_phased_trigger_thresholds[i] = cfg.didaq.thresholds.phased.initial[i];
      ds->didaq_phased_servo_thresholds[i] = clamp(
        cfg.didaq.thresholds.phased.initial[i] * cfg.didaq.servo.phased.servo_thresh_frac +
        cfg.didaq.servo.phased.servo_thresh_offset, 0, 65535);
    }
  }

  didaq_coin_thresholds_t coin_th;
  memcpy(coin_th.coin_thresholds, ds->didaq_coin_thresholds, sizeof(coin_th.coin_thresholds));

  didaq_phased_thresholds_t phased_th;
  memcpy(phased_th.beam_trig_thresholds, ds->didaq_phased_trigger_thresholds, sizeof(phased_th.beam_trig_thresholds));
  memcpy(phased_th.beam_servo_thresholds, ds->didaq_phased_servo_thresholds, sizeof(phased_th.beam_servo_thresholds));

  didaq_set_thresholds(didaq, &phased_th, &coin_th);

  didaq_trigger_setup_t trig = {
    .enable_ext = cfg.didaq.trigger.ext.enabled,
    .enable_pps = cfg.didaq.trigger.pps.enabled,
    .phased = {
      .enable = cfg.didaq.trigger.phased.enable,
      .enable_readout = cfg.didaq.trigger.phased.enable_readout,
      .require_consecutive_windows = cfg.didaq.trigger.phased.require_consecutive,
      .divide_by_2 = cfg.didaq.trigger.phased.divide_by_2,
      .chan_exclude_mask = cfg.didaq.trigger.phased.channel_exclude_mask,
      .beam_exclude_mask = cfg.didaq.trigger.phased.beam_exclude_mask
    }
  };

  for (int i = 0; i < DIDAQ_NUM_COINC; i++)
  {
    trig.coinc[i].enable = cfg.didaq.trigger.coinc[i].enable;
    trig.coinc[i].quad_mode = cfg.didaq.trigger.coinc[i].quad_mode;
    trig.coinc[i].enable_readout = cfg.didaq.trigger.coinc[i].enable_readout;
    trig.coinc[i].num_required = cfg.didaq.trigger.coinc[i].num_required;
    trig.coinc[i].coinc_window = cfg.didaq.trigger.coinc[i].window;
    trig.coinc[i].channel_exclude_mask = cfg.didaq.trigger.coinc[i].exclude_mask;
  }

  int ret = didaq_configure_trigger(didaq, &trig);

  pthread_rwlock_unlock(&cfg_lock);
  pthread_mutex_unlock(&didaq_lock);

  return ret;
}

/** Write out the gain codes/RMS from the last auto-gain equalization (mirrors
 *  write_gain_codes_flower()). Called once at the start of each run, so the
 *  codes computed once at daemon startup (didaq_initial_setup()) get logged
 *  into every run's aux directory for provenance.
 */
static int write_gain_codes_didaq(char * buf)
{
  if (!didaq) return -1;
  static int gain_codes_counter = 0;
  time_t now;
  time(&now);

  sprintf(buf, "%s/aux/didaq_gain_codes.%d.txt", output_dir, gain_codes_counter++);
  FILE * of = fopen(buf,"w");
  if (!of) return 1;
  fprintf(of,"# DIDAQ gain codes, station=%d, run=%d,  time=%lu\n", station_number, run_number, now);
  for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++)
  {
    fprintf(of, "%u%s", didaq_gain_codes[i], i < RNO_G_NUM_RADIANT_CHANNELS -1 ? " " : "\n");
  }
  for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++)
  {
    fprintf(of, "%.3f%s", didaq_gain_rms[i], i < RNO_G_NUM_RADIANT_CHANNELS -1 ? " " : "\n");
  }
  fclose(of);
  add_to_file_list(buf);
  return 0;
}

/** Per-beam state for the phased-array threshold servo (RNO_G_NUM_DIDAQ_BEAMS
 *  beams), mirroring flower_phased_servo_state_t.
 */
typedef struct didaq_phased_servo_state
{
  float value[RNO_G_NUM_DIDAQ_BEAMS];
  float last_value[RNO_G_NUM_DIDAQ_BEAMS];
  float error[RNO_G_NUM_DIDAQ_BEAMS];
  float last_error[RNO_G_NUM_DIDAQ_BEAMS];
  float sum_error[RNO_G_NUM_DIDAQ_BEAMS];
} didaq_phased_servo_state_t;

/** Per-channel state for the coincidence-trigger threshold servo
 *  (RNO_G_NUM_RADIANT_CHANNELS channels), mirroring flower_coinc_servo_state_t.
 *  Unlike RADIANT's coincidence servo (radiant_coinc_servo_state_t), no
 *  multi-period rolling window is needed: DIDAQ's coincidence-singles scaler
 *  is already a plain per-second rate (no prescaler/period bookkeeping
 *  needed, unlike RADIANT's radiant_scalers -- see radiant_raw_coinc_scaler()).
 */
typedef struct didaq_coinc_servo_state
{
  float value[RNO_G_NUM_RADIANT_CHANNELS];
  float last_value[RNO_G_NUM_RADIANT_CHANNELS];
  float error[RNO_G_NUM_RADIANT_CHANNELS];
  float last_error[RNO_G_NUM_RADIANT_CHANNELS];
  float sum_error[RNO_G_NUM_RADIANT_CHANNELS];
} didaq_coinc_servo_state_t;

/** Update each channel's servo error state from the latest coincidence-singles
 *  scalers, mirroring FLOWER's update_flower_coinc_servo_state() -- read the
 *  current rate directly (no rolling average) and optionally subtract the
 *  gated variant. Unlike FLOWER, there's no per-channel "fast" (100Hz)
 *  scaler to blend in: DIDAQ's only 100mHz-rate coincidence scalers
 *  (coinc_trig_100mHz/_gated) are per coincidence-group output, not
 *  decomposable per input channel.
 */
static void update_didaq_coinc_servo_state(didaq_coinc_servo_state_t * st, const rno_g_daqstatus_t * ds)
{
  int sub = cfg.didaq.servo.coinc.subtract_gated;

  for (int chan = 0; chan < RNO_G_NUM_RADIANT_CHANNELS; chan++)
  {
    float val = ds->didaq_scalers.coinc_singles_1Hz[chan]
              - sub * ds->didaq_scalers.coinc_singles_1Hz_gated[chan];

    servo_record_value(&st->value[chan], &st->last_value[chan], &st->error[chan], &st->last_error[chan],
                        &st->sum_error[chan], val, cfg.didaq.servo.coinc.scaler_goals[chan], 0);
  }
}

/** Update the phased/beam servo error state from the latest beam scalers.
 *  beam_servo_1Hz updates every second and has no gated counterpart, so it
 *  plays the role of FLOWER's "fast" scaler group; beam_trig_100mHz(_gated)
 *  only updates every 10s (hence the *0.1 to get back to a Hz-comparable
 *  rate) but does have a gated counterpart, so it plays the role of FLOWER's
 *  "slow" (optionally gate-subtracted) group.
 */
static void update_didaq_phased_servo_state(didaq_phased_servo_state_t * st, const rno_g_daqstatus_t * ds)
{
  float fw = cfg.didaq.servo.phased.fast_scaler_weight;
  float sw = cfg.didaq.servo.phased.slow_scaler_weight;
  int sub = cfg.didaq.servo.phased.subtract_gated;
  const float slow_to_hz = 0.1;

  for (int i = 0; i < RNO_G_NUM_DIDAQ_BEAMS; i++)
  {
    float val = fw * ds->didaq_scalers.beam_servo_1Hz[i]
              + sw * slow_to_hz * (ds->didaq_scalers.beam_trig_100mHz[i]
              - sub * ds->didaq_scalers.beam_trig_100mHz_gated[i]);

    servo_record_value(&st->value[i], &st->last_value[i], &st->error[i], &st->last_error[i],
      &st->sum_error[i], val, cfg.didaq.servo.phased.phased_scaler_goals[i], 0);
  }
}

/** Servo/scaler-monitoring logic for the DIDAQ board, the ON_DIDAQ counterpart
 *  of radiant_flower_servo(). DIDAQ has two independent servo loops (unlike
 *  RADIANT+FLOWER's one-per-board split): the per-channel coincidence
 *  threshold servo (RNO_G_NUM_RADIANT_CHANNELS channels, one DAC threshold
 *  per channel, no separate trig/servo split; each channel feeds exactly one
 *  of the RNO_G_NUM_DIDAQ_COINC coincidence triggers -- channels 0-11 feed
 *  coinc[0], 12-23 feed coinc[1]) and the per-beam phased-array threshold
 *  servo (RNO_G_NUM_DIDAQ_BEAMS beams, with a separate trig/servo threshold
 *  pair like FLOWER's phased trigger). Both loops' scalers come off a single
 *  didaq_read_scalers() SPI transaction, so that's only issued once per call
 *  when either loop is due for an update.
 *
 *  NOTE: unlike RADIANT (UART) vs FLOWER/DIDAQ-readout (SPI), DIDAQ's scalers,
 *  threshold-set and event-readout registers all live on the *same* SPI bus
 *  as acq_thread's event readout, and didaq_dev_t has no internal locking of
 *  its own. didaq_lock is a plain mutex (not a rwlock) for exactly this
 *  reason: every access here and in acq_thread/didaq_configure() must be
 *  mutually exclusive, there is no safe-to-share "read-only" case.
 */
static void didaq_servo(double nowf)
{
  static int last_cfg_counter = -1;
  static didaq_phased_servo_state_t phased_state = {0};
  static didaq_coinc_servo_state_t coinc_state = {0};

  static float didaq_coinc_float_thresh[RNO_G_NUM_RADIANT_CHANNELS];
  static float didaq_phased_float_thresh[RNO_G_NUM_DIDAQ_BEAMS];

  static float min_coinc_thresh = 0;
  static float max_coinc_thresh = 0;
  static float min_phased_thresh = 0;
  static float max_phased_thresh = 0;

  static uint32_t coinc_active_chan = 0;
  static uint16_t phased_exclude_beam = 0;

  static double last_scalers_coinc = 0;
  static double last_scalers_phased = 0;
  static double last_servo_coinc = 0;
  static double last_servo_phased = 0;

  int coinc_active = cfg.didaq.trigger.coinc[0].enable || cfg.didaq.trigger.coinc[1].enable;
  int phased_active = cfg.didaq.trigger.phased.enable;

  if (config_counter > last_cfg_counter)
  {
    last_cfg_counter = config_counter;
    memset(&coinc_state, 0, sizeof(coinc_state));
    memset(&phased_state, 0, sizeof(phased_state));

    min_coinc_thresh = cfg.didaq.thresholds.coinc.min;
    max_coinc_thresh = cfg.didaq.thresholds.coinc.max;

    min_phased_thresh = cfg.didaq.thresholds.phased.min;
    max_phased_thresh = cfg.didaq.thresholds.phased.max;

    coinc_active_chan = 0;
    if (cfg.didaq.trigger.coinc[0].enable)
      coinc_active_chan |= (~(uint32_t) cfg.didaq.trigger.coinc[0].exclude_mask) & 0xfff;
    if (cfg.didaq.trigger.coinc[1].enable)
      coinc_active_chan |= ((~(uint32_t) cfg.didaq.trigger.coinc[1].exclude_mask) & 0xfff) << 12;

    phased_exclude_beam = cfg.didaq.trigger.phased.beam_exclude_mask;

    for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++)
      didaq_coinc_float_thresh[i] = ds->didaq_coin_thresholds[i];
    for (int i = 0; i < RNO_G_NUM_DIDAQ_BEAMS; i++)
      didaq_phased_float_thresh[i] = ds->didaq_phased_servo_thresholds[i];
  }

  float diff_scalers_coinc = nowf - last_scalers_coinc;
  float diff_scalers_phased = nowf - last_scalers_phased;

  int need_coinc_scalers = coinc_active && cfg.didaq.servo.coinc.scaler_update_interval
    && cfg.didaq.servo.coinc.scaler_update_interval < diff_scalers_coinc;
  int need_phased_scalers = phased_active && cfg.didaq.servo.phased.scaler_update_interval
    && cfg.didaq.servo.phased.scaler_update_interval < diff_scalers_phased;

  if (need_coinc_scalers || need_phased_scalers)
  {
    pthread_mutex_lock(&didaq_lock);
    didaq_scalers_t raw = {0};
    rno_g_daqstatus_t ds0 = {0};
    int ok = didaq_read_scalers(didaq, &raw) + didaq_read_daqstatus(didaq, &ds0);
    // didaq_dump_scalers(&raw, stdout);

    pthread_mutex_unlock(&didaq_lock);


    if (ok)
    {
      fprintf(stderr, "Problem reading didaq scalers/daqstatus\n");
    }
    else
    {
      memcpy(ds, &ds0, sizeof(ds0));
      // rno_g_didaq_scalers_t mirrors didaq_scalers_t field-for-field, minus
      // the trailing readout_time; a straight memcpy of the smaller struct's
      // size copies exactly the shared fields.
      memcpy(&ds->didaq_scalers, &raw, sizeof(ds->didaq_scalers));

      if (need_coinc_scalers)
      {
        update_didaq_coinc_servo_state(&coinc_state, ds);
        last_scalers_coinc = nowf;
      }

      if (need_phased_scalers)
      {
        update_didaq_phased_servo_state(&phased_state, ds);
        last_scalers_phased = nowf;
      }
    }

  }

  float diff_servo_coinc = nowf - last_servo_coinc;
  float diff_servo_phased = nowf - last_servo_phased;

  int coinc_changed = 0;
  int phased_changed = 0;

  if (coinc_active && cfg.didaq.servo.coinc.enable && cfg.didaq.servo.coinc.servo_interval
      && cfg.didaq.servo.coinc.scaler_update_interval < diff_servo_coinc)
  {
    for (int ch = 0; ch < RNO_G_NUM_RADIANT_CHANNELS; ch++)
    {
      if ((coinc_active_chan & (1u << ch)) == 0) continue;

      double dthreshold = servo_pid_step(cfg.didaq.servo.coinc.P, cfg.didaq.servo.coinc.I,
        cfg.didaq.servo.coinc.D, coinc_state.error[ch], coinc_state.sum_error[ch],
        coinc_state.last_error[ch]);

      didaq_coinc_float_thresh[ch] = clamp(didaq_coinc_float_thresh[ch] + dthreshold,
        min_coinc_thresh, max_coinc_thresh);
      ds->didaq_coin_thresholds[ch] = didaq_coinc_float_thresh[ch];
    }
    coinc_changed = 1;
    last_servo_coinc = nowf;
  }

  if (phased_active && cfg.didaq.servo.phased.enable && cfg.didaq.servo.phased.servo_interval
      && cfg.didaq.servo.phased.scaler_update_interval < diff_servo_phased)
  {
    for (int beam = 0; beam < RNO_G_NUM_DIDAQ_BEAMS; beam++)
    {
      if (phased_exclude_beam & (1u << beam)) continue;

      double d_servo_threshold = servo_pid_step(cfg.didaq.servo.phased.P, cfg.didaq.servo.phased.I,
        cfg.didaq.servo.phased.D, phased_state.error[beam], phased_state.sum_error[beam],
        phased_state.last_error[beam]);

      didaq_phased_float_thresh[beam] = clamp(didaq_phased_float_thresh[beam] + d_servo_threshold,
        min_phased_thresh, max_phased_thresh);
      ds->didaq_phased_servo_thresholds[beam] = didaq_phased_float_thresh[beam];

      ds->didaq_phased_trigger_thresholds[beam] = clamp(
          (didaq_phased_float_thresh[beam] - cfg.didaq.servo.phased.servo_thresh_offset) /
          cfg.didaq.servo.phased.servo_thresh_frac,
          min_phased_thresh, max_phased_thresh);
    }
    phased_changed = 1;
    last_servo_phased = nowf;
  }

  if (coinc_changed || phased_changed)
  {
    didaq_coin_thresholds_t coin_th;
    memcpy(coin_th.coin_thresholds, ds->didaq_coin_thresholds, sizeof(coin_th.coin_thresholds));

    didaq_phased_thresholds_t phased_th;
    memcpy(phased_th.beam_trig_thresholds, ds->didaq_phased_trigger_thresholds, sizeof(phased_th.beam_trig_thresholds));
    memcpy(phased_th.beam_servo_thresholds, ds->didaq_phased_servo_thresholds, sizeof(phased_th.beam_servo_thresholds));

    pthread_mutex_lock(&didaq_lock);
    didaq_set_thresholds(didaq, phased_changed ? &phased_th : NULL, coinc_changed ? &coin_th : NULL);
    pthread_mutex_unlock(&didaq_lock);
  }
}

#else
/** This configures the radiant. It holds the radiant write lock (and acquires the config read lock)*/
static int radiant_configure()
{

  pthread_rwlock_wrlock(&radiant_lock);
  pthread_rwlock_rdlock(&cfg_lock);

  //set thresholds, seeding them from the config if we don't already have valid ones from the
  //shmem file. This runs on every (re)configure -- not just the one at startup via
  //radiant_initial_setup() -- so that a live config reread (SIGUSR1) with
  //load_from_threshold_file turned off actually takes effect, instead of ds's thresholds
  //silently staying whatever they were before the reread.
  int need_to_copy_radiant_thresholds_from_cfg = !(
    cfg.radiant.thresholds.load_from_threshold_file && shared_ds_file_size == sizeof(rno_g_daqstatus_t));
  if (need_to_copy_radiant_thresholds_from_cfg)
  {
    for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++)
    {
      ds->radiant_thresholds[i] = cfg.radiant.thresholds.initial[i] * RADIANT_THRESHOLD_COUNTS_PER_VOLT;
    }
  }
  radiant_set_trigger_thresholds(radiant, 0, RNO_G_NUM_RADIANT_CHANNELS-1, ds->radiant_thresholds);

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


static int write_gain_codes_flower(char * buf)
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
  for (int i = 0; i < RNO_G_NUM_LT_CHANNELS; i++)
  {
    fprintf(of, "%.3f%s", flower_rms[i], i < RNO_G_NUM_LT_CHANNELS -1 ? " " : "\n");
  }
  fclose(of);
  add_to_file_list(buf);
  return 0;
}


/** this configures the flower trigger. It holds the flower write lock (and acquires the config read lock)*/
static int flower_configure()
{
  if (!flower) return -1;

  pthread_rwlock_wrlock(&flower_lock);
  pthread_rwlock_rdlock(&cfg_lock);

  //if we don't already have valid thresholds loaded from the shmem file, seed them from the
  //config. This runs on every (re)configure -- not just the one at startup via
  //flower_initial_setup() -- so that a live config reread (SIGUSR1) with load_from_threshold_file
  //turned off actually takes effect, instead of ds's thresholds silently staying whatever they
  //were before the reread.
  int need_to_copy_lt_thresholds_from_cfg = !(
    cfg.lt.thresholds.load_from_threshold_file && shared_ds_file_size == sizeof(rno_g_daqstatus_t));
  if (need_to_copy_lt_thresholds_from_cfg)
  {
    for (int i = 0;  i <  RNO_G_NUM_LT_CHANNELS; i++)
    {
      ds->lt_trigger_thresholds[i] = cfg.lt.thresholds.initial_coinc_thresholds[i];
      ds->lt_servo_thresholds[i] =
        clamp(cfg.lt.thresholds.initial_coinc_thresholds[i] * cfg.lt.servo.servo_thresh_frac +
          cfg.lt.servo.servo_thresh_offset, 0, 255);
    }

    for (int i = 0;  i <  RNO_G_NUM_LT_BEAMS; i++)
    {
      ds->lt_phased_trigger_thresholds[i] = cfg.lt.thresholds.initial_phased_thresholds[i];
      ds->lt_phased_servo_thresholds[i] =
        clamp(cfg.lt.thresholds.initial_phased_thresholds[i] * cfg.lt.servo.phased_servo_thresh_frac +
          cfg.lt.servo.servo_thresh_offset, 0, 4095);
    }
  }

  flower_set_coinc_thresholds(flower,  ds->lt_trigger_thresholds, ds->lt_servo_thresholds, 0xf);
  flower_set_phased_thresholds(flower,  ds->lt_phased_trigger_thresholds, ds->lt_phased_servo_thresholds, 0x1ff);

  rno_g_lt_trigger_config_t ltcfg;
  rno_g_lt_phased_trigger_config_t ltcfg_phased;

  ltcfg.window = cfg.lt.trigger.coinc.window;
  ltcfg.vpp_mode = cfg.lt.trigger.coinc.vpp;
  ltcfg.num_coinc =cfg.lt.trigger.coinc.enable_rf_coinc_trigger ?  cfg.lt.trigger.coinc.min_coincidence-1 : 4;
  ltcfg.channel_mask=0xf; //cfg.lt.trigger.coinc.rf_coinc_channel_mask; not implemented. forced to 0xf
  ltcfg_phased.beam_mask=cfg.lt.trigger.phased.rf_phased_beam_mask;
  ltcfg_phased.phased_threshold_offset=cfg.lt.trigger.phased.rf_phased_threshold_offset;
  //might want to add an xorr between enables unless someone really wanted to use both

  int ret = flower_configure_trigger(flower, ltcfg, ltcfg_phased);

  flower_trigger_enables_t trig_enables = {
    .enable_coinc=cfg.lt.trigger.coinc.enable_rf_coinc_trigger,
    .enable_phased=cfg.lt.trigger.phased.enable_rf_phased_trigger,
    .enable_pps = 0,
    .enable_ext = 0
  };

  flower_trigout_enables_t trigout_enables = {
    .enable_rf_sysout=cfg.lt.trigger.enable_rf_trigger_sys_out,
    .enable_rf_auxout=cfg.lt.trigger.enable_rf_trigger_sma_out,
    .enable_pps_sysout=cfg.lt.trigger.enable_pps_trigger_sys_out,
    .enable_pps_auxout=cfg.lt.trigger.enable_pps_trigger_sma_out
  };

  flower_enable_force_trigger_preclear(flower, cfg.lt.waveforms.preclear_force_trigger);

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


static int flower_initial_setup()
{
  if (!flower) return -1;

  //do the auto gain if asked to
  if (cfg.lt.gain.auto_gain)
  {
    float target = cfg.lt.gain.target_rms;
    //disable the coincident trigger momentarily
    flower_trigger_enables_t trig_enables = {.enable_coinc=0, .enable_pps = 0, .enable_ext = 0, .enable_phased=0};
    flower_set_trigger_enables(flower,trig_enables);
    flower_equalize(flower, target, flower_codes, FLOWER_EQUALIZE_VERBOSE, flower_rms);
  }

  if (cfg.lt.waveforms.length > 0 && (cfg.lt.waveforms.at_start.enable || cfg.lt.waveforms.at_finish.enable) && ((run_number % cfg.lt.waveforms.skip_runs) == 0))
  {
    flower_waveforms_len = cfg.lt.waveforms.length;
    flower_waveforms_data = calloc(RNO_G_NUM_LT_CHANNELS, flower_waveforms_len);
    for (int i = 0; i < RNO_G_NUM_LT_CHANNELS; i++)
    {
      flower_waveforms[i] = flower_waveforms_data + i * flower_waveforms_len;
    }
  }

  //thresholds are seeded/pushed inside flower_configure() itself now (so a live config reread
  //re-applies them too, not just this startup call)
  flower_configure();

  return 0;
}


//right now this can only run in the main thread before and after data taking!!!
static int flower_take_waveform(gzFile of, int force, int iev, struct timespec * deadline)
{

  if (!force || !cfg.lt.waveforms.preclear_force_trigger) flower_buffer_clear(flower);
  if (force) flower_force_trigger(flower);
  int avail = 0;
  struct timespec now;
  while (!avail)
  {
    clock_gettime(CLOCK_REALTIME, &now);
    if (deadline && timespec_difference(&now, deadline) > 0) {
      return -1;
    }

    flower_buffer_check(flower, &avail);

    if (!avail)
    {
      usleep(50000); // 50 ms
    }

    // maybe feed watchdog
    if (last_watchdog < now.tv_sec - 5)
    {
      feed_watchdog(0);
    }
  }

  flower_waveform_metadata_t meta = {0};
  flower_read_waveforms(flower, flower_waveforms_len, flower_waveforms);
  flower_read_waveform_metadata(flower ,&meta);

  gzprintf(of,"%s\n\t\t{\n\t\t\t\"force\": %s,\n", iev > 0 ? "," : "", force ? "true" : "false");
  gzprintf(of,"\t\t\t\"metadata\": { \"event_counter\": %u, \"trigger_counter\": %u, \"trigger_type\": \"%s\", \"pps_flag\": %s, \"timestamp\": %"PRIu64 ", \"recent_pps_timestamp\": %"PRIu64 "},\n",
      meta.event_counter, meta.trigger_counter, flower_trigger_type_as_string(meta.trigger_type), meta.pps_flag ? "true" : "false",  meta.timestamp, meta.recent_pps_timestamp);

  for (int i = 0 ; i < RNO_G_NUM_LT_CHANNELS; i++)
  {
    gzprintf(of,"\t\t\t\"ch%d\": [",i);
    for (int j = 0; j < flower_waveforms_len; j++)
    {
      gzprintf(of,"%d",((int)flower_waveforms[i][j])-128);
      if (j < flower_waveforms_len-1)
        gzprintf(of,",");
    }
    if (i < RNO_G_NUM_LT_CHANNELS - 1)
      gzprintf(of,"],\n");
    else
      gzprintf(of,"]\n");
  }
  gzprintf(of,"\t\t}");

  return 0;
}

//right now this can only run in the main thread before and after data taking!!!
static int flower_take_waveforms(int nforce, int nsecs_rf, const char *outfile)
{

  gzFile of = gzopen(outfile,"w");
  gzprintf(of,"{\n\t\"hostname\" : \"rno-g-%03d\", \"run\": %d,\n\t\"events\" : [", station_number, run_number);

  int nev = 0;
  //force first
  for (int iev = 0; iev < nforce; iev++)
  {
    flower_take_waveform(of, 1, nev++, NULL);
  }

  if (nsecs_rf > 0)
  {
    struct timespec rf_start;
    clock_gettime(CLOCK_REALTIME, &rf_start);
    struct timespec deadline = {.tv_sec = rf_start.tv_sec + nsecs_rf, .tv_nsec = rf_start.tv_nsec};

    while (!flower_take_waveform(of, 0, nev, &deadline)) nev++;
  }

  gzprintf(of,"\t]\n}");
  gzclose(of);
  return 0;
}


static void record_timimg()
{
  feed_watchdog(0); //don't get killed by watchdog
  printf("Performing timing measurements. This should just take ~ a minute.\n");

  char command[200];
  snprintf(command, sizeof(command), "%s -n %d --data_dir %s",
      "python3 /home/rno-g/stationrc/record_timings.py",
      cfg.radiant.timing_recording.n_recordings,
      cfg.radiant.timing_recording.directory);

  system(command);
  sleep(1);  // Probably not necessary but does not harm
}


static const char * bias_scan_tmpfile = "/tmp/bias_scan.dat.gz";
static int did_bias_scan = 0;

static int do_bias_scan()
{
  printf("Performing bias scan. This will take a while (20-30 min).\n");
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
  ped.run = run_number;

  for (int val = cfg.radiant.bias_scan.min_val;
      val <= cfg.radiant.bias_scan.max_val;
      val+= cfg.radiant.bias_scan.step_val)
  {
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

  return 0;
}


/* Initial radiant config, including potential pedestal taking and even bias scans!
 *
 * this happens before threads start while holding config lock.
 *
 *
 * */
static int radiant_initial_setup()
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
      lseek(pedestal_fd, 0, SEEK_SET);

      //truncate to right size if not already right
      if (fsize!= sizeof(rno_g_pedestal_t))
      {
        ftruncate(pedestal_fd, sizeof(rno_g_pedestal_t));
      }

      pedestals = mmap(0, sizeof(rno_g_pedestal_t), PROT_READ | PROT_WRITE, MAP_SHARED, pedestal_fd, 0);

      //valid to read, maybe!
      if (pedestals == MAP_FAILED)
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

  //thresholds are seeded/pushed inside radiant_configure() itself now (so a live config reread
  //re-applies them too, not just this startup call)

  //set up DMA correctly
  radiant_reset_fifo_counters(radiant);
  radiant_set_nbuffers_per_readout(radiant, cfg.radiant.readout.nbuffers_per_readout);
  radiant_dma_setup_event(radiant, cfg.radiant.readout.readout_mask);

  //then do the rest of the configuration
  radiant_configure();

  return 0;
}


typedef struct flower_coinc_servo_state
{
  float value[RNO_G_NUM_LT_CHANNELS];
  float last_value[RNO_G_NUM_LT_CHANNELS];
  float error[RNO_G_NUM_LT_CHANNELS];
  float last_error[RNO_G_NUM_LT_CHANNELS];
  float sum_error[RNO_G_NUM_LT_CHANNELS];
} flower_coinc_servo_state_t;

typedef struct flower_phased_servo_state
{
  float value[RNO_G_NUM_LT_BEAMS];
  float last_value[RNO_G_NUM_LT_BEAMS];
  float error[RNO_G_NUM_LT_BEAMS];
  float last_error[RNO_G_NUM_LT_BEAMS];
  float sum_error[RNO_G_NUM_LT_BEAMS];
} flower_phased_servo_state_t;

/** RADIANT's scalers need adjusting for prescaling/period before they're a
 *  plain per-second rate.
 */
static float radiant_raw_coinc_scaler(const rno_g_daqstatus_t * ds, int chan)
{
  return ds->radiant_scalers[chan] * (1 + ds->radiant_prescalers[chan]) / (ds->radiant_scaler_period?:1);
}

/** Per-channel multi-period state for RADIANT's coincidence-trigger threshold
 *  servo: each channel has its own trigger threshold, serviced independently
 *  off that channel's own singles-rate scaler, averaged over a
 *  multi-timescale rolling window (needed because RADIANT's raw scaler
 *  counts are noisy and require prescaler/period correction -- see
 *  radiant_raw_coinc_scaler() above). DIDAQ/FLOWER don't need this: their
 *  scalers are already-computed per-second rates, so they use direct-read
 *  servo state instead (didaq_coinc_servo_state_t / flower_coinc_servo_state_t).
 */
typedef struct radiant_coinc_servo_state
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
} radiant_coinc_servo_state_t;

/** File-scope (not local to radiant_flower_servo()) so that mon_thread() can
 *  free scaler_v_mem after the acquisition loop exits.
 */
static radiant_coinc_servo_state_t radiant_coinc_servo_state = {0};

static void setup_radiant_coinc_servo_state(radiant_coinc_servo_state_t * state, const rno_g_radiant_servo_config_t * servo_cfg)
{
  int max_periods = 0;
  for (int i = 0; i < NUM_SERVO_PERIODS; i++)
  {
    if (servo_cfg->nscaler_periods_per_servo_period[i] > max_periods)
    {
      max_periods = servo_cfg->nscaler_periods_per_servo_period[i];
    }
  }

  if (state->max_periods < max_periods)
  {
    if (state->scaler_v_mem)
    {
      free(state->scaler_v_mem);
      memset(state, 0, sizeof(*state));
    }
    state->scaler_v_mem = malloc(sizeof(float) * max_periods * RNO_G_NUM_RADIANT_CHANNELS);
    state->max_periods = max_periods;
    for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++)
    {
      state->scaler_v[i] = state->scaler_v_mem + max_periods * i;
    }
  }

  memcpy(state->nscaler_periods_per_servo_period, servo_cfg->nscaler_periods_per_servo_period, sizeof(int) * NUM_SERVO_PERIODS);
  memcpy(state->period_weights, servo_cfg->period_weights, sizeof(float) * NUM_SERVO_PERIODS);
}

/** Roll the latest per-channel scalers (via radiant_raw_coinc_scaler()) into
 *  the multi-timescale average and update each channel's servo error state.
 */
static void update_radiant_coinc_servo_state(radiant_coinc_servo_state_t * st, const rno_g_daqstatus_t * ds,
                                      const rno_g_radiant_servo_config_t * servo_cfg)
{
  int idx = (st->nperiods_populated++) % st->max_periods;
  int max_idxs = st->nperiods_populated < st->max_periods ? st->nperiods_populated : st->max_periods;

  for (int chan = 0; chan < RNO_G_NUM_RADIANT_CHANNELS; chan++)
  {
    st->scaler_v[chan][idx] = radiant_raw_coinc_scaler(ds, chan);

    float new_value = 0;
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
      new_value += st->period_weights[j]*sumthis/nthis;
    }

    if (servo_cfg->use_log)
    {
      new_value = log10(servo_cfg->log_offset + new_value);
    }

    servo_record_value(&st->value[chan], &st->last_value[chan], &st->error[chan], &st->last_error[chan],
                        &st->sum_error[chan], new_value, servo_cfg->scaler_goals[chan], servo_cfg->max_sum_err);
  }
}


static void update_flower_coinc_servo_state(flower_coinc_servo_state_t *st, const rno_g_daqstatus_t * ds)
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

    int fw_ver;
    flower_get_fwversion_int(flower, &fw_ver);

    if (fw_ver < 6) fast_factor = 1000;
    else fast_factor = 100;
  }

  for (int i = 0; i < RNO_G_NUM_LT_CHANNELS; i++)
  {

    float val =  fw * fast_factor*fast->servo_per_chan[i]+ sw *(slow->servo_per_chan[i]-sub*slow_gated->servo_per_chan[i]);
    servo_record_value(&st->value[i], &st->last_value[i], &st->error[i], &st->last_error[i],
                        &st->sum_error[i], val, cfg.lt.servo.coinc_scaler_goals[i], 0);
  }
}


static void update_flower_phased_servo_state(flower_phased_servo_state_t *st, const rno_g_daqstatus_t * ds)
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

    uint8_t station, major, minor;
    flower_get_fwversion(flower, &station, &major, &minor,0,0,0);

    if (!major && minor < 6) fast_factor = 1000;
    else fast_factor = 100;
  }

  for (int i = 0; i < RNO_G_NUM_LT_BEAMS; i++)
  {

    float val =  fw * fast_factor*fast->servo_per_beam[i] + sw *(slow->servo_per_beam[i]-sub*slow_gated->servo_per_beam[i]);
    servo_record_value(&st->value[i], &st->last_value[i], &st->error[i], &st->last_error[i],
                        &st->sum_error[i], val, cfg.lt.servo.phased_scaler_goals[i], 0);
  }
}



/** Servo/scaler-monitoring logic for the RADIANT + FLOWER boards, split out of
 *  mon_thread() for readability. Called once per mon_thread loop iteration (with
 *  the cfg read lock already held); keeps its own persistent state (servo
 *  history, computed thresholds, last-update times) across calls via statics.
 */
static void radiant_flower_servo(double nowf)
{
  // The `static` locals below have static storage duration: each is allocated once,
  // for the lifetime of the program (not per-call like a normal local), and keeps
  // its value between calls. This is only safe because radiant_flower_servo()
  // is only ever called from mon_thread (a single thread). radiant_coinc_servo_state
  // is declared at file scope instead (see above its typedef) so mon_thread() can
  // free its scaler_v_mem after the loop exits.
  static int last_cfg_counter = -1;
  static flower_coinc_servo_state_t flwr_coinc_servo_state = {0};
  static flower_phased_servo_state_t flwr_phased_servo_state = {0};

  static float flower_coinc_float_thresh[RNO_G_NUM_LT_CHANNELS];
  static float flower_phased_float_thresh[RNO_G_NUM_LT_BEAMS];

  static uint32_t min_rad_thresh = 0;
  static uint32_t max_rad_thresh = 0;
  static uint32_t max_rad_change = 0;

  static double last_scalers_radiant = 0;
  static double last_scalers_lt = 0;
  static double last_servo_radiant = 0;
  static double last_servo_lt = 0;

  float diff_scalers_radiant = nowf - last_scalers_radiant;
  float diff_scalers_lt = nowf - last_scalers_lt;
  float diff_servo_radiant = nowf - last_servo_radiant;
  float diff_servo_lt = nowf - last_servo_lt;

  //re set up the RADIANT
  if (config_counter > last_cfg_counter)
  {
    last_cfg_counter = config_counter;
    setup_radiant_coinc_servo_state(&radiant_coinc_servo_state, &cfg.radiant.servo);
    memset(&flwr_coinc_servo_state, 0, sizeof(flower_coinc_servo_state_t));
    memset(&flwr_phased_servo_state, 0, sizeof(flower_phased_servo_state_t));

    // We allow re-reading the cfg, so those are not const.
    min_rad_thresh = cfg.radiant.thresholds.min * RADIANT_THRESHOLD_COUNTS_PER_VOLT;
    max_rad_thresh = cfg.radiant.thresholds.max * RADIANT_THRESHOLD_COUNTS_PER_VOLT;
    max_rad_change = cfg.radiant.servo.max_thresh_change * RADIANT_THRESHOLD_COUNTS_PER_VOLT;

    for (int i = 0; i < RNO_G_NUM_LT_CHANNELS; i++) flower_coinc_float_thresh[i] = ds->lt_servo_thresholds[i];
    for (int i = 0; i < RNO_G_NUM_LT_BEAMS; i++) flower_phased_float_thresh[i] = ds->lt_phased_servo_thresholds[i];
  }

  //do we need radiant scalers?
  if ( (cfg.radiant.trigger.RF[0].enabled || cfg.radiant.trigger.RF[1].enabled) &&
        cfg.radiant.servo.scaler_update_interval &&
        cfg.radiant.servo.scaler_update_interval < diff_scalers_radiant )
  {
    while (1)
    {
      //read twice and make sure equal
      static rno_g_daqstatus_t ds0 = {0};
      memcpy(&ds0, ds, sizeof(ds0)); // copy the flower stuff so it doesn't get overwritten
      static uint16_t scaler_check[RNO_G_NUM_RADIANT_CHANNELS] = {0};
      int ok = radiant_read_daqstatus(radiant, &ds0) + radiant_get_scalers(radiant, 0, RNO_G_NUM_RADIANT_CHANNELS-1, scaler_check);

      if (ok) fprintf(stderr,"Problem reading daqstatus\n");

      if (!memcmp(ds0.radiant_scalers, scaler_check, sizeof(ds0.radiant_scalers)))
      {
          memcpy(ds, &ds0, sizeof(ds0));
          break;
      }

      printf("WARNING: Unequal sequential DAQStatus, trying again\n");
    }

    //update the running averages for the radiant
    update_radiant_coinc_servo_state(&radiant_coinc_servo_state, ds, &cfg.radiant.servo);
    last_scalers_radiant = nowf;
  }

  // do we need to servo radiant?
  if ((cfg.radiant.trigger.RF[0].enabled||cfg.radiant.trigger.RF[1].enabled) && cfg.radiant.servo.enable && cfg.radiant.servo.servo_interval
      && cfg.radiant.servo.scaler_update_interval < diff_servo_radiant)
  {
    for (int ch = 0; ch < RNO_G_NUM_RADIANT_CHANNELS; ch++)
    {
      //only servo channels that are part of the trigger?
      if ( 0 == (radiant_trig_chan & (1 << ch))) continue;

      double dthreshold = servo_pid_step(cfg.radiant.servo.P, cfg.radiant.servo.I, cfg.radiant.servo.D,
                           radiant_coinc_servo_state.error[ch], radiant_coinc_servo_state.sum_error[ch], radiant_coinc_servo_state.last_error[ch]);

      if (max_rad_thresh && fabs(dthreshold) > max_rad_change)
      {
        dthreshold = (dthreshold < 0)  ? -max_rad_change : max_rad_change;
      }

      ds->radiant_thresholds[ch] -= dthreshold;
      if (ds->radiant_thresholds[ch] < min_rad_thresh)  ds->radiant_thresholds[ch] = min_rad_thresh;
      if (ds->radiant_thresholds[ch] > max_rad_thresh)  ds->radiant_thresholds[ch] = max_rad_thresh;
    }

    //set the thresholds
    radiant_set_trigger_thresholds(radiant, 0, RNO_G_NUM_RADIANT_CHANNELS-1, ds->radiant_thresholds);
    last_servo_radiant = nowf;
  }


  // do we need LT scalers?
  if ((cfg.lt.trigger.coinc.enable_rf_coinc_trigger||cfg.lt.trigger.phased.enable_rf_phased_trigger)&&cfg.lt.servo.scaler_update_interval && cfg.lt.servo.scaler_update_interval < diff_scalers_lt && flower)
  {
    flower_fill_daqstatus(flower, ds);

    update_flower_coinc_servo_state(&flwr_coinc_servo_state, ds);
    update_flower_phased_servo_state(&flwr_phased_servo_state, ds);

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
    if(cfg.lt.trigger.coinc.enable_rf_coinc_trigger)
    {
      for (int ch = 0; ch < RNO_G_NUM_LT_CHANNELS; ch++)
      {
         if(!(cfg.lt.trigger.coinc.rf_coinc_channel_mask&(1<<ch))) continue;//ignore turned off beams
         double d_servo_threshold = servo_pid_step(cfg.lt.servo.P, cfg.lt.servo.I, cfg.lt.servo.D,
                                  flwr_coinc_servo_state.error[ch], flwr_coinc_servo_state.sum_error[ch], flwr_coinc_servo_state.last_error[ch]);


         flower_coinc_float_thresh[ch] = clamp(flower_coinc_float_thresh[ch] + d_servo_threshold,4,120);
         ds->lt_servo_thresholds[ch] = flower_coinc_float_thresh[ch];
         ds->lt_trigger_thresholds[ch] = clamp( (flower_coinc_float_thresh[ch] - cfg.lt.servo.servo_thresh_offset) / cfg.lt.servo.servo_thresh_frac, 4, 120);
      }
      flower_set_coinc_thresholds(flower,ds->lt_trigger_thresholds,ds->lt_servo_thresholds,cfg.lt.trigger.coinc.rf_coinc_channel_mask);
    }

    if(cfg.lt.trigger.phased.enable_rf_phased_trigger)
    {
      for (int beam = 0; beam < RNO_G_NUM_LT_BEAMS; beam++)
      {
         if(!(cfg.lt.trigger.phased.rf_phased_beam_mask&(1<<beam))) continue;//ignore turned off beams
         double d_servo_threshold = servo_pid_step(cfg.lt.servo.phased_P, cfg.lt.servo.I, cfg.lt.servo.D,
                                  flwr_phased_servo_state.error[beam], flwr_phased_servo_state.sum_error[beam], flwr_phased_servo_state.last_error[beam]);


       flower_phased_float_thresh[beam] = clamp(flower_phased_float_thresh[beam] + d_servo_threshold, 4, 4095);
       ds->lt_phased_servo_thresholds[beam] = flower_phased_float_thresh[beam];
       ds->lt_phased_trigger_thresholds[beam] = clamp((flower_phased_float_thresh[beam] - cfg.lt.servo.servo_thresh_offset) / cfg.lt.servo.phased_servo_thresh_frac, 1, 4095);
      }
      flower_set_phased_thresholds(flower,ds->lt_phased_trigger_thresholds,ds->lt_phased_servo_thresholds,cfg.lt.trigger.phased.rf_phased_beam_mask);
    }

    last_servo_lt = nowf;
  }
}


/**
 * Open and configure the radiant and flower boards.
 *
 *  - initialize the radiant lock and, record the timing before the radiant
 *    (with a python script which initalizes the radiant by itself)
 *  - open the radiant, retrying (and dropping kernel caches) a few times
 *    in case kernel fragmentation is preventing the open, and giving up
 *    after too many failed attempts
 *  - initialize the flower lock and open the flower (fatal only if the
 *    flower is marked as required in the config), and warn if its
 *    firmware reports a station number that doesn't match ours
 *  - run each board's initial setup routine, feeding the watchdog between
 *    steps since this can take a while (take bias scan if condition is met)
 *
 * Returns 0 on success, 1 on failure (radiant could not be opened after
 * repeated attempts, the flower could not be opened but is required, or
 * either board's initial setup failed).
 */
static int open_and_setup_radiant_and_flower()
{
  //initialize the radiant lock
  pthread_rwlock_init(&radiant_lock, NULL);

  // When it is time to do a bias scan record the timing before setting up the radiant
  if (cfg.radiant.timing_recording.enable && ((cfg.radiant.timing_recording.skip_runs < 2) ||
      ((run_number % cfg.radiant.timing_recording.skip_runs) == 0)))
  {
    record_timimg();
  }

  int nattempts = 0;
  //open the radiant
  do
  {
    radiant  = radiant_open(
      cfg.radiant.device.spi_device,
      cfg.radiant.device.uart_device,
      cfg.radiant.device.poll_gpio,
      cfg.radiant.device.spi_enable_gpio);

    if (!radiant)
    {
      fprintf(stderr, "COULD NOT OPEN RADIANT. Attemping to drop caches in case kernel fragmentation is the issue.");
      if (nattempts++ > 3)
      {
        fprintf(stderr, "Giving up...\n");
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
  pthread_rwlock_init(&flower_lock, NULL);

  flower = flower_open(cfg.lt.device.spi_device, cfg.lt.device.spi_enable_gpio);
  if (!flower && cfg.lt.device.required)
  {
    fprintf(stderr, "COULD NOT OPEN FLOWER. Waiting 20 seconds before quitting");
    sleep(20);
    return 1;
  }

  uint8_t fwstation, fwmajor, fwminor;
  flower_get_fwversion(flower, &fwstation, &fwmajor, &fwminor, 0, 0, 0);
  if ((1000*fwmajor + fwminor) >= 14 && fwstation != station_number)
  {
    //complain but don't quit since it's not necessarily fatal (ie lab testing)
    fprintf(stderr,"Station number and station specific FLOWER firmware mismatch!\n");
  }

  feed_watchdog(0);

  //intitial configure of the radiant, bail if can't open
  if (radiant_initial_setup())
    return 1;
  feed_watchdog(0);

  //and the flower, bail if can't open  and required
  if (flower_initial_setup() && cfg.lt.device.required)
    return 1;
  feed_watchdog(0);

  return 0;
}

// you should be holding a flower lock while calling this
static int flower_update_pps_offset()
{
  float wanted_delay = cfg.lt.trigger.pps_trigger_delay;

  // clamp to a second
  if (fabs(wanted_delay) >= 1e6) wanted_delay =   (wanted_delay*1e-6 - ((int) (wanted_delay*1e-6)))*1e6;

  int delay_cycles = round(wanted_delay * delay_clock_estimate/1e6);
  if (delay_cycles < 0) delay_cycles += delay_clock_estimate;
  return flower_set_delayed_pps_delay(flower,delay_cycles);
}

#endif


static struct drand48_data sw_rand;
static double calc_next_sw_trig(float now)
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


static void set_calpulser_atten(float atten)
{
    if (atten < 0) atten = 0;
    if (atten > 31.5) atten = 31.5;
    atten = round(atten*2);
    rno_g_cal_set_atten(calpulser,(uint8_t) atten);
}

static int calpulser_configure()
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


static float clamp(float val, float min, float max)
{
  if (val > max) return max;
  if (val < min) return min;
  return val;
}


/** Update a single servo channel's error-tracking state given its latest raw value.
 *  This bookkeeping (value/last_value/error/last_error/sum_error, with optional
 *  clamping of the accumulated error) is the same regardless of how the raw value
 *  or goal were computed, so any digitizer's servo loop can share it. Pass 0 for
 *  max_sum_err to disable clamping.
 */
static void servo_record_value(float * value, float * last_value, float * error, float * last_error,
                                float * sum_error, float new_value, float goal, float max_sum_err)
{
  *last_value = *value;
  *value = new_value;
  *last_error = *error;
  *error = new_value - goal;
  *sum_error += *error;
  if (max_sum_err && fabs(*sum_error) > max_sum_err)
  {
    *sum_error = *sum_error < 0 ? -max_sum_err : max_sum_err;
  }
}

/** Standard PID step, given the gains and a channel's current error state. */
static double servo_pid_step(double P, double I, double D, float error, float sum_error, float last_error)
{
  return P * error + I * sum_error + D * (error - last_error);
}


/** The acquisition thread
 *
 * This has sole control over the SPI interface for the RADIANT.
 * Since configuration is always done over UART, this does not need to react to config changes,
 * but it may need to temporarily pause. For this reason it acquires a read lock on the radiant_config lock.
 *
 **/
static void * acq_thread(void* v)
{
  (void) v;
  while(!quit)
  {
    //acquire read lock on radiant, flower, and cfg

    pthread_rwlock_rdlock(&cfg_lock);

#ifndef ON_DIDAQ
    pthread_rwlock_rdlock(&radiant_lock);
    pthread_rwlock_rdlock(&flower_lock);
    // wait for the RADIANT to trigger
    //TODO handle clear flag, though we don't really want one
    if (radiant_poll_trigger_ready(radiant, cfg.radiant.readout.poll_ms))
    {
      // Get a buffer , and fill it
      acq_buffer_item_t * mem = ice_buf_getmem(acq_buffer);
      radiant_read_event(radiant, &mem->hd, &mem->wf);
      if (flower) flower_fill_header(flower, &mem->hd);

#else
    pthread_mutex_lock(&didaq_lock);
    // wait for the DIDAQ to trigger
    if (didaq_poll_trigger_ready(didaq, cfg.didaq.readout.poll_ms))
    {
      // Get a buffer , and fill it
      acq_buffer_item_t * mem = ice_buf_getmem(acq_buffer);
      didaq_read_event(didaq, &mem->hd, &mem->wf);

#endif

      mem->hd.run_number = run_number;
      mem->wf.run_number = run_number;
      mem->hd.station_number = station_number;
      mem->wf.station = station_number;
      ice_buf_commit(acq_buffer);
    }

    //release the read locks
    pthread_rwlock_unlock(&cfg_lock);
#ifndef ON_DIDAQ
    pthread_rwlock_unlock(&flower_lock);
    pthread_rwlock_unlock(&radiant_lock);
#else
    pthread_mutex_unlock(&didaq_lock);
#endif
  }

  return 0;
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

  //last output time
  double last_daqstatus_out = 0;

  double next_sw_trig = -1;

  while(!quit)
  {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double nowf = now.tv_sec + 1e-9 * now.tv_nsec;

    //figure out how long it's been since we wrote a daqstatus or swept the calpulser
    float diff_last_daqstatus_out = nowf - last_daqstatus_out;
    float diff_sweep = nowf - sweep_time;

    //Hold the config read lock to avoid values getting take from underneath us
    pthread_rwlock_rdlock(&cfg_lock);


    if (next_sw_trig < 0)
    {
      next_sw_trig = calc_next_sw_trig(nowf);
    }


#ifdef ON_DIDAQ
    //do we need to send a soft trigger?
    if (cfg.didaq.trigger.soft.enabled && nowf > next_sw_trig)
    {
      pthread_mutex_lock(&didaq_lock);
      didaq_force_trigger(didaq);
      pthread_mutex_unlock(&didaq_lock);
      next_sw_trig = calc_next_sw_trig(nowf);
    }

    didaq_servo(nowf);
#else
    //do we need to send a soft trigger?
    if (cfg.radiant.trigger.soft.enabled && nowf > next_sw_trig)
    {
      radiant_force_trigger(didaq);
      next_sw_trig = calc_next_sw_trig(nowf);
    }

    radiant_flower_servo(nowf);
#endif

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
      memcpy(&mem->ds, ds, sizeof(rno_g_daqstatus_t));
      ice_buf_commit(mon_buffer);
      if (shared_ds_fd) msync(ds, sizeof(rno_g_daqstatus_t), MS_ASYNC);
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
    if ((cfg.radiant.trigger.soft.enabled || cfg.didaq.trigger.soft.enabled) && next_sw_trig - nowf < sleep_amt)
      sleep_amt = (next_sw_trig - nowf) * 3./4;

    usleep(sleep_amt * 1e6);
  }

  //mostly to suppress warnings
#ifndef ON_DIDAQ
  if (radiant_coinc_servo_state.scaler_v_mem) free(radiant_coinc_servo_state.scaler_v_mem);
#endif

  return 0;
}

//this makes the necessary directories for a time
//returns 0 on success.
static int make_dirs_for_output(const char * prefix)
{

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
    snprintf(bigbuf,bigbuflen,"%s/%s",prefix,subdirs[i]);
    if (mkdir_if_needed(bigbuf))
    {
        fprintf(stderr,"Couldn't make %s. Bad things will happen!\n",bigbuf);
        return 1;
    }
  }

  return 0;
}

static const char * tmp_suffix = ".tmp";
static const int tmp_suffix_len = 4;


static int do_close(rno_g_file_handle_t h, char *path)
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

  int num_events = 0;
  int num_events_this_cycle = 0;

  int ds_i = 0;

  //open the run info and start filling it in
  sprintf(bigbuf,"%s/aux/runinfo.txt", output_dir);
  runinfo = fopen(bigbuf,"w");
  if (runinfo)
  {
    add_to_file_list(bigbuf);
    fprintf(runinfo, "STATION = %d\n", station_number);
    fprintf(runinfo, "RUN = %d\n", run_number);
    fprintf(runinfo, "RUN-START-TIME =  %ld.%09ld\n",precise_start_time.tv_sec, precise_start_time.tv_nsec);
    fprintf(runinfo, "ACQ-START-TIME =  %ld.%09ld\n",precise_acq_time.tv_sec, precise_acq_time.tv_nsec);
    fprintf(runinfo, "LIBRNO-G-GIT-HASH = %s\n", rno_g_get_git_hash());
    fprintf(runinfo, "RNO-G-ICE-SOFTWARE-GIT-HASH = %s\n", get_ice_software_git_hash());
    fprintf(runinfo, "FREE-SPACE-MB-OUTPUT-PARTITION = %f\n", output_partition_free);
    fprintf(runinfo, "FREE-SPACE-MB-RUNFILE-PARTITION = %f\n", runfile_partition_free);

#ifdef ON_DIDAQ
    //write down didaq info to runinfo
    //TODO: assumes didaq exposes fw version / sample rate calls analogous to the RADIANT's; no FLOWER equivalent to report.
    uint8_t fwmajor, fwminor, fwrev, fwyear, fwmon, fwday;
    didaq_get_fw_version(didaq, &fwmajor, &fwminor, &fwrev, &fwyear, &fwmon, &fwday);
    fprintf(runinfo, "DIDAQ-FWVER = %02u.%02u.%02u\n", fwmajor, fwminor, fwrev);
    fprintf(runinfo, "DIDAQ-FWDATE = 20%02u-%02u.%02u\n", fwyear, fwmon, fwday);

    // didaq_get_fw_version(didaq, DEST_MANAGER,  &fwmajor, &fwminor, &fwrev, &fwyear, &fwmon, &fwday);
    // fprintf(runinfo, "DIDAQ-BM-FWVER = %02u.%02u.%02u\n", fwmajor, fwminor, fwrev);
    // fprintf(runinfo, "DIDAQ-BM-FWDATE = 20%02u-%02u.%02u\n", fwyear, fwmon, fwday);

    uint16_t sample_rate = didaq_get_sample_rate(didaq);
    fprintf(runinfo, "DIDAQ-SAMPLERATE = %u\n", sample_rate);
#else
    //write down radiant info to runinfo
    uint8_t fwstation, fwmajor, fwminor, fwrev, fwyear, fwmon, fwday;
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
      flower_get_fwversion(flower, &fwstation, &fwmajor, &fwminor, &flower_fwyear, &fwmon, &fwday);
      fprintf(runinfo, "FLOWER-FWVER = %02u.%02u.%02u\n", fwstation, fwmajor, fwminor);
      fprintf(runinfo, "FLOWER-FWDATE = %02u-%02u.%02u\n", flower_fwyear, fwmon, fwday);
    }
    else
    {
      fprintf(runinfo, "FLOWER-FWVER = 0.0.0\n");
      fprintf(runinfo, "FLOWER-FWDATE = 0000-00.00\n");
    }
#endif
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
#ifndef ON_DIDAQ
    if (!flower) fprintf(fcomment, " !!FLOWER NOT DETECTED!!");
#endif
    fclose(fcomment);
    add_to_file_list(bigbuf);
  }
  else
  {
    fprintf(stderr,"Yikes, couldn't write to %s\n", bigbuf);
  }

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

#ifdef ON_DIDAQ

  //write gain codes
  write_gain_codes_didaq(bigbuf);

#else

  //write gain codes


  write_gain_codes_flower(bigbuf);

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
#endif

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
      printf("-------S%d/R%d after %u seconds-----------\n", station_number, run_number, (unsigned) (now - start_time));
      printf("  total events written: %d\n", num_events);
      printf("  write rate:  %g Hz\n", (num_events == 0) ? 0. :  ((float) num_events_this_cycle) / (now - last_print_out));
      printf("  write buffer occupancy: %d/%d\n", acq_occupancy , cfg.runtime.acq_buf_size);
      num_events_this_cycle = 0;
      rno_g_daqstatus_dump(stdout, ds);
      last_print_out = now;
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

        ds_file_size += rno_g_daqstatus_write(ds_handle, &mon_item.ds);
        ds_file_N++;
        ds_i++;
      }
    }

    if ((int) ice_buf_occupancy(acq_buffer) < cfg.runtime.acq_buf_size /3)
    {
      usleep(25000);
    }

  }

  if (runinfo)
  {
    fprintf(runinfo, "TOTAL-NUMBER-OF-EVENTS-WRITTEN = %d\n", num_events);
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

/**
 * Perform the earliest DAQ startup steps, before any hardware is touched:
 *
 *  - initialize the config lock and load the config
 *  - block (while periodically feeding the watchdog) until both the
 *    runfile partition and the output partition have at least the
 *    configured minimum amount of free space
 *  - record the precise start time
 *  - determine the station number from /STATION_ID (defaulting to 0 if
 *    it can't be read)
 *  - determine the run number and output directory from the runfile,
 *    incrementing the run number as needed to avoid clobbering an
 *    existing run directory (unless overwriting is explicitly allowed)
 *  - make sure the calibration pulser is off, in case of an unclean exit
 *  - open (or, if unavailable, allocate) the shared daqstatus struct and
 *    initialize the radiant/flower trigger thresholds, unless they were
 *    already loaded from a pre-existing shared status file
 *
 * On return, *frun_out holds the open runfile handle (or NULL if no
 * runfile existed yet) so the caller can later rewrite it with the next
 * run number.
 *
 * Returns 0 on success, 1 on failure (a negative run number was found in
 * the runfile).
 */
static int setup_run_and_daqstatus(FILE ** frun_out)
{
  /** Initialize config lock and try to read the config */
  pthread_rwlock_init(&cfg_lock,NULL);
  read_config();

  // Check that there is sufficient free space before proceeding any farther;
  runfile_partition_free = get_free_MB_by_path(cfg.output.runfile);
  output_partition_free = get_free_MB_by_path(cfg.output.base_dir);

  while ( (cfg.output.min_free_space_MB_runfile_partition &&
           runfile_partition_free < cfg.output.min_free_space_MB_runfile_partition) ||
          (cfg.output.min_free_space_MB_output_partition &&
           output_partition_free < cfg.output.min_free_space_MB_output_partition) )
  {
    fprintf(stderr,"Insufficient free space on runfile partition (%f MB free,  %d) and/or output partition (%f MB free,  %d). Waiting ~300 seconds before trying again\n",
            runfile_partition_free, cfg.output.min_free_space_MB_runfile_partition, output_partition_free, cfg.output.min_free_space_MB_output_partition);

    //avoid getting killed by watchdog
    for (int i = 0; i < 15; i++)
    {
      sleep(20);
      feed_watchdog(0);
    }
    runfile_partition_free = get_free_MB_by_path(cfg.output.runfile);
    output_partition_free = get_free_MB_by_path(cfg.output.base_dir);
  }

  clock_gettime(CLOCK_REALTIME, &precise_start_time);

  // Read the station number
  const char * station_number_file = "/STATION_ID";
  FILE *fstation = fopen(station_number_file,"r");
  fscanf(fstation, "%d\n", &station_number);
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

  //make sure calpulser is turned off (in case we didn't exit cleanly!) since we don't want it on during pedestal taking and such
  rno_g_cal_disable_no_handle(cfg.calib.gpio);

  //open the shared status file, if it's there. (always, even if we are not loading the thresholds)
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
      shared_ds_file_size = lseek(shared_ds_fd, 0, SEEK_END);
      lseek(shared_ds_fd, 0, SEEK_SET);

      if (shared_ds_file_size != sizeof(rno_g_daqstatus_t))
      {
        ftruncate(shared_ds_fd, sizeof(rno_g_daqstatus_t));
      }

      ds = mmap(0, sizeof(rno_g_daqstatus_t), PROT_READ | PROT_WRITE, MAP_SHARED, shared_ds_fd,0);
    }
  }

  if (!shared_ds_fd)
  {
    ds = calloc(sizeof(rno_g_daqstatus_t),1);
  }


  *frun_out = frun;
  return 0;
}


/**
 * Advance the runfile to the next run number and set up the output
 * directory for the current run.
 *
 *  - if a runfile was open (frun non-NULL), atomically overwrite it with
 *    run_number+1: write to a temporary file, then rename it over the
 *    real runfile, so a future invocation picks up where this run left
 *    off even if we crash partway through
 *  - allocate the scratch buffer (bigbuf) used throughout the program for
 *    building output file paths
 *  - create the output directory tree for this run
 *  - open the per-run file list, used to track every output file written
 *    during this run, and record the file list itself in it
 *
 * frun is closed (or, on the error path, left for the caller to ignore)
 * by this function; it must not be used by the caller afterward.
 *
 * Returns 0 on success, 1 on failure (temporary run file couldn't be
 * opened/written/renamed, or the scratch buffer couldn't be allocated).
 */
static int setup_output_dir_and_runfile(FILE * frun)
{
  //update the run file
  if (frun)
  {
    fclose(frun);

    char * tmp_run_file = 0;
    asprintf(&tmp_run_file, "%s.tmp", cfg.output.runfile);
    frun = fopen(tmp_run_file, "w");
    if (!frun)
    {
      fprintf(stderr, "Could not open temporary run file: %s\n", tmp_run_file);
      return 1;
    }
    if (0 > fprintf(frun, "%d\n", run_number + 1) || 0 != fclose(frun))
    {
      fprintf(stderr, "Problem writing temporary run file %s\n", tmp_run_file);
      return 1;
    }
    if (rename(tmp_run_file, cfg.output.runfile))
    {
      fprintf(stderr,"Problem moving %s to %s\n", tmp_run_file, cfg.output.runfile);
      return 1;
    }
    free(tmp_run_file);
  }

  bigbuflen = strlen(output_dir) + 512 + 1;
  bigbuf = calloc(1, bigbuflen);

  if (!bigbuf)
  {
    fprintf(stderr,"Could not allocate buffer... that's not good!");
    return 1;
  }

  //let's make the output directories here now
  make_dirs_for_output(output_dir);

  //open the file list
  sprintf(bigbuf, "%s/aux/acq-file-list.txt", output_dir);
  file_list = fopen(bigbuf, "w");
  file_list_fd = fileno(file_list);
  add_to_file_list(bigbuf);

  return 0;
}

/**
 * Install signal handlers, initialize the acq/mon ring buffers, and
 * start the acq, mon, and wri (write) threads.
 *
 *  - install a shared sigaction (signal_handler) for SIGINT, SIGTERM, and
 *    SIGUSR1
 *  - initialize the acq and mon ring buffers from the configured sizes
 *  - record the precise acq start time, then start the acq and mon
 *    threads
 *  - take a read lock on the config lock and hold it (it is released once
 *    the write thread has finished writing out the config) before
 *    starting the write thread, so the write thread is guaranteed to see
 *    a consistent config while the acq/mon threads are already running
 */
static void start_threads()
{
  //set up signal handlers
  sigset_t empty;
  sigemptyset(&empty);
  struct sigaction sa;
  sa.sa_mask = empty;
  sa.sa_flags = 0;
  sa.sa_sigaction = signal_handler;
  sigaction(SIGINT, &sa, 0);
  sigaction(SIGTERM, &sa, 0);
  sigaction(SIGUSR1, &sa, 0);

  //initialize the buffers
  acq_buffer = ice_buf_init(cfg.runtime.acq_buf_size, sizeof(acq_buffer_item_t));
  mon_buffer = ice_buf_init(cfg.runtime.mon_buf_size, sizeof(mon_buffer_item_t));

  //now let's make the threads
  clock_gettime(CLOCK_REALTIME, &precise_acq_time);
  pthread_create(&the_acq_thread,NULL, acq_thread, NULL);
  pthread_create(&the_mon_thread,NULL, mon_thread, NULL);
  feed_watchdog(0);

  //hold the cfg lock until the write thread is done writing the config
  pthread_rwlock_rdlock(&cfg_lock);

  pthread_create(&the_wri_thread, NULL, wri_thread, NULL);
}


static int please_stop()
{
  printf("Stopping...\n");
  quit = 1;
  clock_gettime(CLOCK_REALTIME, &precise_stop_time);
  return 0;
}


int main(int nargs, char ** args)
{
  if (nargs > 1) cfgpath = args[1];

  FILE * frun = NULL;
  if (setup_run_and_daqstatus(&frun))
    return 1;

#ifdef ON_DIDAQ

  if (open_and_setup_didaq())
    return 1;

#else

  if (open_and_setup_radiant_and_flower())
    return 1;

  #endif

  // I think the reason we are only doing this now is to not create empty run directories
  // while we have problems with the hardware and the run would restart...
  if (setup_output_dir_and_runfile(frun))
    return 1;

#ifndef ON_DIDAQ
  //HACK, take initial flower data if we need to
  if (flower && cfg.lt.waveforms.at_start.enable && ((run_number % cfg.lt.waveforms.skip_runs) == 0))
  {
    snprintf(bigbuf,bigbuflen,"%s/aux/flower_start.json.gz", output_dir);
    add_to_file_list(bigbuf);
    flower_take_waveforms(cfg.lt.waveforms.at_start.nforce, cfg.lt.waveforms.at_start.nsecs_rf, bigbuf);
  }
#endif

  start_threads();

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

    if (cfg.output.min_free_space_MB_output_partition > 0)
    {
      // Stop while available memory drops while running. Already performing test in
      // setup_run_and_daqstatus to stop run from actually starting (and run folder being created ...)
      double MBfree = get_free_MB_by_path(cfg.output.base_dir);
      if (MBfree < cfg.output.min_free_space_MB_output_partition)
      {
        fprintf(stderr,
          "Output partition free space is just %f MB, smaller than minimum %d MB\n",
          MBfree, cfg.output.min_free_space_MB_output_partition);
        please_stop();
        continue;
      }
    }

    // Stop at the end of the run
    clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
    if (now.tv_sec - start_time.tv_sec > cfg.output.seconds_per_run)
    {
      please_stop();
    }
    usleep(500e3);
    sched_yield();
  }

  return teardown();
}

static int teardown()
{
  pthread_join(the_acq_thread,0);
  pthread_join(the_mon_thread,0);
  pthread_join(the_wri_thread,0);

#ifndef ON_DIDAQ

  //HACK, take final flower data if we need to
  if (flower && cfg.lt.waveforms.at_finish.enable  && ((run_number % cfg.lt.waveforms.skip_runs) == 0))
  {
    snprintf(bigbuf,bigbuflen,"%s/aux/flower_end.json.gz", output_dir);
    add_to_file_list(bigbuf);
    flower_take_waveforms(cfg.lt.waveforms.at_finish.nforce, cfg.lt.waveforms.at_finish.nsecs_rf, bigbuf);
  }

  //disable the trigger OVLD
  radiant_trigger_enable(radiant,0,0);
  radiant_labs_stop(radiant);
  radiant_close(radiant);
  if (flower) flower_close(flower);

#endif

  fclose(file_list);
  struct timespec end_time;
  clock_gettime(CLOCK_REALTIME, &end_time);
  if (runinfo)
  {
    fprintf(runinfo,"RUN-STOP-TIME = %ld.%09ld\n", precise_stop_time.tv_sec, precise_stop_time.tv_nsec);
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
    munmap(ds, sizeof(rno_g_daqstatus_t));
    close(shared_ds_fd);
  }

  return 0;
}
