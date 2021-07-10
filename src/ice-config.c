#include "ice-config.h" 
#include <libconfig.h> 
#include <stdlib.h> 
#include <stdio.h> 
#include <string.h>

/** This implements configuration of the DAQ.  This is nominally done with
 * libconfig, although this makes heavy use of macros. The output is done "by
 * hand" with macros because that way we can include comments. 
 */


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

  SECT.status_shmem_file = "/rno-g/run/daqstatus.dat" ; 
  SECT.acq_buf_size = 256; 
  SECT.mon_buf_size = 128; 

#undef SECT
#define SECT cfg->lt.gain
  SECT.auto_gain=1; 
  SECT.target_rms=3; 
  for (int i = 0; i < RNO_G_NUM_LT_CHANNELS; i++) 
  {
    SECT.fixed_gain_codes[i] =5;
  }

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
#define SECT cfg->lt.thresholds

  SECT.load_from_threshold_file = 1; 
  for (int i = 0; i < RNO_G_NUM_LT_CHANNELS; i++) 
  {
    SECT.initial[i] = 30; 
  }
#undef SECT
#define SECT cfg->lt.servo
  SECT.enable = 1; 
  for (int i = 0; i < RNO_G_NUM_LT_CHANNELS; i++) 
  {
    SECT.scaler_goals[i] = 30; 
  }
  SECT.servo_thresh_frac = 0.67; 
  SECT.servo_thresh_offset = -10; 
  SECT.fast_scaler_weight = 0.7; 
  SECT.slow_scaler_weight = 0.3; 
  SECT.scaler_update_interval = 0.5; 
  SECT.servo_interval = 1; 
  SECT.subtract_gated = 1; 
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
  SECT.output_enabled =1; 

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

#define LOOKUP_INT_RENAME(X,Y) \
 config_lookup_int(&config, #Y, &cfg->X);  



#define LOOKUP_INT_ELEM(X,i) \
  cfg->X[i] = config_setting_get_int_elem(config_lookup(&config,#X),i);

static int dummy_ival;
//TODO: complain if out of range
#define LOOKUP_UINT8_ELEM(X,i) \
  cfg->X[i] = config_setting_get_int_elem(config_lookup(&config,#X),i);




#define LOOKUP_UINT(X) \
 config_lookup_int(&config, #X, &dummy_ival); \
 cfg->X = (uint32_t) dummy_ival; 

#define LOOKUP_UINT_RENAME(X,Y) \
 config_lookup_int(&config, #Y, &dummy_ival); \
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

#define LOOKUP_FLOAT_RENAME(X,Y) \
  config_lookup_float(&config,#Y, &dummy_val); \
  cfg->X = dummy_val;



#define LOOKUP_FLOAT_ELEM(X,i) \
  cfg->X[i] = config_setting_get_float_elem(config_lookup(&config,#X),i);




int read_acq_config(FILE * f, acq_config_t * cfg) 
{
  config_t config; 
  config_init(&config); 
  config_set_options(&config, CONFIG_OPTION_AUTOCONVERT); 
  if (!config_read(&config, f))
  {
    fprintf(stderr,"Trouble reading config: %s, line: %d\n", config_error_text(&config), config_error_line(&config)); 
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

  //readout 
  LOOKUP_UINT(radiant.readout.readout_mask); 
  LOOKUP_INT(radiant.readout.nbuffers_per_readout);
  LOOKUP_INT(radiant.readout.poll_ms); 

  //trigger
  LOOKUP_INT(radiant.trigger.clear_mode); 
  LOOKUP_INT(radiant.trigger.output_enabled); 
  LOOKUP_INT_RENAME(radiant.trigger.RF[0].enabled, radiant.trigger.RF0.enabled);
  LOOKUP_INT_RENAME(radiant.trigger.RF[1].enabled, radiant.trigger.RF1.enabled)
  LOOKUP_UINT_RENAME(radiant.trigger.RF[0].mask, radiant.trigger.RF0.mask);
  LOOKUP_UINT_RENAME(radiant.trigger.RF[1].mask, radiant.trigger.RF1.mask);
  LOOKUP_FLOAT_RENAME(radiant.trigger.RF[0].window, radiant.trigger.RF0.window);
  LOOKUP_FLOAT_RENAME(radiant.trigger.RF[1].window, radiant.trigger.RF0.window);
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

  LOOKUP_INT(lt.thresholds.load_from_threshold_file);
  for (int i = 0; i < RNO_G_NUM_LT_CHANNELS; i++) 
  {
    LOOKUP_INT_ELEM(lt.thresholds.initial,i);
    LOOKUP_INT_ELEM(lt.servo.scaler_goals,i);
    LOOKUP_INT_ELEM(lt.gain.fixed_gain_codes,i); 
  }
  LOOKUP_INT(lt.servo.subtract_gated);
  LOOKUP_FLOAT(lt.servo.servo_thresh_frac);
  LOOKUP_FLOAT(lt.servo.servo_thresh_offset);
  LOOKUP_FLOAT(lt.servo.servo_interval);
  LOOKUP_FLOAT(lt.servo.scaler_update_interval);
  LOOKUP_FLOAT(lt.servo.enable);
  LOOKUP_FLOAT(lt.servo.fast_scaler_weight); 
  LOOKUP_FLOAT(lt.servo.slow_scaler_weight); 
  LOOKUP_FLOAT(lt.servo.P);
  LOOKUP_FLOAT(lt.servo.I);
  LOOKUP_FLOAT(lt.servo.D);

  LOOKUP_INT(lt.device.spi_enable_gpio); 

  {
    LOOKUP_STRING(lt.device, spi_device); 
  }

  LOOKUP_INT(lt.gain.auto_gain);
  LOOKUP_FLOAT(lt.gain.target_rms); 


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

 SECT(radiant,"RADIANT configuration"); 
   SECT(scalers,"Scalers configuration");
     WRITE_INT(radiant.scalers,use_pps,"use pps, otherwise period is used"); 
     WRITE_FLT(radiant.scalers,period,"The period used for scalers if pps is not enabled"); 
     WRITE_ARR(radiant.scalers,prescal_m1, "The prescaler minus 1 for each channel", RNO_G_NUM_RADIANT_CHANNELS, "%u"); 
   UNSECT(); 
   SECT(thresholds,"Threshold initialization configuration"); 
    WRITE_INT(radiant.thresholds, load_from_threshold_file, "1 to load from threshold file, otherwise initial values will be used"); 
    WRITE_ARR(radiant.thresholds,initial,"Initial thresholds if not loaded from file (in V)", RNO_G_NUM_RADIANT_CHANNELS, "%g"); 
   UNSECT(); 

   SECT(servo, "Threshold servo configuration"); 
    WRITE_INT(radiant.servo, enable, "Enable servoing");
    WRITE_FLT(radiant.servo, scaler_update_interval, "Time interval (in seconds) that scalers are updated at"); 
    WRITE_FLT(radiant.servo, servo_interval, "Time interval (in seconds) that thresholds are updated at"); 
    WRITE_ARR(radiant.servo, nscaler_periods_per_servo_period, 
               "Multiple time periods may be considered in servoing. This sets the length of each time period (" NUM_SERVO_PERIODS_STR " periods must be defined)", NUM_SERVO_PERIODS, "%d" ); 
    WRITE_ARR(radiant.servo, period_weights, 
               "The weights of the aforementioned periods. For scaler goal to mean something sensible, these should add to 1.", NUM_SERVO_PERIODS, "%g" ); 
    WRITE_ARR(radiant.servo, scaler_goals, 
               "The scaler goal for each channel (calculated as the weighted contribution of periods)", RNO_G_NUM_LT_CHANNELS, "%g" ); 
    WRITE_FLT(radiant.servo, max_thresh_change, "The maximum amount the threshold can change by in each step"); 
    WRITE_FLT(radiant.servo,P,"servo PID loop P");
    WRITE_FLT(radiant.servo,I,"servo PID loop I");
    WRITE_FLT(radiant.servo,D,"servo PID loop D");
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
      WRITE_INT(radiant.trigger.pps,enabled,"Enable pps trigger"); 
      WRITE_INT(radiant.trigger.pps,output_enabled,"Enable pps trigger output"); 
    UNSECT(); 
    SECT(RF0,"First RF trigger configuration"); 
      WRITE_INT(radiant.trigger.RF[0],enabled,"Enable this RF trigger"); 
      WRITE_HEX(radiant.trigger.RF[0],mask,"Mask of channels that go into this trigger"); 
      WRITE_FLT(radiant.trigger.RF[0],window,"The time widow (in ns) for the coincidnce  trigger"); 
      WRITE_INT(radiant.trigger.RF[0],num_coincidences,"Number of coincidences (min 1) in this coincidence trigger"); 
    UNSECT()
    SECT(RF1,"Second RF trigger configuration"); 
      WRITE_INT(radiant.trigger.RF[1],enabled,"Enable this RF trigger"); 
      WRITE_HEX(radiant.trigger.RF[1],mask,"Mask of channels that go into this trigger"); 
      WRITE_FLT(radiant.trigger.RF[1],window,"The time widow (in ns) for the coincidnce  trigger"); 
      WRITE_INT(radiant.trigger.RF[1],num_coincidences,"Number of coincidences (min 1) in this coincidence trigger"); 
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
  UNSECT(); 
  SECT(analog,"Analog settings for the RADIANT"); 
    WRITE_INT(radiant.analog,apply_lab4_vbias, "Apply lab4 vbias at beginning of run (instead of using whatever it is)"); 
    WRITE_ARR(radiant.analog,lab4_vbias,"The lab3 vbias (in V) to apply", 2, "%g"); 
    WRITE_INT(radiant.analog,apply_diode_vbias, "Apply diode vbias at beginning of run (instead of using whatever it is)"); 
    WRITE_ARR(radiant.analog,diode_vbias,"The diode vbias (in V) to apply", RNO_G_NUM_RADIANT_CHANNELS, "%g"); 
    WRITE_INT(radiant.analog,apply_attenuations,"Apply attenuations to digitizer/trigger paths"); 
    WRITE_ARR(radiant.analog,digi_attenuation,"Digitizer path attenuations (dB)", RNO_G_NUM_RADIANT_CHANNELS, "%g"); 
    WRITE_ARR(radiant.analog,trig_attenuation,"Trigger path attenuations (dB)", RNO_G_NUM_RADIANT_CHANNELS, "%g"); 
    WRITE_FLT(radiant.analog,settle_time,"Time to wait after setting analog params"); 
  UNSECT(); 
  SECT(device,"RADIANT other device settings"); 
    WRITE_STR(radiant.device,reset_script, "Script to reset the radiant (not implemented yet)"); 
    WRITE_STR(radiant.device,spi_device,"SPI device for RADIANT DMA"); 
    WRITE_STR(radiant.device,uart_device,"UART device for RADIANT comms"); 
    WRITE_INT(radiant.device,poll_gpio,"gpio to poll on"); 
    WRITE_INT(radiant.device,spi_enable_gpio,"gpio to enable gpio (negative for active low)"); 
  UNSECT(); 
  SECT(pps,"RADIANT pps settings"); 
    WRITE_INT(radiant.pps,use_internal,"Use internal PPS instead of from GPS"); 
    WRITE_INT(radiant.pps,sync_out," Enable sync out"); 
    WRITE_INT(radiant.pps,pps_holdoff,"Amount of PPS holdoff (in some units...)"); 
  UNSECT(); 
 UNSECT() ; 

  SECT(lt,"Settings for the low-threshold (FLOWER) board"); 
    SECT(trigger,"Trigger settings for the low-threshold-board"); 
       WRITE_INT(lt.trigger,enable, "Enable the LT trigger"); 
       WRITE_INT(lt.trigger,vpp, " Vpp threshold  (max 255)"); 
       WRITE_INT(lt.trigger,min_coincidence,"Minimum coincidence threshold for channels (minmum 1)"); 
       WRITE_INT(lt.trigger,window,"Coincidence window"); 
    UNSECT(); 

    SECT(thresholds,"Threshold settings for the low-threshold board"); 
       WRITE_INT(lt.thresholds,load_from_threshold_file,"Load thresholds from threshold file (if available)"); 
       WRITE_ARR(lt.thresholds,initial,"Initial thresholds if not loaded from file (in adc)",RNO_G_NUM_LT_CHANNELS,"%u"); 
    UNSECT(); 
    SECT(servo, "Servo setings for the low-threshold board"); 
       WRITE_INT(lt.servo,enable,"Enable servoing"); 
       WRITE_INT(lt.servo,subtract_gated,"Subtract gated scalers"); 
       WRITE_ARR(lt.servo,scaler_goals,"",RNO_G_NUM_LT_CHANNELS,"%u"); 
       WRITE_FLT(lt.servo,servo_thresh_frac,"");
       WRITE_FLT(lt.servo,servo_thresh_offset,"");
       WRITE_FLT(lt.servo,fast_scaler_weight,"");
       WRITE_FLT(lt.servo,slow_scaler_weight,"");
       WRITE_FLT(lt.servo,scaler_update_interval,"");
       WRITE_FLT(lt.servo,servo_interval,"");
       WRITE_FLT(lt.servo,P,"");
       WRITE_FLT(lt.servo,I,"");
       WRITE_FLT(lt.servo,D,"");
    UNSECT(); 
    SECT(gain,""); 
      WRITE_INT(lt.gain,auto_gain,""); 
      WRITE_FLT(lt.gain,target_rms,""); 
      WRITE_ARR(lt.gain,fixed_gain_codes,"", RNO_G_NUM_LT_CHANNELS, "%u"); 
    UNSECT(); 
    SECT(device,""); 
      WRITE_STR(lt.device,spi_device,"");
      WRITE_INT(lt.device,spi_enable_gpio,"");
    UNSECT(); 
  UNSECT()

  SECT(runtime,"Runtime settings"); 
    WRITE_STR(runtime,status_shmem_file,"");
    WRITE_INT(runtime,acq_buf_size,"");
    WRITE_INT(runtime,mon_buf_size,"");
  UNSECT();
   

  SECT(output,"Output settings"); 
    WRITE_STR(output,base_dir,"");
    WRITE_STR(output,runfile,"");
    WRITE_FLT(output,daqstatus_interval,"");
    WRITE_INT(output,seconds_per_run,"");
    WRITE_INT(output,max_events_per_file,"");
    WRITE_INT(output,max_daqstatuses_per_file,"");
    WRITE_INT(output,max_seconds_per_file,"");
    WRITE_INT(output,max_kB_per_file,"");
    WRITE_INT(output,min_free_space_MB,"");
    WRITE_INT(output,print_interval,"");
  UNSECT(); 


 return 0;

}

