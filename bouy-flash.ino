// Model Buoy code V1.3

// 2025-05-11 - Added the calls to set debounce and press time and decided to call it 1.0
// 2025-05-11 - tiny412 sleep/wake was rubbish and was doing a reset. Now fixed so SLEEP_AT_POWER_UP works. Released as 1.1.
// 2025-05-13 - Coded up UART out, VCC reading, and buoy LED PWM curve based on VCC.
// 2025-05-14 - Released as 1.2 after tweaking the pwm_tables and adding battery profiles.
// 2025-05-14 - Sussed the UART pins and switched TX to PA6, condensed VCC reading based on megaTinyCore example, but it was larger so swapped back.
// 2025-05-15 - Fixed the Fast versions of the digital read/write so 328P working again, added option to turn off serial, stripped code back to maximise free space for flash patterns
// 2025-05-16 - Released as 1.3

// ToDo
//   

// Code is written for a ATtiny412 but will also work on a 328P on something like an Uno

// Fuses for ATtiny412
// Clock is fine at 1MHz
// Leave startup at 8ms and millis enabled
// Leave BOD and WDT disabled
// Enabling Save EEPROM means that flash mode setting will survive a reflash
// Leave printf at Default as it isn't used and this setting means it doesn't get included in the code.
// Wire library isn't used so it doesn't matter
// Leave PWM at Default
// attachInterrupt isn't used.
// You need to use Burn Bootloader after changing most of these. There isn't a bootloader but it updates the fuses.
// It isn't mentioned in the menu, but various clock changes also require this, so do it as matter of course unless you care to learn the rules.

// Serial is initialised and used for some debug. The same serial as used for UPDI can be switched to the TX pin after reflash to see serial.
// Leaving SLEEP_AT_POWER_UP defined means you have chance to flick switch or move wire.

// After power up (LED goes from off to bright in a sweep) then there is a confirmation flash showing which pattern is selected.
// So one flash for patternA etc.
// It then does that pattern until SHOW_MINS time is reached or a button is pressed.
// A single press advances to next pattern (double to previous) and you again get the confirmation flash(es).
// A long press (800 milliseconds by default) puts the device into power saving mode and the LED dims to show this.
// This mode is also entered after SHOW_MINS

#include <Arduino.h>
#include <OneButtonTiny.h>      // https://github.com/mathertel/OneButton/blob/master/README.md
#include <avr/sleep.h>
#include <avr/interrupt.h>
#include <avr/io.h>
#include <EEPROM.h>

#define SERIAL_OUT                 // Whether to include serial code/init/output - relatively bulky for tiny412 at 190 bytes
#define SLEEP_AT_POWER_UP          // Means a button push is needed to wake after power up or reflashing.
#define SHOW_MINS 12*60            // Time to show pattern before drifting off the sleep

/* Guide to batteries and brightness
    Set VCC_MAX_MV to the fully charged (or out of packet) voltage. LED PWM will hopefully drop the brightness at these dizzying heights.
    Do all dropper resistor and pin current calculations at this voltage.
    Batteries then drop quickly to a lower figure, typically 3V7 for lithium, 2V7 for CR2032, etc.
    Something below this for VCC_MIN_MV makes sense as you want 100% PWM for the bulk of the operating life.
    Cell/battery voltages below this will then see reduced LED brightness.
    To protect lithium cells, a 3V2 cutoff in MIN_VCC_MV works well and 3V3 might be better.
    Below this voltage the chip will go to sleep. It can be woken up again but will then turn off again.
    For primary cells this is currently set to just above the voltage where the chip powers off anyway, but it's not too critical
*/

// Only used on tiny412 but no harm in definiing them anyway
#define BATTERY_3V_PRIMARY
//#define BATTERY_3V7_LITHIUM

#if defined(BATTERY_3V7_LITHIUM)
# define VCC_MAX_MV 4200            // Highest VCC we should see
# define VCC_MIN_MV 3300            // Min for the PWM brightness table
# define MIN_VCC_MV 3200            // tiny412 will deep sleep if this level reached. 
#endif
#if defined(BATTERY_3V_PRIMARY)
# define VCC_MAX_MV 3000            // Highest VCC we should see
# define VCC_MIN_MV 2700            // Min for the PWM brightness table. 
# define MIN_VCC_MV 1820            // tiny412 will deep sleep if this level reached. 
#endif

#if defined (__AVR_ATtiny412__)
# define tiny412
#endif

/*  412 pinout with functions for this code (RX not currently setup, used, or code written!)
            +---U---+
  VDD    -> |1     8| <- GND
  PA6 TX <- |2     7| -> PA3 LED_BUILTIN
  PA7 RX <- |3     6| -> PA0 UPDI
  PA1    <- |4     5| -> PA2 Button Input
            +-------+
*/

// --- Globals ---

// Different button pins on 412 versus Uno
#if defined(tiny412)
# define BUTTON_PIN PIN_PA2    // For attiny412. If you change this, the sleep function needs coding to use a different interrupt.
#else
# define BUTTON_PIN 2          // Prototyping Uno. If you change this, the sleep function needs coding to use a different interrupt.
#endif

// Different LED pins on 412 versus Uno. Uno could have both LEDs on pin 9 but it needs to be a PWM pin (3, 5, 6, 9, 10 or 11)
// If you want inverted output on 412 (because you're using an NPN for more drive?) you can uncomment the pinConfigure line in SetupPins
#if defined(tiny412)
  const uint8_t BUOY_LED_PIN = LED_BUILTIN; // Or any valid pin
  const uint8_t MMI_LED_PIN = BUOY_LED_PIN; // Or any valid pin. PIN_PA1 is a good option
#else
  const uint8_t BUOY_LED_PIN = LED_BUILTIN; // Or any valid pin
  const uint8_t MMI_LED_PIN = 9;      // Or any valid pin, even same as BUOY_LED_PIN. 9 is good on UNO as PWM
#endif

#if defined(SERIAL_OUT)
# define BAUD_RATE 19200
  uint16_t baud;
#endif

// Note we don't always use digitalWriteFast
// As per the megaTiny Core docs 'The fast digital I/O functions do not turn off PWM as that is inevitably slower (far slower) than writing to pins and they would no longer be "fast" digital I/O.'
// So whenever PWM is (or has been) in use (sweepLED and runPattern) we take care to use digitalWrite() to turn it off.

#if !defined(tiny412)     // We use these for speed with microTinyCores but need fall backs for 328 etc.
# define digitalWriteFast(PIN, VALUE) digitalWrite(PIN, VALUE)
# define pinModeFast(PIN, MODE) digitalWrite(PIN, MODE)
# define digitalReadFast(PIN) digitalRead(PIN)
#endif

// We define these so that they can be set as pulled up inputs to save power
#if defined(tiny412)
# define unused1 PIN_PA6
# define unused2 PIN_PA7
# define unused3 PIN_PA1       // Is used for USART0 TX if uart initialised but OK setting as input to start with
#endif

// Mark/space for confirmation flash
#define CONFIRM_ON 100
#define CONFIRM_OFF 300

// See OneButton docs before changing
#define DEBOUNCE_MS 50
#define PRESS_MS 800      // It might make sense to up this to a couple of seconds and change code so this is used to change sequence. Click is then on/off

uint8_t currentPatternIndex = 0;

OneButtonTiny button (
  BUTTON_PIN,  // Input pin for the button
  true,       // Button is active high
  true        // Enable internal pull-up resistor
);

enum Button_States {
  IDLE,
  CLICK,
  DOUBLECLICK,
  LONGPRESS,
  SLEEP                 // Not really a button event/state but gives code a consistent way control sleep
} Button_Event = IDLE;

// --- Type definitions ---

struct FlashStep {
  bool ledOn;
  uint16_t duration; // ms
};

struct FlashPattern {
  const FlashStep* steps;
  uint8_t numSteps;
};

struct PatternBank {
  const FlashPattern* patterns;
  uint8_t numPatterns;
};

// /////////////////////////////////////////////////
// -- Let's define lots of exciting flash patterns!
// /////////////////////////////////////////////////

// Pattern A: “Iso 4s” (equal on/off, 2s each)
FlashStep patternA_steps[] = {
  {true, 2000},  // LED on 2s
  {false, 2000}  // LED off 2s
};

FlashPattern patternA = {
  .steps = patternA_steps,
  .numSteps = sizeof(patternA_steps) / sizeof(patternA_steps[0])
};

// Pattern B: “Fl(3) 10s” (3 flashes, then long pause to make 10s total)
FlashStep patternB_steps[] = {
  {true, 300}, {false, 300},
  {true, 300}, {false, 300},
  {true, 300}, {false, 6500}  // total = 10s cycle
};

FlashPattern patternB = {
  .steps = patternB_steps,
  .numSteps = sizeof(patternB_steps) / sizeof(patternB_steps[0])
};

// Pattern C: “12 flashy” (12 flashes, then long pause)
FlashStep patternC_steps[] = {
  {true, 300}, {false, 300},
  {true, 300}, {false, 300},
  {true, 300}, {false, 300},
  {true, 300}, {false, 300},
  {true, 300}, {false, 300},
  {true, 300}, {false, 300},
  {true, 300}, {false, 300},
  {true, 300}, {false, 300},
  {true, 300}, {false, 300},
  {true, 300}, {false, 300},
  {true, 300}, {false, 300},

  {true, 300}, {false, 6500}
};

FlashPattern patternC = {
  .steps = patternC_steps,
  .numSteps = sizeof(patternC_steps) / sizeof(patternC_steps[0])
};

FlashPattern allPatterns[] = { patternA, patternB, patternC };
const uint8_t NUM_PATTERNS = sizeof(allPatterns) / sizeof(allPatterns[0]);

PatternBank bank = {
  .patterns = allPatterns,
  .numPatterns = NUM_PATTERNS
};


// /////////////////////////////////////////////////
// Setup
// /////////////////////////////////////////////////

void setupPins () {
  // Pin setup.
  pinModeFast(BUTTON_PIN, INPUT_PULLUP);

  pinModeFast(MMI_LED_PIN, OUTPUT);
  digitalWriteFast(MMI_LED_PIN, LOW);
  
  pinModeFast(BUOY_LED_PIN, OUTPUT);
  //pinConfigure(BUOY_LED_PIN, PIN_DIR_OUTPUT, PIN_INVERT_ON);   // Invert output
  digitalWriteFast(BUOY_LED_PIN, LOW);

  // Saves power,
#if defined(tiny412)
  pinModeFast(unused1, INPUT_PULLUP);
  pinModeFast(unused2, INPUT_PULLUP);
  pinModeFast(unused3, INPUT_PULLUP);
#endif

}

void setup() {

  setupPins();
  delay(10);      // We seem to need some delay or the sleepNow() has issues maybe due to pullups

#if defined(SERIAL_OUT)
# if defined(tiny412)
  uart_init();
# else
  Serial.begin(BAUD_RATE);
# endif
#endif

  // Restore currentPatternIndex from EEPROM
  EEPROM.get(0, currentPatternIndex);
  if (currentPatternIndex >= NUM_PATTERNS || currentPatternIndex < 0) currentPatternIndex = 0;

  // Configure times for OneButton and some s/w timers
  // If you use defaults, and are short of space, comment this out.
  button.setDebounceMs(DEBOUNCE_MS);
  button.setPressMs(PRESS_MS);

  // Attach the click handlers we need
  button.attachClick(click_function);
  button.attachDoubleClick(doubleclick_function);
  button.attachLongPressStart(longpress_function);

// Means we don't start up as soon as power applied. Dunno in final product, but handy when debugging to see serial out.
#if defined(SLEEP_AT_POWER_UP)
  sleepNow(true);
#endif

#if defined(SERIAL_OUT)
# if defined(tiny412)
    uart_print("Hello World!");
    uart_lf();
# else
    Serial.println("Hello World!");
# endif
#endif // SERIAL_OUT

  sweepLED(true, 1000);     // Amazing boot up effect

}

// /////////////////////////////////////////////////
// Loop time, we love loops
// /////////////////////////////////////////////////

bool buttonPressed() {
  return digitalReadFast(BUTTON_PIN) == LOW;
}

void waitForButtonRelease() {
  // Wait until released with debounce
  while (buttonPressed());
  delay(DEBOUNCE_MS);
  while (buttonPressed());
}

// We don't go round this loop very often, really just on button action.
void loop() {

  confirmPattern(currentPatternIndex + 1);        // Show pattern about to do - way better to do on a different LED
  if (BUOY_LED_PIN == MMI_LED_PIN) waitInterruptible(1000);    // Delay if just one LED for both functions
  
  if (Button_Event == IDLE) {       // I tried having this inside the switch and falling through but it didn't work. Odd.
      // Store currentPatternIndex to EEPROM
      EEPROM.put(0, currentPatternIndex);
      runPattern(&bank.patterns[currentPatternIndex], SHOW_MINS);   // Do it unless more button action
      digitalWrite(BUOY_LED_PIN, LOW); // Not done for every exit path so mop up. Don't use Fast as PWM might still be on.
      //uart_print_uint(Button_Event);
      //uart_lf();
  }
  switch (Button_Event) {
    case CLICK: {
        // Move to next pattern
        if (++currentPatternIndex >= NUM_PATTERNS ) currentPatternIndex = 0;
        break;
    }
    case DOUBLECLICK: {
        // Back up a pattern
        if (--currentPatternIndex < 0) currentPatternIndex = NUM_PATTERNS - 1;
        break;
    }
    case LONGPRESS:
    case SLEEP:
    case IDLE: {
        // This is either a power down request or runPattern got bored
        sweepLED(false, 1000);
        digitalWriteFast(BUOY_LED_PIN, LOW);  // Probably not required, but one word of flash
        waitForButtonRelease();
        sleepNow(true);
        waitForButtonRelease();
        sweepLED(true, 1000);
        break;
     }
  }

  Button_Event = IDLE;  // Whatever happened is over, forget it, move on.
}

// /////////////////////////////////////////////////
// Click Handlers
// /////////////////////////////////////////////////


void click_function() {
  //Serial.println("Click");
  Button_Event = CLICK;
}

void longpress_function() {
  //Serial.println("Long Press");
  Button_Event = LONGPRESS;
}

void doubleclick_function() {
  //Serial.println("Double Click!");
  Button_Event = DOUBLECLICK;
}

// /////////////////////////////////////////////////
// runPattern etc.
// /////////////////////////////////////////////////

uint8_t buoy_PWM_level = 1;

void setBuoyLEDPWM(uint8_t level) {
#if defined(tiny412)
  analogWrite(BUOY_LED_PIN, level);
#else
  digitalWrite(BUOY_LED_PIN, HIGH);
#endif
}

// Show the main pattern until button press or time out
// Note that ledPin is passed in, so Fast writes can't be used, but we need PWM to be turned off anyway.
void runPattern(const FlashPattern* pattern, uint16_t durationMinutes) {

  uint32_t startTime = millis();
  uint32_t maxDuration = (uint32_t)durationMinutes * 60000UL;

  while (durationMinutes == 0 || (millis() - startTime) < maxDuration) {

// Calculate PWM level and also check if time to sleep due to low battery
#if defined(tiny412)
    uint16_t vcc_mv = getVCCmv();

# if defined(SERIAL_OUT)
    uart_print("VCC in mV = ");
    uart_print_uint(vcc_mv);
    uart_lf();
# endif

    if (vcc_mv <= MIN_VCC_MV) Button_Event = SLEEP;
    buoy_PWM_level = getBuoyPWM(vcc_mv);
#endif

    // Run the pattern
    for (uint8_t i = 0; i < pattern->numSteps; ++i) {
      if (pattern->steps[i].ledOn)
        setBuoyLEDPWM(buoy_PWM_level);
      else
        digitalWrite(BUOY_LED_PIN, LOW);      // Much safer to not use Fast as we want PWM off.
      if (waitInterruptible(pattern->steps[i].duration)) return;

    }
  }
  Button_Event = SLEEP;       // We hit set duration so exit and sleep
}


// Quick confirmation flash of selected pattern
void confirmPattern(uint8_t count) {
  waitInterruptible(500);
  for (uint8_t i = 0; i < count; ++i) {
    digitalWriteFast(MMI_LED_PIN, HIGH);
    if (waitInterruptible(CONFIRM_ON)) return;
    digitalWriteFast(MMI_LED_PIN, LOW);
    if (waitInterruptible(CONFIRM_OFF)) return;
  }
  if (waitInterruptible(500)) {
    return; // small pause after confirmation
  }
}

// like delay() but bails on button event
bool waitInterruptible(uint16_t duration) {
  uint32_t start = millis();
  while ((millis() - start) < duration) {
    button.tick();
    if (Button_Event != IDLE) return true;
    delay(10);
  }
  return false;
}

// /////////////////////////////////////////////////
// Sleepy stuff, zzzzz
// /////////////////////////////////////////////////

void sleepNow(bool allowWake) {

  noInterrupts();  // Stuff to do in peace

  // Enable pin change interrupt on BUTTON_PIN
#if defined(tiny412)
  // Assuming BUTTON_PIN is on PA2 (PCINT2)
  PORTA.PIN2CTRL = PORT_PULLUPEN_bm | PORT_ISC_LEVEL_gc;  // Don't include PORT_ISC_LEVEL_gc and we'll sleep forever
  PORTA.INTFLAGS = PORT_INT2_bm;            // Clear flag
#else
  // For ATmega328P (Uno)
  PCMSK2 |= (1 << PCINT18); // Enable PCINT18 (D2) - miss this and we sleep forever
  PCIFR |= (1 << PCIF2);    // Clear interrupt flag
  PCICR |= (1 << PCIE2);    // Enable PCINT2 group
#endif

  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep_enable();
  interrupts();
  sleep_cpu();
  sleep_disable(); // Execution resumes here after wake

  // Turn off the button/input interrupts
#if defined(tiny412)
  PORTA.PIN2CTRL = PORT_PULLUPEN_bm;          // Didn't work in ISR using LEVEL and nearly drove me mad.
#else
  PCMSK2 &= ~(1 << PCINT18); // Disable PCINT18 (D2)
#endif

}

// Wake interrupt handlers
#if defined(tiny412)
ISR(PORTA_PORT_vect) {
  // Clear interrupt flags (or it may re-trigger)
  PORTA.INTFLAGS = PORT_INT2_bm;  // for PA2

}
#else
ISR(PCINT2_vect) {
  // Just wake
}
#endif

// /////////////////////////////////////////////////
// LED effect. Don't get too excited.
// /////////////////////////////////////////////////


// Dim or brighten LED.
const uint8_t led_table[51] = {
  0, 0, 0, 0, 0, 1, 1, 2, 3, 4,
  6, 7, 9, 11, 13, 15, 17, 20, 22, 25,
  28, 31, 34, 37, 41, 44, 48, 52, 56, 60,
  64, 69, 73, 78, 83, 88, 93, 98, 103, 109,
  114, 120, 126, 132, 138, 144, 150, 157, 163, 170,
  177
};

void sweepLED(bool turnOn, unsigned long duration_ms) {
  const int steps = 50;
  const unsigned long delay_per_step = duration_ms / steps;

  for (int i = 0; i <= steps; ++i) {
    uint8_t level = turnOn ? led_table[i] : led_table[steps - i];
    analogWrite(MMI_LED_PIN, level);
    delay(delay_per_step);
  }
  digitalWrite(MMI_LED_PIN, LOW);       // Do not change to the Fast version as we need the code to realise PWM needs turning off. Oddly it works elsewhere!

}


// /////////////////////////////////////////////////
// tiny421 USART0 basic driver for debug
// /////////////////////////////////////////////////

#if defined(tiny412) && defined(SERIAL_OUT)

# define USART0_BAUD_RATE(BAUD_RATE) ((uint32_t)(F_CPU * 64 / (16 * (uint32_t)BAUD_RATE)))

  void uart_init(void) {
    PORTA.DIRSET = PIN6_bm;             // TX = PA6, not that the datasheet mentions this
    baud = USART0_BAUD_RATE(BAUD_RATE);
    USART0.BAUD = baud;
    USART0.CTRLB |= USART_TXEN_bm;        // Normal speed, TX enable
    //PORTMUX.CTRLB = PORTMUX_USART0_bm; // As USART is the alternate function of PA1
  }
  
  void uart_tx(char c) {
    while (!(USART0.STATUS & USART_DREIF_bm)); // Wait until ready
    USART0.TXDATAL = c;
  }
  
  void uart_print(const char *s) {
    while (*s) uart_tx(*s++);
  }
  
  void uart_print_uint(uint16_t val) {
    char buf[6];
    uint8_t i = sizeof(buf);
    buf[--i] = 0;
    do {
      buf[--i] = '0' + (val % 10);
      val /= 10;
    } while (val);
    uart_print(&buf[i]);
  }
  
  void uart_lf() {
    uart_tx(10);
  }

#endif

// /////////////////////////////////////////////////
// Power handling.
// /////////////////////////////////////////////////

#if defined (tiny412)

# if defined(BATTERY_3V7_LITHIUM)
// This table has been eyeball tweaked to give constant(ish) brightness across the operative voltage range.
// Highest voltage at start (with lowest PWM drive) and lowest vice versa
# define TABLE_SIZE 8
  const uint8_t pwm_table[TABLE_SIZE] = {100, 121, 142, 163, 184, 205, 230, 255};
#endif

# if defined(BATTERY_3V_PRIMARY)
// This table has been eyeball tweaked to give constant(ish) brightness across the operative voltage range.
// Highest voltage at start (with lowerest PWM drive) and lowest vice versa
# define TABLE_SIZE 8
  const uint8_t pwm_table[TABLE_SIZE] = {160, 173, 186, 199, 213, 227, 241, 255};
#endif

#endif

// Get VCC in mv and use it to calculate a buoy LED PWM
# if defined(tiny412)
  uint32_t getVCCmv() {

# if TRUE
// I hand coded this and then found a shortcut in the megaTinyCore docs. It was larger and slower, hence back in!
  // Set VREF to 1.1V
  VREF.CTRLA = VREF_ADC0REFSEL_1V1_gc;

  // Set reference to VDD
  ADC0.CTRLC = ADC_PRESC_DIV4_gc | ADC_REFSEL_VDDREF_gc;

  // Select internal 1.1 V reference as input
  ADC0.MUXPOS = ADC_MUXPOS_INTREF_gc;

  // Enable ADC
  ADC0.CTRLA = ADC_ENABLE_bm | ADC_FREERUN_bm | ADC_RESSEL_10BIT_gc;

  // Wait for voltage reference to stabilise (optional)
  ADC0.INTFLAGS = ADC_RESRDY_bm;
  ADC0.COMMAND = ADC_STCONV_bm;

  // Wait for result
  while (!(ADC0.INTFLAGS & ADC_RESRDY_bm));
  uint16_t result = ADC0.RES;

  // Disable ADC as it's a power hog
  ADC0.CTRLA |= ADC_ENABLE_bm;
  */
// 3588 above versus 3624 for "shorter version" below!
# else
  //ADCPowerOptions(ADC_ENABLE);
  analogReference(INTERNAL1V1); // set reference to the desired voltage, and set that as the ADC reference.
  analogReference(VDD); // Set the ADC reference to VDD. Voltage selected previously is still the selected, just not set as the ADC reference.
  uint16_t result = analogRead(ADC_INTREF); // proceed to measure the analog reference.
  ADCPowerOptions(ADC_DISABLE); 

  return ( (1024 * 1.1 * 1000) / result);
}
# endif

// Get a suitable PWM signal for the bouy based VCC and doing some twiddling
uint8_t getBuoyPWM(uint16_t vcc_mv) {

  uint16_t span_mv = VCC_MAX_MV - VCC_MIN_MV; // 2400
  uint16_t step_mv = span_mv / (TABLE_SIZE - 1); // 343

  uint16_t clamped_vcc = vcc_mv;
  if (clamped_vcc > VCC_MAX_MV) clamped_vcc = VCC_MAX_MV;
  if (clamped_vcc < VCC_MIN_MV) clamped_vcc = VCC_MIN_MV;

  uint16_t offset = VCC_MAX_MV - clamped_vcc;

  // Fixed-point scaling to 0–7
  uint8_t index = (offset + (step_mv / 2)) / step_mv;

  return ( pwm_table[index]);
}

#endif // defined(tiny412)
