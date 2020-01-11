//! @file

#include "Marlin.h"

#include "fsensor.h"
#include <avr/pgmspace.h>
#include "pat9125.h"
#include "stepper.h"
#include "io_atmega2560.h"
#include "cmdqueue.h"
#include "ultralcd.h"
#include "mmu.h"
#include "cardreader.h"

#include "adc.h"
#include "temperature.h"
#include "config.h"

//! @name Basic parameters
//! @{
#define FSENSOR_CHUNK_LEN    0.64F  //!< filament sensor chunk length 0.64mm
#define FSENSOR_ERR_MAX         17  //!< filament sensor maximum error count for runout detection
//! @}

//! @name Optical quality measurement parameters
//! @{
#define FSENSOR_OQ_MAX_ES      6    //!< maximum error sum while loading (length ~64mm = 100chunks)
#define FSENSOR_OQ_MAX_EM      2    //!< maximum error counter value while loading
#define FSENSOR_OQ_MIN_YD      2    //!< minimum yd per chunk (applied to avg value)
#define FSENSOR_OQ_MAX_YD      200  //!< maximum yd per chunk (applied to avg value)
#define FSENSOR_OQ_MAX_PD      4    //!< maximum positive deviation (= yd_max/yd_avg)
#define FSENSOR_OQ_MAX_ND      5    //!< maximum negative deviation (= yd_avg/yd_min)
#define FSENSOR_OQ_MAX_SH      13   //!< maximum shutter value
//! @}

const char ERRMSG_PAT9125_NOT_RESP[] PROGMEM = "PAT9125 not responding (%d)!\n";

// PJ7 can not be used (does not have PinChangeInterrupt possibility)
#define FSENSOR_INT_PIN          75 //!< filament sensor interrupt pin PJ4
#define FSENSOR_INT_PIN_MASK   0x10 //!< filament sensor interrupt pin mask (bit4)
#define FSENSOR_INT_PIN_PIN_REG PINJ              // PIN register @ PJ4
#define FSENSOR_INT_PIN_VECT PCINT1_vect          // PinChange ISR @ PJ4
#define FSENSOR_INT_PIN_PCMSK_REG PCMSK1          // PinChangeMaskRegister @ PJ4
#define FSENSOR_INT_PIN_PCMSK_BIT PCINT13         // PinChange Interrupt / PinChange Enable Mask @ PJ4
#define FSENSOR_INT_PIN_PCICR_BIT PCIE1           // PinChange Interrupt Enable / Flag @ PJ4

//uint8_t fsensor_int_pin = FSENSOR_INT_PIN;
uint8_t fsensor_int_pin_old = 0;
int16_t fsensor_chunk_len = 0;

//! enabled = initialized and sampled every chunk event
bool fsensor_enabled = true;
//! runout watching is done in fsensor_update (called from main loop)
bool fsensor_watch_runout = true;
//! not responding - is set if any communication error occurred during initialization or readout
bool fsensor_not_responding = false;
//! enable/disable quality meassurement
bool fsensor_oq_meassure_enabled = false;

//! number of errors, updated in ISR
uint8_t fsensor_err_cnt = 0;
//! variable for accumulating step count (updated callbacks from stepper and ISR)
int16_t fsensor_st_cnt = 0;
//! last dy value from pat9125 sensor (used in ISR)
int16_t fsensor_dy_old = 0;

//! log flag: 0=log disabled, 1=log enabled
uint8_t fsensor_log = 1;


//! @name filament autoload variables
//! @{

//! autoload feature enabled
bool fsensor_autoload_enabled = true;
//! autoload watching enable/disable flag
bool fsensor_watch_autoload = false;
//
uint16_t fsensor_autoload_y;
//
uint8_t fsensor_autoload_c;
//
uint32_t fsensor_autoload_last_millis;
//
uint8_t fsensor_autoload_sum;
//! @}


//! @name filament optical quality measurement variables
//! @{

//! Measurement enable/disable flag
bool fsensor_oq_meassure = false;
//! skip-chunk counter, for accurate measurement is necessary to skip first chunk...
uint8_t  fsensor_oq_skipchunk;
//! number of samples from start of measurement
uint8_t fsensor_oq_samples;
//! sum of steps in positive direction movements
uint16_t fsensor_oq_st_sum;
//! sum of deltas in positive direction movements
uint16_t fsensor_oq_yd_sum;
//! sum of errors during measurement
uint16_t fsensor_oq_er_sum;
//! max error counter value during measurement
uint8_t  fsensor_oq_er_max;
//! minimum delta value
int16_t fsensor_oq_yd_min;
//! maximum delta value
int16_t fsensor_oq_yd_max;
//! sum of shutter value
uint16_t fsensor_oq_sh_sum;
//! @}

#if IR_SENSOR_ANALOG
ClFsensorPCB oFsensorPCB;
ClFsensorActionNA oFsensorActionNA;
bool bIRsensorStateFlag=false;
unsigned long nIRsensorLastTime;
#endif //IR_SENSOR_ANALOG

void fsensor_stop_and_save_print(void)
{
    printf_P(PSTR("fsensor_stop_and_save_print\n"));
    stop_and_save_print_to_ram(0, 0); //XYZE - no change
}

void fsensor_restore_print_and_continue(void)
{
    printf_P(PSTR("fsensor_restore_print_and_continue\n"));
	fsensor_err_cnt = 0;
    restore_print_from_ram_and_continue(0); //XYZ = orig, E - no change
}

// fsensor_checkpoint_print cuts the current print job at the current position,
// allowing new instructions to be inserted in the middle
void fsensor_checkpoint_print(void)
{
    printf_P(PSTR("fsensor_checkpoint_print\n"));
    stop_and_save_print_to_ram(0, 0);
    restore_print_from_ram_and_continue(0);
}

void fsensor_init(void)
{
#ifdef PAT9125
	uint8_t pat9125 = pat9125_init();
     printf_P(PSTR("PAT9125_init:%hhu\n"), pat9125);
#endif //PAT9125
	uint8_t fsensor = eeprom_read_byte((uint8_t*)EEPROM_FSENSOR);
	fsensor_autoload_enabled=eeprom_read_byte((uint8_t*)EEPROM_FSENS_AUTOLOAD_ENABLED);
     fsensor_not_responding = false;
#ifdef PAT9125
	uint8_t oq_meassure_enabled = eeprom_read_byte((uint8_t*)EEPROM_FSENS_OQ_MEASS_ENABLED);
	fsensor_oq_meassure_enabled = (oq_meassure_enabled == 1)?true:false;
	fsensor_chunk_len = (int16_t)(FSENSOR_CHUNK_LEN * cs.axis_steps_per_unit[E_AXIS]);

	if (!pat9125)
	{
		fsensor = 0; //disable sensor
		fsensor_not_responding = true;
	}
#endif //PAT9125
#if IR_SENSOR_ANALOG
     bIRsensorStateFlag=false;
     oFsensorPCB=(ClFsensorPCB)eeprom_read_byte((uint8_t*)EEPROM_FSENSOR_PCB);
     oFsensorActionNA=(ClFsensorActionNA)eeprom_read_byte((uint8_t*)EEPROM_FSENSOR_ACTION_NA);
#endif //IR_SENSOR_ANALOG
	if (fsensor)
		fsensor_enable(false);                  // (in this case) EEPROM update is not necessary
	else
		fsensor_disable(false);                 // (in this case) EEPROM update is not necessary
	printf_P(PSTR("FSensor %S"), (fsensor_enabled?PSTR("ENABLED"):PSTR("DISABLED")));
#if IR_SENSOR_ANALOG
     printf_P(PSTR(" (sensor board revision: %S)\n"),(oFsensorPCB==ClFsensorPCB::_Rev03b)?PSTR("03b or newer"):PSTR("03 or older"));
#else //IR_SENSOR_ANALOG
     printf_P(PSTR("\n"));
#endif //IR_SENSOR_ANALOG
	if (check_for_ir_sensor()) ir_sensor_detected = true;

}

bool fsensor_enable(bool bUpdateEEPROM)
{
#ifdef PAT9125
	if (mmu_enabled == false) { //filament sensor is pat9125, enable only if it is working
		uint8_t pat9125 = pat9125_init();
		printf_P(PSTR("PAT9125_init:%hhu\n"), pat9125);
		if (pat9125)
			fsensor_not_responding = false;
		else
			fsensor_not_responding = true;
		fsensor_enabled = pat9125 ? true : false;
		fsensor_watch_runout = true;
		fsensor_oq_meassure = false;
		fsensor_err_cnt = 0;
		fsensor_dy_old = 0;
		eeprom_update_byte((uint8_t*)EEPROM_FSENSOR, fsensor_enabled ? 0x01 : 0x00);
		FSensorStateMenu = fsensor_enabled ? 1 : 0;
	}
	else //filament sensor is FINDA, always enable 
	{
		fsensor_enabled = true;
		eeprom_update_byte((uint8_t*)EEPROM_FSENSOR, 0x01);
		FSensorStateMenu = 1;
	}
#else // PAT9125
#if IR_SENSOR_ANALOG
     if(!fsensor_IR_check())
          {
          bUpdateEEPROM=true;
          fsensor_enabled=false;
          fsensor_not_responding=true;
          FSensorStateMenu=0;
          }
     else {
#endif //IR_SENSOR_ANALOG
     fsensor_enabled=true;
     fsensor_not_responding=false;
     FSensorStateMenu=1;
#if IR_SENSOR_ANALOG
          }
#endif //IR_SENSOR_ANALOG
     if(bUpdateEEPROM)
          eeprom_update_byte((uint8_t*)EEPROM_FSENSOR, FSensorStateMenu);
#endif //PAT9125
	return fsensor_enabled;
}

void fsensor_disable(bool bUpdateEEPROM)
{ 
	fsensor_enabled = false;
	FSensorStateMenu = 0;
     if(bUpdateEEPROM)
          eeprom_update_byte((uint8_t*)EEPROM_FSENSOR, 0x00); 
}

void fsensor_autoload_set(bool State)
{
#ifdef PAT9125
	if (!State) fsensor_autoload_check_stop();
#endif //PAT9125
	fsensor_autoload_enabled = State;
	eeprom_update_byte((unsigned char *)EEPROM_FSENS_AUTOLOAD_ENABLED, fsensor_autoload_enabled);
}

void pciSetup(byte pin)
{
// !!! "digitalPinTo?????bit()" does not provide the correct results for some MCU pins
	*digitalPinToPCMSK(pin) |= bit (digitalPinToPCMSKbit(pin)); // enable pin
	PCIFR |= bit (digitalPinToPCICRbit(pin)); // clear any outstanding interrupt
	PCICR |= bit (digitalPinToPCICRbit(pin)); // enable interrupt for the group 
}

#ifdef PAT9125
void fsensor_autoload_check_start(void)
{
//	puts_P(_N("fsensor_autoload_check_start\n"));
	if (!fsensor_enabled) return;
	if (!fsensor_autoload_enabled) return;
	if (fsensor_watch_autoload) return;
	if (!pat9125_update()) //update sensor
	{
		fsensor_disable();
		fsensor_not_responding = true;
		fsensor_watch_autoload = false;
		printf_P(ERRMSG_PAT9125_NOT_RESP, 3);
		return;
	}
	puts_P(_N("fsensor_autoload_check_start - autoload ENABLED\n"));
	fsensor_autoload_y = pat9125_y; //save current y value
	fsensor_autoload_c = 0; //reset number of changes counter
	fsensor_autoload_sum = 0;
	fsensor_autoload_last_millis = _millis();
	fsensor_watch_runout = false;
	fsensor_watch_autoload = true;
	fsensor_err_cnt = 0;
}

void fsensor_autoload_check_stop(void)
{

//	puts_P(_N("fsensor_autoload_check_stop\n"));
	if (!fsensor_enabled) return;
//	puts_P(_N("fsensor_autoload_check_stop 1\n"));
	if (!fsensor_autoload_enabled) return;
//	puts_P(_N("fsensor_autoload_check_stop 2\n"));
	if (!fsensor_watch_autoload) return;
	puts_P(_N("fsensor_autoload_check_stop - autoload DISABLED\n"));
	fsensor_autoload_sum = 0;
	fsensor_watch_autoload = false;
	fsensor_watch_runout = true;
	fsensor_err_cnt = 0;
}
#endif //PAT9125

bool fsensor_check_autoload(void)
{
	if (!fsensor_enabled) return false;
	if (!fsensor_autoload_enabled) return false;
	if (ir_sensor_detected) {
		if (digitalRead(IR_SENSOR_PIN) == 1) {
			fsensor_watch_autoload = true;
		}
		else if (fsensor_watch_autoload == true) {
			fsensor_watch_autoload = false;
			return true;
		}
	}
#ifdef PAT9125
	if (!fsensor_watch_autoload)
	{
		fsensor_autoload_check_start();
		return false;
	}
#if 0
	uint8_t fsensor_autoload_c_old = fsensor_autoload_c;
#endif
	if ((_millis() - fsensor_autoload_last_millis) < 25) return false;
	fsensor_autoload_last_millis = _millis();
	if (!pat9125_update_y()) //update sensor
	{
		fsensor_disable();
		fsensor_not_responding = true;
		printf_P(ERRMSG_PAT9125_NOT_RESP, 2);
		return false;
	}
	int16_t dy = pat9125_y - fsensor_autoload_y;
	if (dy) //? dy value is nonzero
	{
		if (dy > 0) //? delta-y value is positive (inserting)
		{
			fsensor_autoload_sum += dy;
			fsensor_autoload_c += 3; //increment change counter by 3
		}
		else if (fsensor_autoload_c > 1)
			fsensor_autoload_c -= 2; //decrement change counter by 2 
		fsensor_autoload_y = pat9125_y; //save current value
	}
	else if (fsensor_autoload_c > 0)
		fsensor_autoload_c--;
	if (fsensor_autoload_c == 0) fsensor_autoload_sum = 0;
#if 0
  	puts_P(_N("fsensor_check_autoload\n"));
  	if (fsensor_autoload_c != fsensor_autoload_c_old)
  		printf_P(PSTR("fsensor_check_autoload dy=%d c=%d sum=%d\n"), dy, fsensor_autoload_c, fsensor_autoload_sum);
#endif
//	if ((fsensor_autoload_c >= 15) && (fsensor_autoload_sum > 30))
	if ((fsensor_autoload_c >= 12) && (fsensor_autoload_sum > 20))
	{
//		puts_P(_N("fsensor_check_autoload = true !!!\n"));
		return true;
	}
#endif //PAT9125
	return false;
}

void fsensor_oq_meassure_set(bool State)
{
	fsensor_oq_meassure_enabled = State;
	eeprom_update_byte((unsigned char *)EEPROM_FSENS_OQ_MEASS_ENABLED, fsensor_oq_meassure_enabled);
}

void fsensor_oq_meassure_start(uint8_t skip)
{
	if (!fsensor_enabled) return;
	if (!fsensor_oq_meassure_enabled) return;
	printf_P(PSTR("fsensor_oq_meassure_start\n"));
	fsensor_oq_skipchunk = skip;
	fsensor_oq_samples = 0;
	fsensor_oq_st_sum = 0;
	fsensor_oq_yd_sum = 0;
	fsensor_oq_er_sum = 0;
	fsensor_oq_er_max = 0;
	fsensor_oq_yd_min = FSENSOR_OQ_MAX_YD;
	fsensor_oq_yd_max = 0;
	fsensor_oq_sh_sum = 0;
	pat9125_update();
	pat9125_y = 0;
	fsensor_watch_runout = false;
	fsensor_oq_meassure = true;
}

void fsensor_oq_meassure_stop(void)
{
	if (!fsensor_enabled) return;
	if (!fsensor_oq_meassure_enabled) return;
	printf_P(PSTR("fsensor_oq_meassure_stop, %hhu samples\n"), fsensor_oq_samples);
	printf_P(_N(" st_sum=%u yd_sum=%u er_sum=%u er_max=%hhu\n"), fsensor_oq_st_sum, fsensor_oq_yd_sum, fsensor_oq_er_sum, fsensor_oq_er_max);
	printf_P(_N(" yd_min=%u yd_max=%u yd_avg=%u sh_avg=%u\n"), fsensor_oq_yd_min, fsensor_oq_yd_max, (uint16_t)((uint32_t)fsensor_oq_yd_sum * fsensor_chunk_len / fsensor_oq_st_sum), (uint16_t)(fsensor_oq_sh_sum / fsensor_oq_samples));
	fsensor_oq_meassure = false;
	fsensor_watch_runout = true;
	fsensor_err_cnt = 0;
}

const char _OK[] PROGMEM = "OK";
const char _NG[] PROGMEM = "NG!";

bool fsensor_oq_result(void)
{
	if (!fsensor_enabled) return true;
	if (!fsensor_oq_meassure_enabled) return true;
	printf_P(_N("fsensor_oq_result\n"));
	bool res_er_sum = (fsensor_oq_er_sum <= FSENSOR_OQ_MAX_ES);
	printf_P(_N(" er_sum = %u %S\n"), fsensor_oq_er_sum, (res_er_sum?_OK:_NG));
	bool res_er_max = (fsensor_oq_er_max <= FSENSOR_OQ_MAX_EM);
	printf_P(_N(" er_max = %hhu %S\n"), fsensor_oq_er_max, (res_er_max?_OK:_NG));
	uint8_t yd_avg = ((uint32_t)fsensor_oq_yd_sum * fsensor_chunk_len / fsensor_oq_st_sum);
	bool res_yd_avg = (yd_avg >= FSENSOR_OQ_MIN_YD) && (yd_avg <= FSENSOR_OQ_MAX_YD);
	printf_P(_N(" yd_avg = %hhu %S\n"), yd_avg, (res_yd_avg?_OK:_NG));
	bool res_yd_max = (fsensor_oq_yd_max <= (yd_avg * FSENSOR_OQ_MAX_PD));
	printf_P(_N(" yd_max = %u %S\n"), fsensor_oq_yd_max, (res_yd_max?_OK:_NG));
	bool res_yd_min = (fsensor_oq_yd_min >= (yd_avg / FSENSOR_OQ_MAX_ND));
	printf_P(_N(" yd_min = %u %S\n"), fsensor_oq_yd_min, (res_yd_min?_OK:_NG));

	uint16_t yd_dev = (fsensor_oq_yd_max - yd_avg) + (yd_avg - fsensor_oq_yd_min);
	printf_P(_N(" yd_dev = %u\n"), yd_dev);

	uint16_t yd_qua = 10 * yd_avg / (yd_dev + 1);
	printf_P(_N(" yd_qua = %u %S\n"), yd_qua, ((yd_qua >= 8)?_OK:_NG));

	uint8_t sh_avg = (fsensor_oq_sh_sum / fsensor_oq_samples);
	bool res_sh_avg = (sh_avg <= FSENSOR_OQ_MAX_SH);
	if (yd_qua >= 8) res_sh_avg = true;

	printf_P(_N(" sh_avg = %hhu %S\n"), sh_avg, (res_sh_avg?_OK:_NG));
	bool res = res_er_sum && res_er_max && res_yd_avg && res_yd_max && res_yd_min && res_sh_avg;
	printf_P(_N("fsensor_oq_result %S\n"), (res?_OK:_NG));
	return res;
}
#ifdef PAT9125
ISR(FSENSOR_INT_PIN_VECT)
{
	if (mmu_enabled || ir_sensor_detected) return;
	if (!((fsensor_int_pin_old ^ FSENSOR_INT_PIN_PIN_REG) & FSENSOR_INT_PIN_MASK)) return;
	fsensor_int_pin_old = FSENSOR_INT_PIN_PIN_REG;
	static bool _lock = false;
	if (_lock) return;
	_lock = true;
	int st_cnt = fsensor_st_cnt;
	fsensor_st_cnt = 0;
	sei();
	uint8_t old_err_cnt = fsensor_err_cnt;
	uint8_t pat9125_res = fsensor_oq_meassure?pat9125_update():pat9125_update_y();
	if (!pat9125_res)
	{
		fsensor_disable();
		fsensor_not_responding = true;
		printf_P(ERRMSG_PAT9125_NOT_RESP, 1);
	}
	if (st_cnt != 0)
	{ //movement
		if (st_cnt > 0) //positive movement
		{
			if (pat9125_y < 0)
			{
				if (fsensor_err_cnt)
					fsensor_err_cnt += 2;
				else
					fsensor_err_cnt++;
			}
			else if (pat9125_y > 0)
			{
				if (fsensor_err_cnt)
					fsensor_err_cnt--;
			}
			else //(pat9125_y == 0)
				if (((fsensor_dy_old <= 0) || (fsensor_err_cnt)) && (st_cnt > (fsensor_chunk_len >> 1)))
					fsensor_err_cnt++;
			if (fsensor_oq_meassure)
			{
				if (fsensor_oq_skipchunk)
				{
					fsensor_oq_skipchunk--;
					fsensor_err_cnt = 0;
				}
				else
				{
					if (st_cnt == fsensor_chunk_len)
					{
						if (pat9125_y > 0) if (fsensor_oq_yd_min > pat9125_y) fsensor_oq_yd_min = (fsensor_oq_yd_min + pat9125_y) / 2;
						if (pat9125_y >= 0) if (fsensor_oq_yd_max < pat9125_y) fsensor_oq_yd_max = (fsensor_oq_yd_max + pat9125_y) / 2;
					}
					fsensor_oq_samples++;
					fsensor_oq_st_sum += st_cnt;
					if (pat9125_y > 0) fsensor_oq_yd_sum += pat9125_y;
					if (fsensor_err_cnt > old_err_cnt)
						fsensor_oq_er_sum += (fsensor_err_cnt - old_err_cnt);
					if (fsensor_oq_er_max < fsensor_err_cnt)
						fsensor_oq_er_max = fsensor_err_cnt;
					fsensor_oq_sh_sum += pat9125_s;
				}
			}
		}
		else //negative movement
		{
		}
	}
	else
	{ //no movement
	}

#ifdef DEBUG_FSENSOR_LOG
	if (fsensor_log)
	{
		printf_P(_N("FSENSOR cnt=%d dy=%d err=%hhu %S\n"), st_cnt, pat9125_y, fsensor_err_cnt, (fsensor_err_cnt > old_err_cnt)?_N("NG!"):_N("OK"));
		if (fsensor_oq_meassure) printf_P(_N("FSENSOR st_sum=%u yd_sum=%u er_sum=%u er_max=%hhu yd_max=%u\n"), fsensor_oq_st_sum, fsensor_oq_yd_sum, fsensor_oq_er_sum, fsensor_oq_er_max, fsensor_oq_yd_max);
	}
#endif //DEBUG_FSENSOR_LOG

	fsensor_dy_old = pat9125_y;
	pat9125_y = 0;

	_lock = false;
	return;
}

void fsensor_setup_interrupt(void)
{

	pinMode(FSENSOR_INT_PIN, OUTPUT);
	digitalWrite(FSENSOR_INT_PIN, LOW);
	fsensor_int_pin_old = 0;

	//pciSetup(FSENSOR_INT_PIN);
// !!! "pciSetup()" does not provide the correct results for some MCU pins
// so interrupt registers settings:
     FSENSOR_INT_PIN_PCMSK_REG |= bit(FSENSOR_INT_PIN_PCMSK_BIT); // enable corresponding PinChangeInterrupt (individual pin)
     PCIFR |= bit(FSENSOR_INT_PIN_PCICR_BIT);     // clear previous occasional interrupt (set of pins)
     PCICR |= bit(FSENSOR_INT_PIN_PCICR_BIT);     // enable corresponding PinChangeInterrupt (set of pins)
}

#endif //PAT9125

void fsensor_st_block_chunk(int cnt)
{
	if (!fsensor_enabled) return;
	fsensor_st_cnt += cnt;
	if (abs(fsensor_st_cnt) >= fsensor_chunk_len)
	{
// !!! bit toggling (PINxn <- 1) (for PinChangeInterrupt) does not work for some MCU pins
		if (PIN_GET(FSENSOR_INT_PIN)) {PIN_VAL(FSENSOR_INT_PIN, LOW);}
		else {PIN_VAL(FSENSOR_INT_PIN, HIGH);}
	}
}


//! Common code for enqueing M600 and supplemental codes into the command queue.
//! Used both for the IR sensor and the PAT9125
void fsensor_enque_M600(){
	printf_P(PSTR("fsensor_update - M600\n"));
	eeprom_update_byte((uint8_t*)EEPROM_FERROR_COUNT, eeprom_read_byte((uint8_t*)EEPROM_FERROR_COUNT) + 1);
	eeprom_update_word((uint16_t*)EEPROM_FERROR_COUNT_TOT, eeprom_read_word((uint16_t*)EEPROM_FERROR_COUNT_TOT) + 1);
	enquecommand_front_P((PSTR("M600")));
}

//! @brief filament sensor update (perform M600 on filament runout)
//!
//! Works only if filament sensor is enabled.
//! When the filament sensor error count is larger then FSENSOR_ERR_MAX, pauses print, tries to move filament back and forth.
//! If there is still no plausible signal from filament sensor plans M600 (Filament change).
void fsensor_update(void)
{
#ifdef PAT9125
		if (fsensor_enabled && fsensor_watch_runout && (fsensor_err_cnt > FSENSOR_ERR_MAX))
		{
			bool autoload_enabled_tmp = fsensor_autoload_enabled;
			fsensor_autoload_enabled = false;
			bool oq_meassure_enabled_tmp = fsensor_oq_meassure_enabled;
			fsensor_oq_meassure_enabled = true;

			fsensor_stop_and_save_print();

			fsensor_err_cnt = 0;
			fsensor_oq_meassure_start(0);

			enquecommand_front_P((PSTR("G1 E-3 F200")));
			process_commands();
			KEEPALIVE_STATE(IN_HANDLER);
			cmdqueue_pop_front();
			st_synchronize();

			enquecommand_front_P((PSTR("G1 E3 F200")));
			process_commands();
			KEEPALIVE_STATE(IN_HANDLER);
			cmdqueue_pop_front();
			st_synchronize();

			uint8_t err_cnt = fsensor_err_cnt;
			fsensor_oq_meassure_stop();

			bool err = false;
			err |= (err_cnt > 1);

			err |= (fsensor_oq_er_sum > 2);
			err |= (fsensor_oq_yd_sum < (4 * FSENSOR_OQ_MIN_YD));

            fsensor_restore_print_and_continue();
			fsensor_autoload_enabled = autoload_enabled_tmp;
			fsensor_oq_meassure_enabled = oq_meassure_enabled_tmp;

			if (!err)
				printf_P(PSTR("fsensor_err_cnt = 0\n"));
			else
				fsensor_enque_M600();
		}
#else //PAT9125
		if (CHECK_FSENSOR && fsensor_enabled && ir_sensor_detected)
        {
               if(digitalRead(IR_SENSOR_PIN))
               {                                  // IR_SENSOR_PIN ~ H
#if IR_SENSOR_ANALOG
                    if(!bIRsensorStateFlag)
                    {
                         bIRsensorStateFlag=true;
                         nIRsensorLastTime=_millis();
                    }
                    else
                    {
                         if((_millis()-nIRsensorLastTime)>IR_SENSOR_STEADY)
                         {
                              uint8_t nMUX1,nMUX2;
                              uint16_t nADC;
                              bIRsensorStateFlag=false;
                              // sequence for direct data reading from AD converter
                              DISABLE_TEMPERATURE_INTERRUPT();
                              nMUX1=ADMUX;        // ADMUX saving
                              nMUX2=ADCSRB;
                              adc_setmux(VOLT_IR_PIN);
                              ADCSRA|=(1<<ADSC);  // first conversion after ADMUX change discarded (preventively)
                              while(ADCSRA&(1<<ADSC))
                                   ;
                              ADCSRA|=(1<<ADSC);  // second conversion used
                              while(ADCSRA&(1<<ADSC))
                                   ;
                              nADC=ADC;
                              ADMUX=nMUX1;        // ADMUX restoring
                              ADCSRB=nMUX2;
                              ENABLE_TEMPERATURE_INTERRUPT();
                              // end of sequence for ...
                              if((oFsensorPCB==ClFsensorPCB::_Rev03b)&&((nADC*OVERSAMPLENR)>((int)IRsensor_Hopen_TRESHOLD)))
                              {
                                   fsensor_disable();
                                   fsensor_not_responding = true;
                                   printf_P(PSTR("IR sensor not responding (%d)!\n"),1);
                                   if((ClFsensorActionNA)eeprom_read_byte((uint8_t*)EEPROM_FSENSOR_ACTION_NA)==ClFsensorActionNA::_Pause)
                                   if(oFsensorActionNA==ClFsensorActionNA::_Pause)
                                        lcd_pause_print();
                              }
                              else
                              {
#endif //IR_SENSOR_ANALOG
                                  fsensor_checkpoint_print();
                                  fsensor_enque_M600();
#if IR_SENSOR_ANALOG
                              }
                         }
                    }
               }
               else
               {                                  // IR_SENSOR_PIN ~ L
                    bIRsensorStateFlag=false;
#endif //IR_SENSOR_ANALOG
               }
		}
#endif //PAT9125
}

#if IR_SENSOR_ANALOG
bool fsensor_IR_check()
{
uint16_t volt_IR_int;
bool bCheckResult;

volt_IR_int=current_voltage_raw_IR;
bCheckResult=(volt_IR_int<((int)IRsensor_Lmax_TRESHOLD))||(volt_IR_int>((int)IRsensor_Hmin_TRESHOLD));
bCheckResult=bCheckResult&&(!((oFsensorPCB==ClFsensorPCB::_Rev03b)&&(volt_IR_int>((int)IRsensor_Hopen_TRESHOLD))));
return(bCheckResult);
}
#endif //IR_SENSOR_ANALOG
