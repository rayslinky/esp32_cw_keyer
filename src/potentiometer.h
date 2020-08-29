#include "display.h"
#ifdef ESP32
#define default_pot_full_scale_reading 4095
#elif
#define default_pot_full_scale_reading 1023
#endif
#ifdef ESP32
#define potentiometer_reading_threshold 200
#elif
#define potentiometer_reading_threshold 1
#endif
#define potentiometer_always_on 0
#define potentiometer_enable_pin 0 // if defined, the potentiometer will be enabled only when this pin is held low; set to 0 to ignore this pin
#ifdef ESP32
#define potentiometer_change_threshold 0.9
#elif
#define potentiometer_change_threshold 0.9 // don't change the keyer speed until pot wpm has changed more than this
#endif
#define initial_pot_wpm_low_value 13  // Potentiometer WPM fully CCW
#define initial_pot_wpm_high_value 35 // Potentiometer WPM fully CW
class wpmPotentiometer
{
    byte pot_wpm_low_value;
    byte pot_wpm_high_value;
    byte last_pot_wpm_read;
    int pot_full_scale_reading = default_pot_full_scale_reading;
    int potPin;

    byte pot_value_wpm()
    {
        // int pot_read = analogRead(potentiometer);
        // byte return_value = map(pot_read, 0, pot_full_scale_reading, pot_wpm_low_value, pot_wpm_high_value);
        // return return_value;

        static int last_pot_read = 0;
        static byte return_value = 0;
        int pot_read = analogRead(potPin);
        if (abs(pot_read - last_pot_read) > potentiometer_reading_threshold)
        {
            return_value = map(pot_read, 0, pot_full_scale_reading, pot_wpm_low_value, pot_wpm_high_value);
            last_pot_read = pot_read;
        }
        return return_value;
    }

public:
    int initialize(int pin)
    {
        potPin = pin;
        pinMode(potPin, INPUT);
        pot_wpm_low_value = initial_pot_wpm_low_value;
        pot_wpm_high_value = initial_pot_wpm_high_value;
        last_pot_wpm_read = pot_value_wpm();
        configuration.pot_activated = 1;
    }

    void checkPotentiometer(void setWpmCallback(byte))
    {
        static unsigned long last_pot_check_time = 0;

        if ((configuration.pot_activated || potentiometer_always_on) && ((millis() - last_pot_check_time) > potentiometer_check_interval_ms))
        {
            last_pot_check_time = millis();
            if ((potentiometer_enable_pin) && (digitalRead(potentiometer_enable_pin) == HIGH))
            {
                return;
            }
            byte pot_value_wpm_read = pot_value_wpm();
#ifdef ESP32
            if (((abs(pot_value_wpm_read - last_pot_wpm_read) * 10) > (potentiometer_change_threshold * 10)))
            {
#elif
            if (((abs(pot_value_wpm_read - last_pot_wpm_read) * 10) > (potentiometer_change_threshold * 10)))
            {
#endif
                setWpmCallback(pot_value_wpm_read);

                last_pot_wpm_read = pot_value_wpm_read;
            }
        }
    }
};

wpmPotentiometer wpmPot;
