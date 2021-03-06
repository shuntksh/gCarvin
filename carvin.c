/*
  carvin.c - Handles Carvin Controller specific items

  Copyright (c) 2014,2015 Bart Dring / Inventables
*/

#include "system.h"
#include "settings.h"
#include "carvin.h"

// this is used for the hardware ID function
// These pins are hardwired to either Gnd or 5V 
// Each new rev of hardware gets a new ID
#define HRDW_ID_DDR	     DDRC
#define HRDW_ID_PORT     PORTC
#define HRDW_ID_PIN      PINC
#define HRDW_ID_0	     3 		
#define HRDW_ID_1        4
#define HRDW_ID_2        5
#define HRDW_ID_3        6
#define HRDW_ID_4        7
#define HRDW_ID_MASK     (1<<HRDW_ID_0 | 1<<HRDW_ID_1 | 1<<HRDW_ID_2 | 1<<HRDW_ID_3 | 1<<HRDW_ID_4) 

int control_button_counter = 0;  // initialize this for use in button debouncing

// setup routine for a Carvin Controller
void carvin_init()
{
  use_sleep_feature = true;

  // setup the hardware I.D. 
  HRDW_ID_DDR &= ~(HRDW_ID_MASK);  // make pins inputs
  HRDW_ID_PORT |= HRDW_ID_MASK;  // Turn on the internal pullups
  // setup the PWM pins as outputs
  BUTTON_LED_DDR |= (1<<BUTTON_LED_BIT);
  DOOR_LED_DDR |= (1<<DOOR_LED_BIT);
  SPINDLE_LED_DDR |= (1<<SPINDLE_LED_BIT);

  hardware_rev = get_hardware_rev();
  
  spindle_current_init( hardware_rev >= 1U );
  
  tmc26x_init();  // SPI functions to program the chips

  // -------------- Setup PWM on Timer 4 ------------------------------

  //  Setup PWM For LEDs
  TCCR4A = (1<<COM4A1) | (1<<COM4B1) | (1<<COM4C1) |(1<<WGM41) | (1<<WGM40);
  TCCR4B = (TCCR4B & 0b11111000) | 0x02; // set to 1/8 Prescaler
  //  Set initial duty cycles
  BUTTON_LED_OCR = 0;
  SPINDLE_LED_OCR = 0;

  // -------------- Setup PWM on Timer 3 ------------------------------

  //  Setup PWM For Door LED
  //  --------- Timer 3 ... it controls the following pins ----------
  //  PORTE BIT 3, OCR3A (stepper driver current)
  //  PORTE BIT4, OCR3B (Door Light)  !!!! schem error

  TCCR3A = (1<<COM3A1) | (1<<COM3B1) | (1<<WGM31) | (1<<WGM30);
  TCCR3B = (TCCR3B & 0b11111000) | 0x02; // set to 1/8 Prescaler
  //  Set initial duty cycle
  DOOR_LED_OCR = 0;

  // ---------------- TIMER5 ISR SETUP --------------------------

  // Setup a timer5 interrupt to handle timing of things like LED animations and spindle soft start in the background
  TCCR5A = 0;     // Clear entire TCCR1A register
  TCCR5B = 0;     // Clear entire TCCR1B register

  TCCR5B |= (1 << WGM52);  // turn on CTC mode:
  TCCR5B |= (1 << CS52);   // divide clock/256

  OCR5A = CARVIN_TIMING_CTC;  // set compare match register to desired timer count:

  TIMSK5 |= (1 << OCIE5A);  // enable timer compare interrupt (grbl turns on interrupts)

  // ----------------Initial LED SETUP -----------------------------

  // setup LEDs and spindle
  init_pwm(&button_led);
  init_pwm(&door_led);
  init_pwm(&spindle_led);
  init_pwm(&spindle_motor);

  // fade on the button and door LEDs at startup
  set_pwm(&button_led, BUTTON_LED_LEVEL_ON,BUTTON_LED_RISE_TIME);
  set_pwm(&door_led, DOOR_LED_LEVEL_IDLE,DOOR_LED_RISE_TIME);

  setTMC26xRunCurrent(1);
  
}

// Timer5 Interrupt
// keep this fast to not bother the stepper timing
// Things done here......
//  LED Animations
//  Spindle Softstart
//  Button debounce
ISR(TIMER5_COMPA_vect)
{
  // see if the led values need to change
  if (pwm_level_change(&button_led))
  {
    BUTTON_LED_OCR = button_led.current_level;
  }

  if (pwm_level_change(&door_led))
  {
    DOOR_LED_OCR = door_led.current_level;
  }
  if (pwm_level_change(&spindle_led))
  {
    SPINDLE_LED_OCR = spindle_led.current_level;
  }

  if (pwm_level_change(&spindle_motor))
  {
    if(spindle_motor.current_level == 0) {  // added by Brian R. for PWM 0 fix
      SPINDLE_PWM_PORT &= ~(1<<SPINDLE_PWM_BIT);
      TCCRA_REGISTER &= ~(1<<COMB_BIT | 1<<(COMB_BIT-1));
    } else {
      TCCRA_REGISTER = (TCCRA_REGISTER | (1<<COMB_BIT)) & ~(1<<(COMB_BIT-1));
    }
    
    SPINDLE_MOTOR_OCR = spindle_motor.current_level;
  }

  if (control_button_counter > 0)
  {
    control_button_counter--;
    if (control_button_counter == 0)
    {    
      checkControlPins();
    }
  }
  
  if ( spindle_current_proc() )
  {
    printPgmString(PSTR("[OverCurrent:"));
    printFloat(spindle_current_get(), 2);
    printPgmString(PSTR("]\r\n"));
    system_set_exec_state_flag(EXEC_SAFETY_DOOR);
  }
}

// init or reset the led values
void init_pwm(struct pwm_analog * pwm)
{
  (* pwm).current_level = 0;
  (* pwm).duration = 0;
  (* pwm).dur_counter = 0;
  (* pwm).throb = 0;					
  (* pwm).throb_min = 0;
  (* pwm).target = 0;
}

// setup an LED with a new brightness level ... change is done via ISR
void set_pwm(struct pwm_analog * pwm, uint8_t target_level, uint8_t duration)
{
  (* pwm).duration = duration;
  (* pwm).throb = false;
  (* pwm).target = target_level;
}

/* setup an LED for throb ... throbing is done via ISR
    pwm = analog item affected
  min_throb = is the minimum value the level goes to. Effect is better if LEDs never actually go off.
  duration = seconds for the fade
*/
void throb_pwm(struct pwm_analog * pwm, uint8_t min_throb, uint8_t duration)
{
  (* pwm).current_level = 0;
  (* pwm).duration = duration;
  (* pwm).throb = true;
  (* pwm).target = LED_FULL_ON;
  (* pwm).throb_min = min_throb;
}

// Adjusts the level of an LED
// Returns true if the value changed
// This is called by the timer ISR...there is no reason to call it outside the ISR
int pwm_level_change(struct pwm_analog * pwm)
{
  // see if the value needs to change
  if ((* pwm).target == (* pwm).current_level)
  {
    return false;
  }

  // if duration is 0, change the level right away
  if ((* pwm).duration == 0)
  {
    (* pwm).current_level = (* pwm).target;
    return true;
  }

  // the duration counter causes the function to be called more than once before it makes a change by counting down to 1
  if ((* pwm).dur_counter > 1)
  {
    (* pwm).dur_counter--;  // count down to 1
  }
  else
  {
    if ((* pwm).current_level < (* pwm).target)
    {
      (* pwm).current_level++;
      if ((* pwm).throb && (* pwm).current_level == LED_FULL_ON)  // check to see if we need to reverse the throb
      (* pwm).target = (* pwm).throb_min;
    }
    else
    {
      (* pwm).current_level--;
      if ((* pwm).throb && (* pwm).current_level <= (* pwm).throb_min)  // check to see if we need to reverse the throb
        (* pwm).target = LED_FULL_ON;
    }

    (* pwm).dur_counter = (* pwm).duration;  // reset the duration counter
    return true;
  }

  return false;

}

#ifdef GEN1_HARDWARE
void set_stepper_current(float current)
{
  float vref = 0;

  // current = VREF /(8× RS)  from driver datasheet
  vref = current * (8 * I_SENSE_RESISTOR);

  // vref goes through a resistor dividor that cuts the voltage in half
  vref = (vref /2.5) * 1023 / 2;

  STEPPER_VREF_OCR = (int)vref;
}
#endif


void set_button_led()
{
	if ((sys.state == STATE_HOLD) || (sys.state == STATE_SAFETY_DOOR))
	{
		throb_pwm(&button_led, BUTTON_LED_THROB_MIN, BUTTON_LED_THROB_RATE);		
	}
	else
	{
		set_pwm(&button_led, BUTTON_LED_LEVEL_ON,BUTTON_LED_RISE_TIME);
	}
}

uint8_t get_hardware_rev()
{
  uint8_t rev = 0U;
  
  rev = (HRDW_ID_PIN & HRDW_ID_MASK) >> HRDW_ID_0;	
  rev ^= (0b11111);
  
  return (rev);
}

/*
 This is a software driver hard reset using the watchdog timer
 Make sure your bootloader is compatible with a WDT.  Some do not reset the WDT
 and you can get stuck in a WDT reboot loop.
 */
void reset_cpu()
{
  wdt_enable(WDTO_15MS);
  while(1)
  {
    // wait for it...boom
  }
}

/*
Show the state of all of the ports with switches

A typical response will look like this...
{Ctl:00000100,Lim:01110000,Prb:00001000}

The 1's represent the port pin high

This is typically used in mfg testing only

*/
void print_switch_states()
{
  printPgmString(PSTR("{Sw:"));

  printPgmString(PSTR("Ctl:"));
  print_uint8_base2_ndigit(CONTROL_PIN & CONTROL_MASK, 8);

  printPgmString(PSTR(",Lim:"));
  print_uint8_base2_ndigit(LIMIT_PIN & LIMIT_MASK, 8);

  printPgmString(PSTR(",Prb:"));
  print_uint8_base2_ndigit(PROBE_PIN & PROBE_MASK, 8);

  printPgmString(PSTR("}\r\n"));
}
