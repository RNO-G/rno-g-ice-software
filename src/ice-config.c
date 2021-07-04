#include "ice-config.h" 
#include <libconfig.h> 
#include <stdlib.h> 
#include <stdio.h> 
#include <string.h>


int init_acq_config(acq_config_t * cfg) 
{

  //output 
  // Yes, I really am this lazy 
#define SECT cfg->output

  SECT.base_dir = "/data/daq/"; 
  SECT.runfile = "/rno-g/var/runfile"; 
  SECT.max_events_per_file = 100; 
  SECT.max_daqstatuses_per_file= 100; 
  SECT.max_seconds_per_file = 60; 
  SECT.max_kB_per_file = 0; 
  SECT.min_free_space_MB = 512; 
  SECT.print_interval = 10; 
  SECT.daqstatus_interval = 5; 
  SECT.seconds_per_run = 7200; 
    
#undef SECT
#define SECT cfg->runtime

  SECT.status_shmem_file = "/rno-g/run/thresholds.dat" ; 
  SECT.acq_buf_size = 256; 
  SECT.mon_buf_size = 128; 


  //lt 
  //
#undef SECT
#define SECT cfg->lt.device
  SECT.spi_enable_gpio = 0;
  SECT.spi_device = "/dev/spidev1.0" ;


#undef SECT
#define SECT cfg->lt.trigger
  SECT.vpp =1; 
  SECT.min_coincidence=2; 
  SECT.window = 2; 
  SECT.enable = 1; 

#undef SECT
#define SECT cfg->lt.servo
  SECT.enable = 1; 
  SECT.load_from_threshold_file = 1; 
  for (int i = 0; i < RNO_G_NUM_LT_CHANNELS; i++) 
  {
    SECT.initial_trigger_thresholds[i] = 30; 
    SECT.scaler_goals[i] = 1000; 
  }
  SECT.servo_thresh_frac = 0.67; 
  SECT.servo_thresh_offset = -10; 
  SECT.fast_scaler_weight = 0.7; 
  SECT.slow_scaler_weight = 0.3; 
  SECT.scaler_update_interval = 0.5; 
  SECT.servo_interval = 1; 
  SECT.P = 0.5; 
  SECT.I = 0.5; 
  SECT.D = 0; 


////RADIANT 
#undef SECT
#define SECT cfg->radiant.pps
  SECT.use_internal = 0; 
  SECT.sync_out = 0; 
  SECT.pps_holdoff =10; 


#undef SECT
#define SECT cfg->radiant.device

  SECT.reset_script = "/rno-g/bin/reset-radiant" ;
  SECT.spi_device = "/dev/spidev0.0"; 
  SECT.uart_device = "/dev/ttyRadiant"; 
  SECT.poll_gpio = 46 ; 
  SECT.spi_enable_gpio = -61 ; 

  //radiant analog
#undef SECT
#define SECT cfg->radiant.analog
  SECT.apply_lab4_vbias = 0; 
  SECT.lab4_vbias[0] = 1.5; 
  SECT.lab4_vbias[1] = 1.5; 
  SECT.apply_diode_vbias = 0; 
  SECT.apply_attenuations = 0; 
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
  SECT.apply_attenuation = 0; 
  SECT.attenuation = 0; 
  SECT.pedestal_file = "/rno-g/var/peds.dat"; 
  SECT.pedestal_subtract = 1; 

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

  //SURFACE TRIGGER? 
  SECT.RF[0].enabled = 1; 
  SECT.RF[0].mask =  (1 << 12) | (1 << 13) | (1 << 14) | (1 << 15) | (1 << 16) | (1 << 17) | (1 << 18) | (1 <<19) | ( 1<< 20)   ; 
  SECT.RF[0].window = 30 ; // ?!?? 
  SECT.RF[0].num_coincidences = 3; 

  //DEEP TRIGGER? 
  SECT.RF[1].enabled = 1; 
  SECT.RF[1].mask = 0xf; // ??!? 
  SECT.RF[1].window = 20; 
  SECT.RF[1].num_coincidences = 2; 

  //LT 
  SECT.ext.enabled = 1; 

  //SOFT trigger
  SECT.soft.enabled = 1; 
  SECT.soft.use_exponential_distribution = 1; 
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
    SECT.period_weights[i] = 1; 
  }
  for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++) 
  {
    SECT.scaler_goals[i] = 5; // noise avoiding! 
  }
  SECT.P = 0.5; 
  SECT.I = 0.5; 
  SECT.D = 0; 
  SECT.max_thresh_change = 0.05; 


#undef SECT 
#define SECT cfg->radiant.thresholds

  SECT.load_from_threshold_file = 1; 
  for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++) 
  {
    SECT.initial[i]=  1.;  // 1 V 
  }

#undef SECT 
#define SECT cfg->radiant.scalers
  SECT.use_pps = 1; 
  SECT.period = 1; 
  for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++) 
  {
    SECT.prescal_m1[i]=  0; 
  }

  return 0;
}

#define LOOKUP_INT(X) \
 config_lookup_int(&config, #X, &cfg->X);  

#define LOOKUP_INT_ELEM(X,i) \
  cfg->X[i] = config_setting_get_int_elem(config_lookup(&config,#X),i);

static int dummy_ival;
//TODO: complain if out of range
#define LOOKUP_UINT8_ELEM(X,i) \
  cfg->X[i] = config_setting_get_int_elem(config_lookup(&config,#X),i);




#define LOOKUP_UINT(X) \
 config_lookup_int(&config, #X, &dummy_ival); \
 cfg->X = (uint32_t) dummy_ival; 


static const char * dummy_str; 
#define LOOKUP_STRING(PATH,X) \
  static char * X;\
  if (X) free(X);\
  config_lookup_string(&config, #PATH "." #X, &dummy_str);\
  X = strdup(dummy_str); \
  cfg->PATH.X = X; 

static double dummy_val; 
#define LOOKUP_FLOAT(X) \
  config_lookup_float(&config,#X, &dummy_val); \
  cfg->X = dummy_val;

#define LOOKUP_FLOAT_ELEM(X,i) \
  cfg->X[i] = config_setting_get_float_elem(config_lookup(&config,#X),i);




int read_acq_config(FILE * f, acq_config_t * cfg) 
{
  config_t config; 
  config_init(&config); 
  if (config_read(&config, f))
  {
    return -1; 
  }

  //OUTPUT


  LOOKUP_STRING(output,base_dir); 
  LOOKUP_STRING(output,runfile); 

  LOOKUP_INT(output.seconds_per_run); 
  LOOKUP_INT(output.max_events_per_file); 
  LOOKUP_INT(output.max_daqstatuses_per_file); 
  LOOKUP_INT(output.max_seconds_per_file); 
  LOOKUP_INT(output.max_kB_per_file); 
  LOOKUP_INT(output.print_interval); 
  LOOKUP_FLOAT(output.daqstatus_interval); 
  LOOKUP_INT(output.min_free_space_MB); 


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

  //readout 
  LOOKUP_UINT(radiant.readout.readout_mask); 
  LOOKUP_INT(radiant.readout.nbuffers_per_readout);
  LOOKUP_INT(radiant.readout.poll_ms); 

  //trigger
  LOOKUP_INT(radiant.trigger.clear_mode); 
  LOOKUP_INT(radiant.trigger.output_enabled); 
  LOOKUP_INT(radiant.trigger.RF[0].enabled)
  LOOKUP_INT(radiant.trigger.RF[1].enabled)
  LOOKUP_UINT(radiant.trigger.RF[0].mask)
  LOOKUP_UINT(radiant.trigger.RF[1].mask)
  LOOKUP_FLOAT(radiant.trigger.RF[0].window)
  LOOKUP_FLOAT(radiant.trigger.RF[1].window)
  LOOKUP_INT(radiant.trigger.RF[0].num_coincidences)
  LOOKUP_INT(radiant.trigger.RF[1].num_coincidences)
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


  //thresholds
  LOOKUP_INT(radiant.thresholds.load_from_threshold_file); 
  for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++) 
  {
    LOOKUP_FLOAT_ELEM(radiant.thresholds.initial,i); 
  }

  //scalers
  LOOKUP_INT(radiant.scalers.use_pps); 
  LOOKUP_FLOAT(radiant.scalers.period); 
  for (int i = 0; i < RNO_G_NUM_RADIANT_CHANNELS; i++) 
  {
    LOOKUP_UINT8_ELEM(radiant.scalers.prescal_m1,i); 
  }


  //runtime
  LOOKUP_STRING(runtime,status_shmem_file); 
  LOOKUP_INT(runtime.acq_buf_size); 
  LOOKUP_INT(runtime.mon_buf_size); 

  //LT 
  LOOKUP_INT(lt.trigger.vpp);
  LOOKUP_INT(lt.trigger.enable);
  LOOKUP_INT(lt.trigger.min_coincidence);
  LOOKUP_INT(lt.trigger.window);

  LOOKUP_INT(lt.servo.load_from_threshold_file);
  for (int i = 0; i < RNO_G_NUM_LT_CHANNELS; i++) 
  {
    LOOKUP_INT_ELEM(lt.servo.initial_trigger_thresholds,i);
  }
  LOOKUP_FLOAT(lt.servo.servo_thresh_frac);
  LOOKUP_FLOAT(lt.servo.servo_thresh_offset);
  LOOKUP_FLOAT(lt.servo.servo_interval);
  LOOKUP_FLOAT(lt.servo.scaler_update_interval);
  LOOKUP_FLOAT(lt.servo.enable);
  LOOKUP_FLOAT(lt.servo.fast_scaler_weight); 
  LOOKUP_FLOAT(lt.servo.slow_scaler_weight); 

  LOOKUP_INT(lt.device.spi_enable_gpio); 

  {
    LOOKUP_STRING(lt.device, spi_device); 
  }

  config_destroy(&config); 
  return 0; 
}



int dump_acq_config(FILE *f, const acq_config_t * cfg) 
{
  /*
  int indent_level = 0; 
  const char * indents[] = { "", "\t", "\t\t", "\t\t\t", "\t\t\t\t", "\t\t\t\t\t"}; 

#define SECT(X) fprintf(f,"%s%s\n%s{\n", indents[indent_level], X, indents[indent_level]); indent_level++; 
#define UNSECT() indent_level--; fprintf("\n%s}\n", indents[indent_level]); 
#define WRITE_INT(X,Y,COMMENT) fprintf(f,"%s//%s\n%s%s=%d;\n", indents[indent_level], COMMENT, indents[indent_level], Y, cfg->X##Y); 
#define WRITE_FLT(X,Y,COMMENT) fprintf(f,"%s//%s\n%s%s=%f;\n", indents[indent_level], COMMENT, indents[indent_level], Y, cfg->X##Y); 
#define WRITE_STR(X,Y,COMMENT) fprintf(f,"%s//%s\n%s%s=%s;\n", indents[indent_level], COMMENT, indents[indent_level], Y, cfg->X##Y); 

 SECT(radiant); 
   SECT(scalers);
   WRITE_INT(radiant.scalers,use_pps,"use pps, otherwise period is used"); 
   WRITE_FLT(radiant.scalers,period,"use pps, otherwise period is used"); 
   
   UNSECT(); 
 UNSECT() ; 
;

*/


}

