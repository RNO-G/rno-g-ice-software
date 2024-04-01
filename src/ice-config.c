#include "ice-config.h" 
#include <libconfig.h> 
#include <stdlib.h> 
#include <stdio.h> 
#include <string.h>

/** This implements configuration of the DAQ.  This is nominally done with
 * libconfig, although this makes heavy use of macros to form a horrific DSL.
 * The output is done "by hand" with macros because that way we can include
 * comments.
 *
 * If you want to add a new setting, first add it to the relevant struct in the
 * header file, then you'll need to put a default value in init_X_config, add a
 * lookup in read_X_config and a write out in write_X_config (including a
 * comment). For most simple data types this should be straightforward... 
 *
 */


int init_acq_config(acq_config_t * cfg) 
{

  //output 
  // Yes, I really am this lazy 
  // to avoid lots of typing we'll keep #defining and #undefing the section
#define SECT cfg->output

  SECT.base_dir = "/data/daq/"; 
  SECT.runfile = "/rno-g/var/runfile"; 
  SECT.max_events_per_file = 100; 
  SECT.max_daqstatuses_per_file= 100; 
  SECT.max_seconds_per_file = 60; 
  SECT.max_kB_per_file = 0; 
  SECT.min_free_space_MB_output_partition = 512; 
  SECT.min_free_space_MB_runfile_partition = 64; 
  SECT.allow_rundir_overwrite = 0; 
  SECT.print_interval = 5; 
  SECT.daqstatus_interval = 1; 
  SECT.seconds_per_run = 7200; 
  SECT.comment = ""; 
    
#undef SECT
#define SECT cfg->runtime

  SECT.status_shmem_file = "/rno-g/run/daqstatus.dat" ; 
  SECT.acq_buf_size = 256; 
  SECT.mon_buf_size = 128; 

#undef SECT
#define SECT cfg->lt.gain
  SECT.auto_gain=1; 
  SECT.target_rms=5; 
  for (int i = 0; i < RNO_G_NUM_LT_CHANNELS; i++) 
  {
    SECT.fixed_gain_codes[i] =5;
  }

  //lt  (low-threshold) 
  //
#undef SECT
#define SECT cfg->lt.device
  SECT.spi_enable_gpio = 0;
  SECT.spi_device = "/dev/spidev1.0" ;
  SECT.required = 1; 


#undef SECT
#define SECT cfg->lt.trigger
  SECT.vpp =1; 
  SECT.min_coincidence=2; 
  SECT.window = 5; 
  SECT.enable_rf_coinc_trigger = 0; 
  SECT.enable_rf_phased_trigger = 1; 
  SECT.rf_coinc_channel_mask=0xf;
  SECT.rf_phased_beam_mask=0xffff;

  SECT.enable_rf_trigger_sys_out =1;
  SECT.enable_rf_trigger_sma_out =0;

  SECT.enable_pps_trigger_sys_out =0;
  SECT.enable_pps_trigger_sma_out =0;
  SECT.pps_trigger_delay = 0;

#undef SECT
#define SECT cfg->lt.thresholds

  SECT.load_from_threshold_file = 1; 
  for (int i = 0; i < RNO_G_NUM_LT_CHANNELS; i++) 
  {
    SECT.initial_coinc_thresholds[i] = 30; 
  }
    for (int i = 0; i < RNO_G_NUM_LT_BEAMS; i++) 
  {
    SECT.initial_phased_thresholds[i] = 4000; 
  }
#undef SECT
#define SECT cfg->lt.servo
  SECT.enable = 1; 
  for (int i = 0; i < RNO_G_NUM_LT_CHANNELS; i++) 
  {
    SECT.coinc_scaler_goals[i] = 2500; 
  }
  for (int i = 0; i < RNO_G_NUM_LT_BEAMS; i++) 
  {
    SECT.phased_scaler_goals[i] = 100; 
  }
  SECT.servo_thresh_frac = 0.95; 
  SECT.phased_servo_thresh_frac = 0.7; 

  SECT.servo_thresh_offset = 0; 
  SECT.fast_scaler_weight = 0.3; 
  SECT.slow_scaler_weight = 0.7; 
  SECT.scaler_update_interval = 0.5; 
  SECT.servo_interval = 1; 
  SECT.subtract_gated = 0; 
  SECT.P = 0.0002;
  SECT.phased_P = 0.002; 

  SECT.I = 0; 
  SECT.D = 0; 


////RADIANT 
#undef SECT
#define SECT cfg->radiant.pps
  SECT.use_internal = 0; 
  SECT.sync_out = 0; 
  SECT.pps_holdoff =10; 


#undef SECT
#define SECT cfg->radiant.device

  //hah, this does nothing so far
  SECT.reset_script = "/rno-g/bin/reset-radiant" ;
  SECT.spi_device = "/dev/spidev0.0"; 
  SECT.uart_device = "/dev/ttyRadiant"; 
  SECT.poll_gpio = 46 ; 
  SECT.spi_enable_gpio = -61 ; 

  //radiant analog
#undef SECT
#define SECT cfg->radiant.analog
  SECT.apply_lab4_vbias = 1; 
  SECT.lab4_vbias[0] = 1.5; 
  SECT.lab4_vbias[1] = 1.5; 
  SECT.apply_diode_vbias = 0; 
  SECT.apply_attenuations = 1; 
  SECT.settle_time = 0.5; 

  for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++) 
  {
    SECT.diode_vbias[i] = 1.25; 
    SECT.digi_attenuation[i] = 0; 
    SECT.trig_attenuation[i] = 0; 
  }

  //radiant pedestals 
#undef SECT
#define SECT cfg->radiant.pedestals

  SECT.compute_at_start = 1; 
  SECT.ntriggers_per_computation = 512; 
  SECT.apply_attenuation = 1; 
  SECT.attenuation = 31.75; 
  SECT.pedestal_file = "/rno-g/var/peds.dat"; 
  SECT.pedestal_subtract = 1; 
  SECT.ntriggers_per_cycle = 1; 
  SECT.sleep_per_cycle = 1e-3; 

  //radiant readout
#undef SECT
#define SECT cfg->radiant.readout
  SECT.readout_mask = 0xffffff; 
  SECT.nbuffers_per_readout = 2; 
  SECT.poll_ms = 10; 

  //radiant trigger
#undef SECT
#define SECT cfg->radiant.trigger
  SECT.clear_mode = 0; 
  SECT.output_enabled =1; 

  //Upward Surface
  SECT.RF[0].enabled = 1; 
  SECT.RF[0].mask =  0x092000; //upward pointing LPDAs
  SECT.RF[0].window = 50 ; // ?!?? 
  SECT.RF[0].num_coincidences = 2; 
<<<<<<< HEAD
  SECT.RF[0].readout_delay=1014; //delay 19*(53.3ns)=1013.3ns
=======
  SECT.RF[0].readout_delay=1014; //delay 19*(53.3ns)=1013.3ns
>>>>>>> add_readout_delay_settings
  SECT.RF[0].readout_delay_mask=0b1011; //delay all power and helper strings. not surface

  //Downward Surface
  SECT.RF[1].enabled = 1; 
  SECT.RF[1].mask = 0x16d000; // downward pointing LPDAs
  SECT.RF[1].window = 50; 
  SECT.RF[1].num_coincidences = 2; 
  SECT.RF[1].readout_delay=587; //delay 11*(53.333ns)=586.666ns
  SECT.RF[1].readout_delay_mask=0b1011; //delay all power and helper strings. not surface


  //LT 
  SECT.ext.enabled = 1; 

  //SOFT trigger
  SECT.soft.enabled = 1; 
  SECT.soft.use_exponential_distribution = 0; 
  SECT.soft.interval = 10; 
  SECT.soft.interval_jitter = 0; 
  SECT.soft.output_enabled = 0; 

  //PPS trigger
  SECT.pps.enabled = 0; 
  SECT.pps.output_enabled = 0; 

  //SERVO 

#undef SECT
#define SECT cfg->radiant.servo 

  SECT.enable = 1; 
  SECT.scaler_update_interval = 0.5; 
  SECT.servo_interval = 1; 
  for (int i = 0; i < NUM_SERVO_PERIODS; i++) 
  {
    SECT.nscaler_periods_per_servo_period[i] = i+1; 
    SECT.period_weights[i] = i == 0 ? 1: 0; 
  }
  for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++) 
  {
    SECT.scaler_goals[i] = ( (1<<i) & 0x1ff000) ? 1 : 5; // noise avoiding! 
  }
  SECT.P = 5; 
  SECT.I = 0; 
  SECT.D = 0; 
  SECT.max_thresh_change = 0.01; 
  SECT.max_sum_err = 10000; 


#undef SECT 
#define SECT cfg->radiant.thresholds

  SECT.min = 0.5; 
  SECT.max = 1.45; 
  SECT.load_from_threshold_file = 1; 
  for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++) 
  {
    SECT.initial[i]=  1.05;  // 1.05 V 
  }

#undef SECT 
#define SECT cfg->radiant.scalers
  SECT.use_pps = 1; 
  SECT.period = 1; 
  for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++) 
  {
    SECT.prescal_m1[i]=  0; 
  }
#undef SECT 

#define SECT cfg->radiant.bias_scan 
  SECT.enable_bias_scan = 0; 
  SECT.skip_runs = 13; 
  SECT.min_val = 0; 
  SECT.step_val = 16; 
  SECT.max_val = 3072; 
  SECT.navg_per_step = 512; 
  SECT.sleep_time = 1; 
  SECT.apply_attenuation = 1; 
  SECT.attenuation = 31.75;

#undef SECT 
#define SECT cfg->calib
  SECT.enable_cal = 0; 
  SECT.i2c_bus = 2; 
  SECT.gpio = 49; 
  SECT.rev = "/REV"; 
  SECT.channel= RNO_G_CAL_NO_OUTPUT; 
  SECT.type = RNO_G_CAL_NO_SIGNAL; 
  SECT.atten = 31.5; 

  SECT.sweep.enable = 0; 
  SECT.sweep.start_atten = 31.5; 
  SECT.sweep.stop_atten = 0; 
  SECT.sweep.atten_step = 0.5; 
  SECT.sweep.step_time = 100; 
#undef SECT

  return 0;
}

#define LOOKUP_INT(X) \
 config_lookup_int(&config, #X, &cfg->X);  

#define LOOKUP_INT_RENAME(X,Y) \
 config_lookup_int(&config, #Y, &cfg->X);  

static const char * dummy_enum_str; 
#define LOOKUP_ENUM(PATH, X, TYPE, STRS) \
  if (CONFIG_TRUE==config_lookup_string(&config, #PATH "." #X, &dummy_enum_str)){ \
  int found_##X = 0;\
  for (unsigned istr = 0; istr < sizeof(STRS)/sizeof(*STRS); istr++) \
  {\
    if (!strcasecmp(STRS[istr], dummy_enum_str))\
    {\
      found_##X = 1;\
      cfg->PATH.X = (TYPE) istr; break; \
    }\
  }\
  if (!found_##X) \
  {\
    fprintf(stderr,#PATH "." #X " not valid. Got \"%s\". Valid values: [", dummy_enum_str); \
    for (unsigned istr = 0; istr < sizeof(STRS)/sizeof(*STRS); istr++) \
    {\
      fprintf(stderr, " \"%s\" ", STRS[istr]);\
    }\
    fprintf(stderr,"]\n");\
  }}

static config_setting_t * dummy_setting, *dummy_setting2; 
#define LOOKUP_INT_ELEM(X,i) \
   dummy_setting = config_lookup(&config,#X); \
  if (dummy_setting) { \
    dummy_setting2 = config_setting_get_elem(dummy_setting,i); \
    if (dummy_setting2 && config_setting_is_number(dummy_setting2)) {cfg->X[i] = config_setting_get_int(dummy_setting2);} }



static int dummy_ival;
//TODO: complain if out of range
#define LOOKUP_UINT8_ELEM(X,i) \
   dummy_setting = config_lookup(&config,#X); \
  if (dummy_setting) { \
    dummy_setting2 = config_setting_get_elem(dummy_setting,i); \
    if (dummy_setting2 && config_setting_is_number(dummy_setting2)) {cfg->X[i] = (uint8_t) config_setting_get_int(dummy_setting2);} }



#define LOOKUP_FLOAT_ELEM(X,i) \
  dummy_setting = config_lookup(&config,#X); \
  if (dummy_setting) { \
    dummy_setting2 = config_setting_get_elem(dummy_setting,i); \
    if (dummy_setting2 && config_setting_is_number(dummy_setting2)) {cfg->X[i] = config_setting_get_float(dummy_setting2);} }




#define LOOKUP_UINT(X) \
 config_lookup_int(&config, #X, &dummy_ival); \
 cfg->X = (uint32_t) dummy_ival; 

#define LOOKUP_UINT_RENAME(X,Y) \
 if (CONFIG_TRUE == config_lookup_int(&config, #Y, &dummy_ival)){\
 cfg->X = (uint32_t) dummy_ival; }


static const char * dummy_str; 
#define LOOKUP_STRING(PATH,X) \
  static char * X;\
  if (X) free(X);\
  if (CONFIG_TRUE==config_lookup_string(&config, #PATH "." #X, &dummy_str)){\
  X = strdup(dummy_str); \
  cfg->PATH.X = X; }

static double dummy_val; 
#define LOOKUP_FLOAT(X) \
  if (CONFIG_TRUE==config_lookup_float(&config,#X, &dummy_val)){ \
  cfg->X = dummy_val;}

#define LOOKUP_FLOAT_RENAME(X,Y) \
  if (CONFIG_TRUE==config_lookup_float(&config,#Y, &dummy_val)){ \
  cfg->X = dummy_val;}



//define enum string arrays here 
const char * calpulser_outs[] = RNO_G_CALPULSER_OUT_STRS; 
const char * calpulser_modes[] = RNO_G_CALPULSER_MODE_STRS; 


int read_acq_config(FILE * f, acq_config_t * cfg) 
{
  config_t config; 
  config_init(&config); 
  config_set_auto_convert(&config, 1); 
  if (!config_read(&config, f))
  {
    fprintf(stderr,"Trouble reading config: %s, line: %d\n", config_error_text(&config), config_error_line(&config)); 
    return -1; 
  }

  //OUTPUT


  LOOKUP_STRING(output,base_dir); 
  LOOKUP_STRING(output,runfile); 
  LOOKUP_STRING(output,comment); 

  LOOKUP_INT(output.seconds_per_run); 
  LOOKUP_INT(output.max_events_per_file); 
  LOOKUP_INT(output.max_daqstatuses_per_file); 
  LOOKUP_INT(output.max_seconds_per_file); 
  LOOKUP_INT(output.max_kB_per_file); 
  LOOKUP_INT(output.print_interval); 
  LOOKUP_FLOAT(output.daqstatus_interval); 
  LOOKUP_INT(output.min_free_space_MB_output_partition); 
  LOOKUP_INT(output.min_free_space_MB_runfile_partition); 
  LOOKUP_INT(output.allow_rundir_overwrite); 


  //RADIANT

  //pps 
  LOOKUP_INT(radiant.pps.use_internal); 
  LOOKUP_INT(radiant.pps.sync_out); 
  LOOKUP_INT(radiant.pps.pps_holdoff); 

  //device 
  LOOKUP_STRING(radiant.device,reset_script); 
  {
    LOOKUP_STRING(radiant.device,spi_device); 
  }
  LOOKUP_STRING(radiant.device,uart_device); 
  LOOKUP_INT(radiant.device.poll_gpio); 
  LOOKUP_INT(radiant.device.spi_enable_gpio); 

  //analog
  LOOKUP_INT(radiant.analog.apply_lab4_vbias); 
  LOOKUP_INT(radiant.analog.apply_diode_vbias); 
  LOOKUP_INT(radiant.analog.apply_attenuations); 
  LOOKUP_FLOAT(radiant.analog.settle_time); 
  LOOKUP_FLOAT_ELEM(radiant.analog.lab4_vbias,0); 
  LOOKUP_FLOAT_ELEM(radiant.analog.lab4_vbias,1); 


  for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++) 
  {
    LOOKUP_FLOAT_ELEM(radiant.analog.diode_vbias,i);
    LOOKUP_FLOAT_ELEM(radiant.analog.digi_attenuation,i);
    LOOKUP_FLOAT_ELEM(radiant.analog.trig_attenuation,i);
  }

  //pedestals 
  LOOKUP_INT(radiant.pedestals.compute_at_start); 
  LOOKUP_INT(radiant.pedestals.ntriggers_per_computation); 
  LOOKUP_INT(radiant.pedestals.apply_attenuation); 
  LOOKUP_FLOAT(radiant.pedestals.attenuation); 
  LOOKUP_STRING(radiant.pedestals,pedestal_file); 
  LOOKUP_INT(radiant.pedestals.pedestal_subtract); 
  LOOKUP_INT(radiant.pedestals.ntriggers_per_cycle); 
  LOOKUP_FLOAT(radiant.pedestals.sleep_per_cycle); 

  //readout 
  LOOKUP_UINT(radiant.readout.readout_mask); 
  LOOKUP_INT(radiant.readout.nbuffers_per_readout);
  LOOKUP_INT(radiant.readout.poll_ms); 

  //trigger
  LOOKUP_INT(radiant.trigger.clear_mode); 
  LOOKUP_INT(radiant.trigger.output_enabled); 
  LOOKUP_INT_RENAME(radiant.trigger.RF[0].enabled, radiant.trigger.RF0.enabled);
  LOOKUP_INT_RENAME(radiant.trigger.RF[1].enabled, radiant.trigger.RF1.enabled);
  LOOKUP_UINT_RENAME(radiant.trigger.RF[0].mask, radiant.trigger.RF0.mask);
  LOOKUP_UINT_RENAME(radiant.trigger.RF[1].mask, radiant.trigger.RF1.mask);
  LOOKUP_FLOAT_RENAME(radiant.trigger.RF[0].window, radiant.trigger.RF0.window);
  LOOKUP_FLOAT_RENAME(radiant.trigger.RF[1].window, radiant.trigger.RF1.window);  
  LOOKUP_UINT_RENAME(radiant.trigger.RF[0].readout_delay, radiant.trigger.RF0.readout_delay);
  LOOKUP_UINT_RENAME(radiant.trigger.RF[1].readout_delay, radiant.trigger.RF1.readout_delay);
  LOOKUP_UINT_RENAME(radiant.trigger.RF[0].readout_delay_mask, radiant.trigger.RF0.readout_delay_mask);
  LOOKUP_UINT_RENAME(radiant.trigger.RF[1].readout_delay_mask, radiant.trigger.RF1.readout_delay_mask);
  LOOKUP_INT_RENAME(radiant.trigger.RF[0].num_coincidences, radiant.trigger.RF0.num_coincidences)
  LOOKUP_INT_RENAME(radiant.trigger.RF[1].num_coincidences, radiant.trigger.RF1.num_coincidences)
  LOOKUP_INT(radiant.trigger.pps.enabled);
  LOOKUP_INT(radiant.trigger.pps.output_enabled);
  LOOKUP_INT(radiant.trigger.ext.enabled);
  LOOKUP_INT(radiant.trigger.soft.enabled);
  LOOKUP_INT(radiant.trigger.soft.use_exponential_distribution);
  LOOKUP_FLOAT(radiant.trigger.soft.interval);
  LOOKUP_FLOAT(radiant.trigger.soft.interval_jitter);
  LOOKUP_INT(radiant.trigger.soft.output_enabled);

  //servo 
  LOOKUP_INT(radiant.servo.enable);
  LOOKUP_FLOAT(radiant.servo.scaler_update_interval);
  LOOKUP_FLOAT(radiant.servo.servo_interval);
  for (int i = 0; i < NUM_SERVO_PERIODS; i++) 
  {
    LOOKUP_INT_ELEM(radiant.servo.nscaler_periods_per_servo_period,i);
    LOOKUP_FLOAT_ELEM(radiant.servo.period_weights,i);
  }
  for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++) 
  {
    LOOKUP_FLOAT_ELEM(radiant.servo.scaler_goals,i);
  }
  LOOKUP_FLOAT(radiant.servo.P);
  LOOKUP_FLOAT(radiant.servo.I);
  LOOKUP_FLOAT(radiant.servo.D);
  LOOKUP_FLOAT(radiant.servo.max_thresh_change); 
  LOOKUP_FLOAT(radiant.servo.max_sum_err); 


  //thresholds
  LOOKUP_INT(radiant.thresholds.load_from_threshold_file); 
  for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++) 
  {
    LOOKUP_FLOAT_ELEM(radiant.thresholds.initial,i); 
  }
  LOOKUP_FLOAT(radiant.thresholds.min); 
  LOOKUP_FLOAT(radiant.thresholds.max); 

  //scalers
  LOOKUP_INT(radiant.scalers.use_pps); 
  LOOKUP_FLOAT(radiant.scalers.period); 
  for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++) 
  {
    LOOKUP_UINT8_ELEM(radiant.scalers.prescal_m1,i); 
  }

  //bias scan
  LOOKUP_INT(radiant.bias_scan.enable_bias_scan); 
  LOOKUP_INT(radiant.bias_scan.skip_runs); 
  LOOKUP_INT(radiant.bias_scan.min_val); 
  LOOKUP_INT(radiant.bias_scan.step_val); 
  LOOKUP_INT(radiant.bias_scan.max_val); 
  LOOKUP_INT(radiant.bias_scan.navg_per_step); 
  LOOKUP_FLOAT(radiant.bias_scan.sleep_time); 
  LOOKUP_INT(radiant.bias_scan.apply_attenuation); 
  LOOKUP_FLOAT(radiant.bias_scan.attenuation); 



  //runtime
  LOOKUP_STRING(runtime,status_shmem_file); 
  LOOKUP_INT(runtime.acq_buf_size); 
  LOOKUP_INT(runtime.mon_buf_size); 

  //LT 
  LOOKUP_INT(lt.trigger.vpp);
  //for backwards compatibility 
  LOOKUP_INT_RENAME(lt.trigger.enable_rf_phased_trigger, lt.trigger.phased_enable);
  LOOKUP_INT_RENAME(lt.trigger.enable_rf_coinc_trigger, lt.trigger.coinc_enable);

  LOOKUP_INT(lt.trigger.enable_rf_phased_trigger);
  LOOKUP_INT(lt.trigger.enable_rf_coinc_trigger);

  LOOKUP_INT(lt.trigger.rf_phased_beam_mask);
  LOOKUP_INT(lt.trigger.rf_coinc_channel_mask);

  LOOKUP_INT(lt.trigger.min_coincidence);
  LOOKUP_INT(lt.trigger.window);
  LOOKUP_INT(lt.trigger.enable_pps_trigger_sys_out);
  LOOKUP_INT(lt.trigger.enable_pps_trigger_sma_out);
  LOOKUP_INT(lt.trigger.enable_rf_trigger_sys_out);
  LOOKUP_INT(lt.trigger.enable_rf_trigger_sma_out);
  LOOKUP_FLOAT(lt.trigger.pps_trigger_delay);

  LOOKUP_INT(lt.thresholds.load_from_threshold_file);

  for (int i = 0; i < RNO_G_NUM_LT_CHANNELS; i++) 
  {
    LOOKUP_INT_ELEM(lt.thresholds.initial_coinc_thresholds,i);
    LOOKUP_INT_ELEM(lt.servo.coinc_scaler_goals,i);
    LOOKUP_INT_ELEM(lt.gain.fixed_gain_codes,i); 
  }

  for (int i = 0; i < RNO_G_NUM_LT_BEAMS; i++) 
  {
    LOOKUP_INT_ELEM(lt.thresholds.initial_phased_thresholds,i);
    LOOKUP_INT_ELEM(lt.servo.phased_scaler_goals,i);
  }

  LOOKUP_INT(lt.servo.subtract_gated);
  LOOKUP_FLOAT(lt.servo.servo_thresh_frac);
  LOOKUP_FLOAT(lt.servo.phased_servo_thresh_frac);

  LOOKUP_FLOAT(lt.servo.servo_thresh_offset);
  LOOKUP_FLOAT(lt.servo.servo_interval);
  LOOKUP_FLOAT(lt.servo.scaler_update_interval);
  LOOKUP_FLOAT(lt.servo.enable);
  LOOKUP_FLOAT(lt.servo.fast_scaler_weight); 
  LOOKUP_FLOAT(lt.servo.slow_scaler_weight); 
  LOOKUP_FLOAT(lt.servo.P);
  LOOKUP_FLOAT(lt.servo.phased_P);

  LOOKUP_FLOAT(lt.servo.I);
  LOOKUP_FLOAT(lt.servo.D);

  LOOKUP_INT(lt.device.spi_enable_gpio); 
  LOOKUP_INT(lt.device.required); 

  {
    LOOKUP_STRING(lt.device, spi_device); 
  }

  LOOKUP_INT(lt.gain.auto_gain);
  LOOKUP_FLOAT(lt.gain.target_rms); 

  LOOKUP_INT(calib.enable_cal); 
  LOOKUP_INT(calib.i2c_bus); 
  LOOKUP_INT(calib.gpio); 
  LOOKUP_STRING(calib,rev); 
  LOOKUP_ENUM(calib,channel, rno_g_calpulser_out_t, calpulser_outs); 
  LOOKUP_ENUM(calib,type, rno_g_calpulser_mode_t, calpulser_modes); 
  LOOKUP_FLOAT(calib.atten); 

  LOOKUP_INT(calib.sweep.enable); 
  LOOKUP_FLOAT(calib.sweep.start_atten);
  LOOKUP_FLOAT(calib.sweep.stop_atten);
  LOOKUP_FLOAT(calib.sweep.atten_step);
  LOOKUP_INT(calib.sweep.step_time); 

  config_destroy(&config); 
  return 0; 
}



int dump_acq_config(FILE *f, const acq_config_t * cfg) 
{
  int indent_level = 0; 
  const char * indents[] = { "", "\t", "\t\t", "\t\t\t", "\t\t\t\t", "\t\t\t\t\t"};  

#undef SECT 
#define SECT(X, COMMENT) fprintf(f,"%s//%s\n%s%s:\n%s{\n",indents[indent_level],COMMENT, indents[indent_level], #X, indents[indent_level]); indent_level++; 
#define UNSECT() indent_level--; fprintf(f,"%s};\n\n", indents[indent_level]); 
#define WRITE_VAL(X,Y,COMMENT,TYPE) fprintf(f,"%s//%s\n%s%s=" TYPE ";\n", indents[indent_level], COMMENT, indents[indent_level], #Y, cfg->X.Y); 
#define WRITE_INT(X,Y,COMMENT) WRITE_VAL(X,Y,COMMENT,"%d")
#define WRITE_UINT(X,Y,COMMENT) WRITE_VAL(X,Y,COMMENT,"%u")
#define WRITE_HEX(X,Y,COMMENT) WRITE_VAL(X,Y,COMMENT,"0x%x")
#define WRITE_FLT(X,Y,COMMENT) WRITE_VAL(X,Y,COMMENT,"%g")
#define WRITE_STR(X,Y,COMMENT) WRITE_VAL(X,Y,COMMENT,"\"%s\"")
#define WRITE_ARR(X,Y,COMMENT,LEN,TYPE) \
  fprintf(f,"%s//%s\n%s%s=[", indents[indent_level], COMMENT, indents[indent_level], #Y ); \
  for (int i = 0; i < LEN; i++) fprintf(f, TYPE "%s", cfg->X.Y[i], i < LEN-1 ? ",": ""); \
  fprintf(f,"];\n"); 

#define WRITE_ENUM(X,Y,COMMENT, STRS)\
  fprintf(f,"%s//%s", indents[indent_level], COMMENT); \
  fprintf(f," [ valid values: "); \
  for (unsigned istr = 0; istr < sizeof(STRS)/sizeof(*STRS); istr++) fprintf(f," \"%s\" ", STRS[istr]); \
  fprintf(f, "]\n"); \
  unsigned index_##X##Y= (int) cfg->X.Y; \
  if (index_##X##Y>= sizeof(STRS)/sizeof(*STRS)) index_##X##Y= 0; \
  fprintf(f,"%s%s=\"%s\";\n", indents[indent_level],#Y,STRS[index_##X##Y]); 

  fprintf(f,"//////////////////////////////////////////////////////////////////////////////////////////////////////\n");
  fprintf(f,"// Main configuration file for rno-g-acq (typically /rno-g/cfg/acq.cfg is used)\n"); 
  fprintf(f,"// This file is in libconfig format, though your syntax highligher might mistake it for json\n"); 
  fprintf(f,"// Changing values in this file may adversely affect the operation of the DAQ.\n"); 
  fprintf(f,"// If you don't know what you're doing now would be a good time to exit your text editor. \n"); 
  fprintf(f,"//////////////////////////////////////////////////////////////////////////////////////////////////////\n\n");
  


 SECT(radiant,"RADIANT configuration"); 
   SECT(scalers,"Scalers configuration");
     WRITE_INT(radiant.scalers,use_pps,"use PPS, otherwise period is used"); 
     WRITE_FLT(radiant.scalers,period,"The period used for scalers if PPS is not enabled"); 
     WRITE_ARR(radiant.scalers,prescal_m1, "The prescaler minus 1 for each channel", RNO_G_NUM_RADIANT_CHANNELS, "%u"); 
   UNSECT(); 
   SECT(thresholds,"Threshold initialization configuration"); 
    WRITE_INT(radiant.thresholds, load_from_threshold_file, "1 to load from threshold file, otherwise initial values will be used"); 
    WRITE_ARR(radiant.thresholds,initial,"Initial thresholds if not loaded from file (in V)", RNO_G_NUM_RADIANT_CHANNELS, "%g"); 
    WRITE_FLT(radiant.thresholds, min, "Minimum allowed threshold, in V"); 
    WRITE_FLT(radiant.thresholds, max, "Maximum allowed threshold, in V"); 
   UNSECT(); 

   SECT(servo, "Threshold servo configuration"); 
    WRITE_INT(radiant.servo, enable, "Enable servoing of RADIANT thresholds");
    WRITE_FLT(radiant.servo, scaler_update_interval, "Time interval (in seconds) that scalers are updated at"); 
    WRITE_FLT(radiant.servo, servo_interval, "Time interval (in seconds) that thresholds are updated at"); 
    WRITE_ARR(radiant.servo, nscaler_periods_per_servo_period, 
               "Multiple time periods may be considered in servoing. This sets the length of each time period (" NUM_SERVO_PERIODS_STR " periods must be defined)", NUM_SERVO_PERIODS, "%d" ); 
    WRITE_ARR(radiant.servo, period_weights, 
               "The weights of the aforementioned periods. For scaler goal to mean something sensible, these should add to 1.", NUM_SERVO_PERIODS, "%g" ); 
    WRITE_ARR(radiant.servo, scaler_goals, 
               "The scaler goal for each channel (calculated as the weighted contribution of periods)", RNO_G_NUM_RADIANT_CHANNELS, "%g" ); 
    WRITE_FLT(radiant.servo, max_thresh_change, "The maximum amount the threshold can change by in each step"); 
    WRITE_FLT(radiant.servo,P,"servo PID loop P");
    WRITE_FLT(radiant.servo,I,"servo PID loop I");
    WRITE_FLT(radiant.servo,D,"servo PID loop D");
    WRITE_FLT(radiant.servo, max_sum_err, "Maximum allowed error sum (in Hz)"); 
  UNSECT(); 

  SECT(trigger,"Trigger configuration"); 
    SECT(soft,"Software trigger configuration"); 
      WRITE_INT(radiant.trigger.soft,enabled,"Enable soft trigger"); 
      WRITE_INT(radiant.trigger.soft,use_exponential_distribution,"Use exponential distribution of inter-soft trigger timing"); 
      WRITE_FLT(radiant.trigger.soft,interval,"Soft trigger interval"); 
      WRITE_FLT(radiant.trigger.soft,interval_jitter,"Jitter (uniform) on soft trigger interval"); 
      WRITE_INT(radiant.trigger.soft,output_enabled,"Enable output for soft trigger"); 
    UNSECT(); 
    SECT(ext,"External (Low-threshold!) trigger configuration") ;
      WRITE_INT(radiant.trigger.ext,enabled,"Enable ext trigger (note: this is the low threshold trigger!)"); 
    UNSECT(); 
    SECT(pps,"PPS trigger configuration"); 
      WRITE_INT(radiant.trigger.pps,enabled,"Enable PPS trigger"); 
      WRITE_INT(radiant.trigger.pps,output_enabled,"Enable PPS trigger output"); 
    UNSECT(); 
    SECT(RF0,"First RF trigger configuration"); 
      WRITE_INT(radiant.trigger.RF[0],enabled,"Enable this RF trigger"); 
      WRITE_HEX(radiant.trigger.RF[0],mask,"Mask of channels that go into this trigger"); 
      WRITE_FLT(radiant.trigger.RF[0],window,"The time window (in ns) for the coincidence  trigger"); 
      WRITE_INT(radiant.trigger.RF[0],num_coincidences,"Number of coincidences (min 1) in this coincidence trigger"); 
      WRITE_INT(radiant.trigger.RF[0],readout_delay,"Time delay (in ns) to delay readout of channels in group mask");
      WRITE_INT(radiant.trigger.RF[0],readout_delay_mask,"Group mask of which channels will be delayed on this trigger");

    UNSECT()
    SECT(RF1,"Second RF trigger configuration"); 
      WRITE_INT(radiant.trigger.RF[1],enabled,"Enable this RF trigger"); 
      WRITE_HEX(radiant.trigger.RF[1],mask,"Mask of channels that go into this trigger"); 
      WRITE_FLT(radiant.trigger.RF[1],window,"The time window (in ns) for the coincidence  trigger"); 
      WRITE_INT(radiant.trigger.RF[1],num_coincidences,"Number of coincidences (min 1) in this coincidence trigger"); 
      WRITE_INT(radiant.trigger.RF[1],readout_delay,"Time delay (in ns) to delay readout of channels in group mask");
      WRITE_INT(radiant.trigger.RF[1],readout_delay_mask,"Group mask of which channels will be delayed on this trigger");
    UNSECT()

    WRITE_INT(radiant.trigger,clear_mode,"Enable clear mode (don't...)"); 
    WRITE_INT(radiant.trigger,output_enabled,"Enable trigger output"); 
  UNSECT(); 
  SECT(readout,"Readout settings for the RADIANT"); 
    WRITE_HEX(radiant.readout,readout_mask , "Mask of channels to read (0xffffff for all)"); 
    WRITE_INT(radiant.readout,nbuffers_per_readout,"The number of 1024-sample buffers per readout. Use 1 or 2..."); 
    WRITE_INT(radiant.readout,poll_ms,"Timeout in ms for gpio poll (higher reduces CPU, but reduces soft trigger granularity"); 
  UNSECT();
  SECT(pedestals,"Pedestal settings for RADIANT"); 
    WRITE_INT(radiant.pedestals,compute_at_start,"Compute pedestals at start of run"); 
    WRITE_INT(radiant.pedestals,ntriggers_per_computation,"Number of triggers used to compute pedetsal"); 
    WRITE_INT(radiant.pedestals,apply_attenuation,"Apply attenuation when computing pedestals"); 
    WRITE_FLT(radiant.pedestals,attenuation,"Amount of attenuation to apply when computing pedestals"); 
    WRITE_STR(radiant.pedestals,pedestal_file,"File to load / store pedestals from / to"); 
    WRITE_INT(radiant.pedestals,pedestal_subtract,"Subtract pedestals from waveforms"); 
    WRITE_INT(radiant.pedestals,ntriggers_per_cycle,"Number of internal triggers taken at once during pedestals (or bias scans)"); 
    WRITE_FLT(radiant.pedestals,sleep_per_cycle,"Time to sleep (in seconds) between ntriggers_per_cycle. Typical values might be 1e-6 to 1e-2."); 
  UNSECT(); 
  SECT(analog,"Analog settings for the RADIANT"); 
    WRITE_INT(radiant.analog,apply_lab4_vbias, "Apply lab4 vbias at beginning of run (instead of using whatever it is)"); 
    WRITE_ARR(radiant.analog,lab4_vbias,"The lab4 vbias (in V) to apply", 2, "%g"); 
    WRITE_INT(radiant.analog,apply_diode_vbias, "Apply diode vbias at beginning of run (instead of using whatever it is)"); 
    WRITE_ARR(radiant.analog,diode_vbias,"The diode vbias (in V) to apply", RNO_G_NUM_RADIANT_CHANNELS, "%g"); 
    WRITE_INT(radiant.analog,apply_attenuations,"Apply attenuations to digitizer/trigger paths"); 
    WRITE_ARR(radiant.analog,digi_attenuation,"Digitizer path attenuations (dB)", RNO_G_NUM_RADIANT_CHANNELS, "%g"); 
    WRITE_ARR(radiant.analog,trig_attenuation,"Trigger path attenuations (dB)", RNO_G_NUM_RADIANT_CHANNELS, "%g"); 
    WRITE_FLT(radiant.analog,settle_time,"Time to wait after setting analog settings"); 
  UNSECT(); 
  SECT(device,"RADIANT other device settings"); 
    WRITE_STR(radiant.device,reset_script, "Script to reset the radiant (not implemented yet, so this is merely aspirational)"); 
    WRITE_STR(radiant.device,spi_device,"SPI device for RADIANT DMA"); 
    WRITE_STR(radiant.device,uart_device,"UART device for RADIANT and RADIANT controller communications"); 
    WRITE_INT(radiant.device,poll_gpio,"gpio to poll on for new DMA transfers"); 
    WRITE_INT(radiant.device,spi_enable_gpio,"gpio to enable SPI (use negative value for active low)"); 
  UNSECT(); 
  SECT(pps,"RADIANT pps settings"); 
    WRITE_INT(radiant.pps,use_internal,"Use internal PPS instead of from GPS"); 
    WRITE_INT(radiant.pps,sync_out," Enable sync out"); 
    WRITE_INT(radiant.pps,pps_holdoff,"Amount of PPS holdoff (in some units...) for debouncing (I think?)"); 
  UNSECT(); 

  SECT(bias_scan, "Bias Scan Settings"); 
    WRITE_INT(radiant.bias_scan,enable_bias_scan,"Enable bias scan"); 
    WRITE_INT(radiant.bias_scan,skip_runs, "If >1, will only do a bias scan when run % skip_runs == 0"); 
    WRITE_INT(radiant.bias_scan,min_val, "Start DAC value (in adc) for bias scan"); 
    WRITE_INT(radiant.bias_scan,step_val, "DAC step value (in adc) for bias scan"); 
    WRITE_INT(radiant.bias_scan,max_val, "DAC step value (in adc) for bias scan"); 
    WRITE_INT(radiant.bias_scan,navg_per_step, "Number of averages per step"); 
    WRITE_FLT(radiant.bias_scan,sleep_time, "Number of seconds to sleep to settle"); 
    WRITE_INT(radiant.bias_scan,apply_attenuation, "Apply Attenuation during bias scan"); 
    WRITE_FLT(radiant.bias_scan,attenuation, "Attenuation to apply during bias scan"); 
  UNSECT(); 


 UNSECT() ; 

  SECT(lt,"Settings for the low-threshold (FLOWER) board"); 
    SECT(trigger,"Trigger settings for the low-threshold-board"); 
       WRITE_INT(lt.trigger,enable_rf_coinc_trigger, "Enable the LT RF trigger (currently a coincidence trigger)"); 
       WRITE_INT(lt.trigger,enable_rf_phased_trigger, "Enable the LT RF trigger (currently a coincidence trigger)"); 
       WRITE_INT(lt.trigger,rf_coinc_channel_mask, "Coincidence trigger channel mask"); 
       WRITE_INT(lt.trigger,rf_phased_beam_mask, "Phased trigger beam mask");
       WRITE_INT(lt.trigger,vpp, " Vpp threshold  (max 255) for RF Trigger"); 
       WRITE_INT(lt.trigger,min_coincidence,"Minimum coincidence threshold for channels (minimum 1) for RF trigger"); 
       WRITE_INT(lt.trigger,window,"Coincidence window for RF trigger"); 
       WRITE_INT(lt.trigger,enable_rf_trigger_sma_out,"Send RF trigger to SMA out"); 
       WRITE_INT(lt.trigger,enable_rf_trigger_sys_out,"Send RF trigger to system out (i.e. to RADIANT)"); 

       WRITE_INT(lt.trigger,enable_pps_trigger_sma_out,"Send PPS trigger to SMA out"); 
       WRITE_INT(lt.trigger,enable_pps_trigger_sys_out,"Send PPS trigger to system out (i.e. to RADIANT)"); 
       WRITE_FLT(lt.trigger,pps_trigger_delay,"The delay, in microseconds,of the PPS trigger relative to the GPS second. Will reounded to nearest 0.1 us. Can be negative to subtrract off from best estimate of current clock rate."); 
    UNSECT(); 

    SECT(thresholds,"Threshold settings for the low-threshold board"); 
       WRITE_INT(lt.thresholds,load_from_threshold_file,"Load thresholds from threshold file (if available)"); 
       WRITE_ARR(lt.thresholds,initial_coinc_thresholds,"Initial thresholds if not loaded from file (in ADC)",RNO_G_NUM_LT_CHANNELS,"%u"); 
       WRITE_ARR(lt.thresholds,initial_phased_thresholds,"Initial thresholds if not loaded from file (in ADC^2)",RNO_G_NUM_LT_BEAMS,"%u"); 

    UNSECT(); 
    SECT(servo, "Servo settings for the low-threshold board"); 
       WRITE_INT(lt.servo,enable,"Enable servoing"); 
       WRITE_INT(lt.servo,subtract_gated,"Subtract gated scalers"); 
       WRITE_ARR(lt.servo,coinc_scaler_goals,"",RNO_G_NUM_LT_CHANNELS,"%u"); 
       WRITE_ARR(lt.servo,phased_scaler_goals,"",RNO_G_NUM_LT_BEAMS,"%u"); 
       WRITE_FLT(lt.servo,servo_thresh_frac,"The servo threshold is related to the trigger threshold by a fraction and offset");
       WRITE_FLT(lt.servo,phased_servo_thresh_frac,"The phased servo threshold is related to the trigger threshold by a fraction and offset");
       WRITE_FLT(lt.servo,servo_thresh_offset,"The servo threshold is related to the trigger threshold by a fraction and offset");
       WRITE_FLT(lt.servo,fast_scaler_weight,"Weight of fast (1Hz?) scalers in calculating PID goal");
       WRITE_FLT(lt.servo,slow_scaler_weight,"Weight of slow (10Hz?) scalers in calculating PID goal");
       WRITE_FLT(lt.servo,scaler_update_interval,"How often we update the scalers");
       WRITE_FLT(lt.servo,servo_interval,"How often we run the scaler");
       WRITE_FLT(lt.servo,P,"PID loop P term");
       WRITE_FLT(lt.servo,phased_P,"Phased Trigger PID loop P term");
       WRITE_FLT(lt.servo,I,"PID loop I term");
       WRITE_FLT(lt.servo,D,"PID loop D term ");
    UNSECT(); 
    SECT(gain,"Settings related to HMCAD1511 gain"); 
      WRITE_INT(lt.gain,auto_gain,"Automatically use HMCAD1511 gain to equalize channels"); 
      WRITE_FLT(lt.gain,target_rms,"Target RMS (in adc) for normalization"); 
      WRITE_ARR(lt.gain,fixed_gain_codes,"If not using auto gain, give us the gain codes (see datasheet)", RNO_G_NUM_LT_CHANNELS, "%u"); 
    UNSECT(); 
    SECT(device,"Settings related to device interface"); 
      WRITE_STR(lt.device,spi_device,"The SPI device for the low-threshold board");
      WRITE_INT(lt.device,spi_enable_gpio,"gpio to enable SPI device");
      WRITE_INT(lt.device,required,"Require the low-threshold board to be detected for the DAQ to function. Turn this to 0 if you don't need it (usually for test-bench?)."); 
    UNSECT(); 
  UNSECT()

  SECT(runtime,"Runtime settings"); 
    WRITE_STR(runtime,status_shmem_file,"The file holding the current daqstatus");
    WRITE_INT(runtime,acq_buf_size,"acq circular buffer size (temporarily stores events between acquisition and writing to disk)");
    WRITE_INT(runtime,mon_buf_size,"monitoring circular buffer size (temporarily stores daqstatus between recording and writing to disk)");
  UNSECT();
   

  SECT(output,"Output settings"); 
    WRITE_STR(output,base_dir,"Base directory for writing out data");
    WRITE_STR(output,runfile,"The file used to persist the run");
    WRITE_STR(output,comment,"A human-readable comment that you can fill what whatever hopefully useful comment (or, an excuse not to take good notes?)");
    WRITE_FLT(output,daqstatus_interval,"Interval that daqstatus is written out. Some things are measured on this cadence  (e.g. calpulser temperature, radiant voltages) ");
    WRITE_INT(output,seconds_per_run,"Number of seconds per run");
    WRITE_INT(output,max_events_per_file,"Maximum number of events per event (and header) file, or 0 to ignore");
    WRITE_INT(output,max_daqstatuses_per_file,"Maximum daqstatuses per daqstatus file, or 0 to ignore");
    WRITE_INT(output,max_seconds_per_file,"Maximum seconds per file (or 0 to ignore)");
    WRITE_INT(output,max_kB_per_file,"Maximum kB per file (or 0 to ignore), not including any compression");
    WRITE_INT(output,min_free_space_MB_output_partition,"Minimum free space on the partition where data gets stored. ");
    WRITE_INT(output,min_free_space_MB_runfile_partition,"Minimum free space on the partition where the runfile gets stored");
    WRITE_INT(output,allow_rundir_overwrite,"Allow overwriting output directories (only effective if there's a runfile)");
    WRITE_INT(output,print_interval,"Interval for printing a bunch of stuff to a screen nobody will see. Ideally done in green text with The Matrix font...");
  UNSECT(); 

  SECT(calib, "In-situ Calibration settings"); 
    WRITE_INT(calib, enable_cal,"Enable in-situ pulser"); 
    WRITE_INT(calib, i2c_bus, "the calpulser i2c-bus"); 
    WRITE_INT(calib, gpio, "the calpulser control gpio"); 
    WRITE_STR(calib,rev, "the board revision (e.g. D or E), or the absolute path to the name of a file containing the board revision"); 
    WRITE_ENUM(calib, channel, "in-situ calpulser channel", calpulser_outs); 
    WRITE_ENUM(calib, type, "in-situ calpulser type", calpulser_modes); 
    WRITE_FLT(calib, atten,"Attenuation in dB (max 31.5, in steps of 0.5 dB)" ); 

    SECT(sweep, "Attenuation sweep settings") ; 
      WRITE_INT(calib.sweep,enable,"Enable sweeping of calpulser attenuation"); 
      WRITE_FLT(calib.sweep,start_atten,"Start attenuation of sweep"); 
      WRITE_FLT(calib.sweep,stop_atten,"Stop attenuation of sweep"); 
      WRITE_FLT(calib.sweep,atten_step,"Attenuation step of sweep"); 
      WRITE_INT(calib.sweep,step_time,"Length of step, in seconds"); 
    UNSECT(); 
  UNSECT(); 



 return 0;

}

