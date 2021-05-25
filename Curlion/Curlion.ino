/*
 * Copyright (C) 2021 Frey Hertz (Pavel Neshumov), Curlion controller
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

// External libraries
#include <EEPROM.h>
#include <SevSeg.h>

/**************************************/
/*            Display pins            */
/**************************************/
#define DISP_V1     5
#define DISP_V2     8
#define DISP_V3     9
#define DUSP_A      6
#define DUSP_B      10
#define DUSP_C      11
#define DUSP_D      1
#define DUSP_E      2
#define DUSP_F      7
#define DUSP_G      12
#define DUSP_DP     0


/************************************/
/*            Other pins            */
/************************************/
#define TEMP_SENS   A0
#define PIN_HEATER  3
#define PIN_BUTTON  4


/*******************************/
/*            Setup            */
/*******************************/
// PD-controller
const float PID_P PROGMEM = 0.42;
const float PID_D PROGMEM = 8.0;

// TEMPERATURE = LOG_FACTOR * log(NTC_RESISTANCE) + LOG_TERM
const float LOG_FACTOR PROGMEM = -24.4;
const float LOG_TERM PROGMEM = 300;

// Supply voltage
const float VCC PROGMEM = 5.0;

// Resistance of the second divider resistor (between GND and A0)
const float SENS_RGND PROGMEM = 22000;

// Kalman filter (larger value = smoother)
const float FILTER_K PROGMEM = 0.85;

// Update time of the display (in ms)
const uint16_t DISP_UPDATE_TIME PROGMEM = 150;

// Max allowed temperature
const uint8_t MAX_SET_TEMP PROGMEM = 170;

// Min allowed temperature
const uint8_t MIN_SET_TEMP PROGMEM = 40;

// Temperature increment
const uint8_t SET_TEMP_INCREMENT PROGMEM = 10;

// After pressing the button for this time, the mode will be switched
const uint16_t BUTTON_LONG_PRESS PROGMEM = 1000;

/***********************************/
/*            Variables            */
/***********************************/
SevSeg sevseg;
byte digit_pins[] = { DISP_V1, DISP_V2, DISP_V3 };
byte segment_pins[] = { DUSP_A, DUSP_B, DUSP_C, DUSP_D, DUSP_E, DUSP_F, DUSP_G, DUSP_DP };
uint8_t mode;   // 0 - init, 2 - show setup, 2 - main loop, 3 - setup
uint8_t disp_counter;
unsigned long disp_timer;
uint8_t int_temperature, set_temperature;
float temperature, vin, r2, raw_temperature;
float sens_devider;
unsigned long button_time_pressed;
boolean button_pressed, button_last_state, button_mode_changing;
float pid_error, pid_output, previous_error;

void setup()
{
    // Hardware setup
    sevseg.begin(COMMON_ANODE, 3, digit_pins, segment_pins, false, false, false);
    pinMode(PIN_HEATER, OUTPUT);
    pinMode(PIN_BUTTON, INPUT_PULLUP);

    // Read setup temperature from the EEPROM
    set_temperature = EEPROM.read(0);
    if (set_temperature > MAX_SET_TEMP || set_temperature < MIN_SET_TEMP) {
        set_temperature = MIN_SET_TEMP;
        EEPROM.write(0, set_temperature);
    }
}

void loop()
{
    // Refresh temperature value
    update_temperature(false);

    // Enable or disable heating element (PID)
    heater_handler();
    
    // Select current loop
    if (mode == 2)
        loop_main();
    else if (mode == 3)
        loop_setup();
    else if (mode == 1)
        loop_show_setup();
    else
        loop_init();

    // Update display every cycle
    sevseg.refreshDisplay();
}

/// <summary>
/// First run ("Push the button" ticker)
/// </summary>
void loop_init() {
    // Stay in the loop until the button is clicked
    if (digitalRead(PIN_BUTTON))
    {
        // Refresh temperature value (every cycle is fresh value)
        update_temperature(true);

        if (millis() - disp_timer >= DISP_UPDATE_TIME) {
            // Refresh text
            disp_push_the_button();
            // Increment counter
            if (disp_counter > 18)
                disp_counter = 0;
            disp_counter++;
            // Store timer
            disp_timer = millis();
        }
    }
    else {
        // Reset counter
        disp_counter = 0;
        // Switch to show_setup (1) loop if button was released
        mode = 1;
    }
}

/// <summary>
/// Second loop (shows setup temperature for a short time)
/// </summary>
void loop_show_setup() {
    if (millis() - disp_timer >= DISP_UPDATE_TIME / 2) {
        if (disp_counter % 2 == 0)
            sevseg.setNumber(set_temperature);
        else
            sevseg.setChars("   ");
        disp_counter++;
        if (disp_counter > 20) {
            // Switch to main (2) loop if time was passed
            mode = 2;
            // Reset counter
            disp_counter = 0;
        }
        disp_timer = millis();
    }
}

/// <summary>
/// Main loop (heating + temperature)
/// </summary>
void loop_main() {
    button_pressed = !digitalRead(PIN_BUTTON);

    if (!button_last_state && button_pressed) {
        // If button was pushed
        button_last_state = 1;
        // Remember time
        button_time_pressed = millis();
    }

    if (!button_mode_changing && button_last_state && button_pressed && millis() - button_time_pressed > BUTTON_LONG_PRESS) {
        // If button was pressed for a long time
        // Change mode to setup (3)
        mode = 3;

        // Set mode_changing flag to true (to prevent looped mode changing)
        button_mode_changing = 1;
    }

    if (!button_pressed) {
        // Reset flags if button was released
        button_last_state = 0;
        button_mode_changing = 0;
    }

    if (millis() - disp_timer >= DISP_UPDATE_TIME) {
        // Display dot at end if heater is on
        if (digitalRead(PIN_HEATER))
            sevseg.setNumber(int_temperature, 0);
        else
            sevseg.setNumber(int_temperature);
        disp_timer = millis();
    }
}

/// <summary>
/// Setup loop (blinking display)
/// </summary>
void loop_setup() {
    button_pressed = !digitalRead(PIN_BUTTON);

    if (!button_last_state && button_pressed) {
        // If button was pushed
        button_last_state = 1;
        // Remember time
        button_time_pressed = millis();
    }

    if (!button_mode_changing && button_last_state && button_pressed && millis() - button_time_pressed > BUTTON_LONG_PRESS) {
        // If button was pressed for a long time
        // Change mode to main (2)
        mode = 2;

        // Set mode_changing flag to true (to prevent looped mode changing)
        button_mode_changing = 1;
    }

    if (button_last_state && !button_pressed && millis() - button_time_pressed < BUTTON_LONG_PRESS) {
        // If button was pressed for a short time
        set_temperature += SET_TEMP_INCREMENT;
        if (set_temperature > MAX_SET_TEMP)
            set_temperature = MIN_SET_TEMP;
        EEPROM.write(0, set_temperature);
    }

    if (!button_pressed) {
        // Reset flags if button was released
        button_last_state = 0;
        button_mode_changing = 0;
    }

    if (millis() - disp_timer >= DISP_UPDATE_TIME) {
        // Display blinking setup temperature
        if (disp_counter)
            sevseg.setNumber(set_temperature);
        else
            sevseg.setChars("   ");
        disp_counter = !disp_counter;
        disp_timer = millis();
    }
}

/// <summary>
/// Draws a ticker with the words "Push the button"
/// </summary>
void disp_push_the_button() {
    switch (disp_counter)
    {
    case 1:
        sevseg.setChars("  P");
        break;
    case 2:
        sevseg.setChars(" PU");
        break;
    case 3:
        sevseg.setChars("PUS");
        break;
    case 4:
        sevseg.setChars("USH");
        break;
    case 5:
        sevseg.setChars("SH ");
        break;
    case 6:
        sevseg.setChars("H T");
        break;
    case 7:
        sevseg.setChars(" TH");
        break;
    case 8:
        sevseg.setChars("THE");
        break;
    case 9:
        sevseg.setChars("HE ");
        break;
    case 10:
        sevseg.setChars("E B");
        break;
    case 11:
        sevseg.setChars(" BU");
        break;
    case 12:
        sevseg.setChars("BUT");
        break;
    case 13:
        sevseg.setChars("UTT");
        break;
    case 14:
        sevseg.setChars("TTO");
        break;
    case 15:
        sevseg.setChars("TON");
        break;
    case 16:
        sevseg.setChars("ON ");
        break;
    case 17:
        sevseg.setChars("N  ");
        break;
    default:
        sevseg.setChars("   ");
        break;
    }
}

/// <summary>
/// Turns on or off the heater according to the temperature
/// </summary>
void heater_handler() {
    if (mode == 2) {
        // Calculate error
        pid_error = (float)set_temperature - temperature;

        // Calculate PID output
        pid_output = PID_P * pid_error + PID_D * (pid_error - previous_error);

        // Store error for next cycles
        previous_error = pid_error;

        if (pid_output > 1.0)
            // Enable heater if pid_output is positive
            digitalWrite(PIN_HEATER, 1);
        else
            // Disable heater if pid_output is negative
            digitalWrite(PIN_HEATER, 0);
    }
    else
        // Turn off the heater in other modes
        digitalWrite(PIN_HEATER, 0);
}

/// <summary>
/// Calculates current temperature
/// </summary>
/// <param name="first_run"> if true, the filter will not be used </param>
void update_temperature(boolean first_run) {
    // Calculate unfiltered temperature
    vin = (float)analogRead(TEMP_SENS) * (VCC / 1023.0);
    r2 = (SENS_RGND * VCC) / vin - SENS_RGND;
    raw_temperature = LOG_FACTOR * log(r2) + LOG_TERM;

    // Filter temperature if needed
    if (!first_run)
        temperature = temperature * FILTER_K + raw_temperature * (1.0 - FILTER_K);
    else
        temperature = raw_temperature;

    // Clip temperature to 0-255 range
    if (temperature > 255)
        temperature = 255;
    else if (temperature < 0)
        temperature = 0;

    // Convert to integer
    int_temperature = (uint8_t)temperature;
}
