#ifndef _rno_g_ice_config_h
#define _rno_g_ice_config_h

#include "rno-g.h"

/** Configuration structs */

#define NUM_SERVO_PERIODS 3
#define NUM_SERVO_PERIODS_STR "3"


/** The acquisition config
 *
 * Some things are changeable at runtime, some require a run restart;
 **/
typedef struct acq_config
{

  //radiant-specific things
  struct
  {

    //Scaler configuration
    struct
    {
      int use_pps;  //use pps, otherwise period is used
      float period ; //period in seconds if not using pps
      uint8_t prescal_m1[RNO_G_NUM_RADIANT_CHANNELS];  //the prescaler minus 1 for this channe
    } scalers;

    struct
    {
      int load_from_threshold_file;
      float initial[RNO_G_NUM_RADIANT_CHANNELS];
      float max;
      float min;
    } thresholds;

    struct
    {
      int enable;
      int use_log;
      float log_offset;
      float scaler_update_interval;
      float servo_interval;
      int nscaler_periods_per_servo_period[NUM_SERVO_PERIODS];
      float period_weights [NUM_SERVO_PERIODS];
      float scaler_goals[RNO_G_NUM_RADIANT_CHANNELS]; // Scaler goals for each channel
      float max_thresh_change;
      float max_sum_err;
      float P;
      float I;
      float D;
    } servo;

    struct
    {
      struct
      {
        int enabled;
        int use_exponential_distribution;
        float interval;
        float interval_jitter;
        int output_enabled;
      } soft;


      struct
      {
        int enabled;
      } ext;

      struct
      {
        int enabled;
        int output_enabled;
      } pps;

      struct
      {
        int enabled;
        uint32_t mask;
        float window;
        int num_coincidences;
        int readout_delay;
        int readout_delay_mask;
      } RF[2];


      int clear_mode;
      int output_enabled;

    } trigger;

    struct
    {
      uint32_t readout_mask;
      int nbuffers_per_readout;
      int poll_ms;
    } readout;

    struct
    {
      int compute_at_start;
      int ntriggers_per_computation;
      int apply_attenuation;
      float attenuation;
      const char * pedestal_file;
      int pedestal_subtract;
      int ntriggers_per_cycle;
      double sleep_per_cycle;
    } pedestals;


    struct
    {
      int apply_lab4_vbias;
      float lab4_vbias[2];

      int apply_diode_vbias;
      float diode_vbias[RNO_G_NUM_RADIANT_CHANNELS];

      int apply_attenuations;
      float digi_attenuation[RNO_G_NUM_RADIANT_CHANNELS];
      float trig_attenuation[RNO_G_NUM_RADIANT_CHANNELS];

      float settle_time;

    } analog;


    struct
    {
      const char * reset_script;
      const char * spi_device;
      const char * uart_device;
      int poll_gpio;
      int spi_enable_gpio;
    } device;

    struct
    {
      int use_internal;
      int sync_out;
      int pps_holdoff;

    } pps;

    struct
    {
      int enable_bias_scan;
      int skip_runs;
      int min_val;
      int step_val;
      int max_val;
      int navg_per_step;
      float sleep_time;
      int apply_attenuation;
      float attenuation;
    } bias_scan;

    struct
    {
      int enable;
      int skip_runs;
      int n_recordings;
      const char * directory;
    } timing_recording;

  } radiant;

  //low threshold board
  struct
  {
    struct
    {
      struct
      {
        int enable_rf_coinc_trigger;       
        int rf_coinc_channel_mask;
        int vpp ;
        int min_coincidence;
        int window;
      }coinc;
      
      struct
      {
        int enable_rf_phased_trigger; 
        int rf_phased_beam_mask;
        int rf_phased_threshold_offset;
      }phased;

      int enable_rf_trigger_sma_out;
      int enable_rf_trigger_sys_out;
      int enable_pps_trigger_sys_out;
      int enable_pps_trigger_sma_out;
      float pps_trigger_delay;
    } trigger;





    struct
    {
      int load_from_threshold_file; 
      uint8_t initial_coinc_thresholds[RNO_G_NUM_LT_CHANNELS];
      uint16_t initial_phased_thresholds[RNO_G_NUM_LT_BEAMS];

    } thresholds; 

    struct
    {
      int enable; 
      int subtract_gated; 
      uint16_t coinc_scaler_goals[RNO_G_NUM_LT_CHANNELS]; 
      uint16_t phased_scaler_goals[RNO_G_NUM_LT_BEAMS]; 
      float servo_thresh_frac; 
      float phased_servo_thresh_frac; 
      float servo_thresh_offset; 
      float fast_scaler_weight;
      float slow_scaler_weight;
      float scaler_update_interval; 
      float servo_interval; 
      float P; 
      float phased_P; 
      float I; 
      float D; 
    } servo; 

    struct
    {
      const char * spi_device;
      int spi_enable_gpio;
      int required;
    } device;

    struct
    {
      int auto_gain;
      float target_rms;
      uint8_t fixed_gain_codes[RNO_G_NUM_LT_CHANNELS];
    } gain;


  } lt;


  struct
  {
    const char * status_shmem_file;
    int acq_buf_size;
    int mon_buf_size;
  } runtime;



  //output
  struct
  {
    const char * base_dir;
    const char * runfile;
    const char * comment;
    float daqstatus_interval;
    int seconds_per_run;
    int max_events_per_file;
    int max_daqstatuses_per_file;
    int max_seconds_per_file;
    int max_kB_per_file;
    int min_free_space_MB_output_partition;
    int min_free_space_MB_runfile_partition;
    int print_interval;
    int allow_rundir_overwrite;
  } output;

  //calibration
  struct
  {
    int turn_off_at_exit;
    int enable_cal;
    int i2c_bus;
    int gpio;
    const char * rev;
    rno_g_calpulser_out_t channel;
    rno_g_calpulser_mode_t type;
    float atten;

    struct
    {
      int enable;
      float start_atten;
      float stop_atten;
      float atten_step;
      int step_time;
    } sweep;

  } calib;


} acq_config_t;


/** Fill in some reasonable defaults for the acq_config_t */
int init_acq_config(acq_config_t * cfg);
int read_acq_config(FILE *f, acq_config_t * cfg);
int dump_acq_config(FILE *f, const acq_config_t * cfg);


typedef struct xfer_config
{
  const char * hostname;
  const char * destdir;
  const char * dbfile;
  int copy_sleep_amt;
  int collect_sleep_amt;

} xfer_config_t;


#endif
