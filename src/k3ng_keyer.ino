#define CODE_VERSION "2020.02.13.01"
#define M5CORE
#define eeprom_magic_number 35 // you can change this number to have the unit re-initialize EEPROM

#include <stdio.h>
#include <queue>
#include <string>
#include "keyer_hardware.h"
#include "keyer_features_and_options.h"
#include "keyer.h"
#include "keyer_dependencies.h"
#include "keyer_debug.h"
#include "keyer_pin_settings.h"
#include "keyer_settings.h"
#include "display.h"
#include "config.h"
#if !defined M5CORE
#include "potentiometer.h"
#endif
#include "cw_utils.h"

#define memory_area_start 113

#if defined(ESP32)
void add_to_send_buffer(byte incoming_serial_byte);

#if defined M5CORE
#include <M5Stack.h>
#endif

#if !defined M5CORE
#include "keyer_esp32now.h"
#endif

#include "keyer_esp32.h"
#endif

// Different hardware might need different tone control capabilities,
// so the approach is to use a global object that implements
// .tone and .noTone.
#if defined M5CORE
#include "tone/M5Tone.h"
M5Tone derivedFromToneBase;
#endif

// make sure your derived object is named derivedFromToneBase
// and implements toneBase's .tone and .noTone
toneBase &toneControl{derivedFromToneBase};

// Different ways to persist config
// E.g. if you wanted sd card inherit from persitentConfig
#if defined(ESP32)
#include "persistentConfig/spiffsPersistentConfig.h"
SpiffsPersistentConfig persistConfig;
#endif

// make sure your derived object is named persistConfig
// and implements .initialize and .save
persistentConfig &configControl{persistConfig};

#include "displays/M5KeyerDisplay.h"
M5KeyerDisplay disp;
displayBase &displayControl{disp};

#include "virtualPins/virtualPins.h"
VirtualPins virtualPins;
#include "virtualPins/m5VirtualButtonPin.h"
M5VirtualButtonPin btnA{M5Btnslist::A};
M5VirtualButtonPin btnC{M5Btnslist::C};
#include "virtualPins/virtualArduinoStyleInputPin.h"
VirtualArduinoStyleInputPin p2(2, VitualArduinoPinPull::VAPP_HIGH);
VirtualArduinoStyleInputPin p5(5, VitualArduinoPinPull::VAPP_HIGH);

VirtualPin &vpA{btnA};
VirtualPin &vpC{btnC};
VirtualPin &vp2{p2};
VirtualPin &vp5{p5};

#include "virtualPins/virtualPinSet.h"
VirtualPinSet ditPaddles;
VirtualPinSet dahPaddles;

// move this later
void lcd_center_print_timed_wpm();

#include "webServer/wifiUtils.h"
WifiUtils wifiUtils{};

#include "webServer/keyerWebServer.h"
KeyerWebServer *keyerWebServer;

std::queue<String> injectedText;

#include "paddleMonitoring/paddleReader.h"
PaddleReader *paddleReader;

void setup()
{

#if defined M5CORE
  M5.begin();
  Serial.begin(115200);
  Serial.println("In setup()");
#endif

  //the web server needs a wifiutils object to handle wifi config
  //also needs place to inject text
  keyerWebServer = new KeyerWebServer(&wifiUtils, &injectedText, &configControl);

  ditPaddles.pins.push_back(&vpA);
  ditPaddles.pins.push_back(&vp2);
  dahPaddles.pins.push_back(&vpC);
  dahPaddles.pins.push_back(&vp5);

  virtualPins.pinsets.insert(std::make_pair(paddle_left, &ditPaddles));
  virtualPins.pinsets.insert(std::make_pair(paddle_right, &dahPaddles));

#ifdef ESPNOW_WIRELESS_KEYER
#ifndef M5CORE
  initialize_espnow_wireless(speed_set);
#endif
#endif
  initialize_pins();

  initialize_keyer_state();
  configControl.initialize(primary_serial_port);

#if !defined M5CORE
  // potentiometer
  wpmPot.initialize(potentiometer_pin);
  configControl.configuration.pot_activated = 1;
#endif

  initialize_default_modes();
  //configControl.configuration.wpm = 15;
  check_for_beacon_mode();
  displayControl.initialize([](char x) {
    send_char(x, KEYER_NORMAL);
  });
  wifiUtils.initialize();

  keyerWebServer->start();
  paddleReader = new PaddleReader([]() { return virtualPins.pinsets.at(paddle_left)->digRead(); },
                                  []() { return virtualPins.pinsets.at(paddle_right)->digRead(); },
                                  configControl.configuration.wpm);
}

// --------------------------------------------------------------------------------------------

void loop()
{

  check_paddles();

  service_dit_dah_buffers();

  service_send_buffer(PRINTCHAR);
  check_ptt_tail();

#if !defined M5CORE
  wpmPot.checkPotentiometer(wpmSetCallBack);
#endif

  check_for_dirty_configuration();

  check_paddles();
  service_send_buffer(PRINTCHAR);
  displayControl.service_display();

  service_paddle_echo();

  service_millis_rollover();
  wifiUtils.processNextDNSRequest();
  keyerWebServer->handleClient();
  service_injected_text();
}

byte service_tx_inhibit_and_pause()
{

  byte return_code = 0;
  static byte pause_sending_buffer_active = 0;

  if (tx_inhibit_pin)
  {
    if ((digitalRead(tx_inhibit_pin) == tx_inhibit_pin_active_state))
    {
      dit_buffer = 0;
      dah_buffer = 0;
      return_code = 1;
      if (send_buffer_bytes)
      {
        clear_send_buffer();
        send_buffer_status = SERIAL_SEND_BUFFER_NORMAL;
      }
    }
  }

  if (tx_pause_pin)
  {
    if ((digitalRead(tx_pause_pin) == tx_pause_pin_active_state))
    {
      dit_buffer = 0;
      dah_buffer = 0;
      return_code = 1;
      if (!pause_sending_buffer_active)
      {
        pause_sending_buffer = 1;
        pause_sending_buffer_active = 1;
        delay(10);
      }
    }
    else
    {
      if (pause_sending_buffer_active)
      {
        pause_sending_buffer = 0;
        pause_sending_buffer_active = 0;
        delay(10);
      }
    }
  }

  return return_code;
}

void check_for_dirty_configuration()
{

  if (configControl.config_dirty)
  {
    Serial.println("dirty config!");
    save_persistent_configuration();
  }
}

//-------------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------------

void check_paddles()
{
  if (paddleReader != nullptr)
  {
    paddleReader->readPaddles();
  }
#define NO_CLOSURE 0
#define DIT_CLOSURE_DAH_OFF 1
#define DAH_CLOSURE_DIT_OFF 2
#define DIT_CLOSURE_DAH_ON 3
#define DAH_CLOSURE_DIT_ON 4

  if (keyer_machine_mode == BEACON)
  {
    return;
  }

  static byte last_closure = NO_CLOSURE;

  check_dit_paddle();
  check_dah_paddle();

  if (configControl.configuration.keyer_mode == ULTIMATIC)
  {
    if (ultimatic_mode == ULTIMATIC_NORMAL)
    {

      switch (last_closure)
      {
      case DIT_CLOSURE_DAH_OFF:
        if (dah_buffer)
        {
          if (dit_buffer)
          {
            last_closure = DAH_CLOSURE_DIT_ON;
            dit_buffer = 0;
          }
          else
          {
            last_closure = DAH_CLOSURE_DIT_OFF;
          }
        }
        else
        {
          if (!dit_buffer)
          {
            last_closure = NO_CLOSURE;
          }
        }
        break;
      case DIT_CLOSURE_DAH_ON:
        if (dit_buffer)
        {
          if (dah_buffer)
          {
            dah_buffer = 0;
          }
          else
          {
            last_closure = DIT_CLOSURE_DAH_OFF;
          }
        }
        else
        {
          if (dah_buffer)
          {
            last_closure = DAH_CLOSURE_DIT_OFF;
          }
          else
          {
            last_closure = NO_CLOSURE;
          }
        }
        break;

      case DAH_CLOSURE_DIT_OFF:
        if (dit_buffer)
        {
          if (dah_buffer)
          {
            last_closure = DIT_CLOSURE_DAH_ON;
            dah_buffer = 0;
          }
          else
          {
            last_closure = DIT_CLOSURE_DAH_OFF;
          }
        }
        else
        {
          if (!dah_buffer)
          {
            last_closure = NO_CLOSURE;
          }
        }
        break;

      case DAH_CLOSURE_DIT_ON:
        if (dah_buffer)
        {
          if (dit_buffer)
          {
            dit_buffer = 0;
          }
          else
          {
            last_closure = DAH_CLOSURE_DIT_OFF;
          }
        }
        else
        {
          if (dit_buffer)
          {
            last_closure = DIT_CLOSURE_DAH_OFF;
          }
          else
          {
            last_closure = NO_CLOSURE;
          }
        }
        break;

      case NO_CLOSURE:
        if ((dit_buffer) && (!dah_buffer))
        {
          last_closure = DIT_CLOSURE_DAH_OFF;
        }
        else
        {
          if ((dah_buffer) && (!dit_buffer))
          {
            last_closure = DAH_CLOSURE_DIT_OFF;
          }
          else
          {
            if ((dit_buffer) && (dah_buffer))
            {
              // need to handle dit/dah priority here
              last_closure = DIT_CLOSURE_DAH_ON;
              dah_buffer = 0;
            }
          }
        }
        break;
      }
    }
    else
    { // if (ultimatic_mode == ULTIMATIC_NORMAL)
      if ((dit_buffer) && (dah_buffer))
      { // dit or dah priority mode
        if (ultimatic_mode == ULTIMATIC_DIT_PRIORITY)
        {
          dah_buffer = 0;
        }
        else
        {
          dit_buffer = 0;
        }
      }
    } // if (ultimatic_mode == ULTIMATIC_NORMAL)
  }   // if (configControl.configuration.keyer_mode == ULTIMATIC)

  if (configControl.configuration.keyer_mode == SINGLE_PADDLE)
  {
    switch (last_closure)
    {
    case DIT_CLOSURE_DAH_OFF:
      if (dit_buffer)
      {
        if (dah_buffer)
        {
          dah_buffer = 0;
        }
        else
        {
          last_closure = DIT_CLOSURE_DAH_OFF;
        }
      }
      else
      {
        if (dah_buffer)
        {
          last_closure = DAH_CLOSURE_DIT_OFF;
        }
        else
        {
          last_closure = NO_CLOSURE;
        }
      }
      break;

    case DIT_CLOSURE_DAH_ON:

      if (dah_buffer)
      {
        if (dit_buffer)
        {
          last_closure = DAH_CLOSURE_DIT_ON;
          dit_buffer = 0;
        }
        else
        {
          last_closure = DAH_CLOSURE_DIT_OFF;
        }
      }
      else
      {
        if (!dit_buffer)
        {
          last_closure = NO_CLOSURE;
        }
      }
      break;

    case DAH_CLOSURE_DIT_OFF:
      if (dah_buffer)
      {
        if (dit_buffer)
        {
          dit_buffer = 0;
        }
        else
        {
          last_closure = DAH_CLOSURE_DIT_OFF;
        }
      }
      else
      {
        if (dit_buffer)
        {
          last_closure = DIT_CLOSURE_DAH_OFF;
        }
        else
        {
          last_closure = NO_CLOSURE;
        }
      }
      break;

    case DAH_CLOSURE_DIT_ON:
      if (dit_buffer)
      {
        if (dah_buffer)
        {
          last_closure = DIT_CLOSURE_DAH_ON;
          dah_buffer = 0;
        }
        else
        {
          last_closure = DIT_CLOSURE_DAH_OFF;
        }
      }
      else
      {
        if (!dah_buffer)
        {
          last_closure = NO_CLOSURE;
        }
      }
      break;

    case NO_CLOSURE:
      if ((dit_buffer) && (!dah_buffer))
      {
        last_closure = DIT_CLOSURE_DAH_OFF;
      }
      else
      {
        if ((dah_buffer) && (!dit_buffer))
        {
          last_closure = DAH_CLOSURE_DIT_OFF;
        }
        else
        {
          if ((dit_buffer) && (dah_buffer))
          {
            // need to handle dit/dah priority here
            last_closure = DIT_CLOSURE_DAH_ON;
            dah_buffer = 0;
          }
        }
      }
      break;
    }
  }

  service_tx_inhibit_and_pause();
}

//-------------------------------------------------------------------------------------------------------

void ptt_key()
{

  unsigned long ptt_activation_time = millis();
  byte all_delays_satisfied = 0;

  if (ptt_line_activated == 0)
  { // if PTT is currently deactivated, bring it up and insert PTT lead time delay

    if (configControl.configuration.current_ptt_line)
    {

      digitalWrite(configControl.configuration.current_ptt_line, ptt_line_active_state);
    }

    ptt_line_activated = 1;

    while (!all_delays_satisfied)
    {

      if ((millis() - ptt_activation_time) >= configControl.configuration.ptt_lead_time[configControl.configuration.current_tx - 1])
      {
        all_delays_satisfied = 1;
      }

    } //while (!all_delays_satisfied)
  }
  ptt_time = millis();
}
//-------------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------------
void ptt_unkey()
{

  if (ptt_line_activated)
  {

#ifdef FEATURE_SO2R_BASE
    if (current_tx_ptt_line)
    {
      digitalWrite(current_tx_ptt_line, ptt_line_inactive_state);
#else
    if (configControl.configuration.current_ptt_line)
    {
      digitalWrite(configControl.configuration.current_ptt_line, ptt_line_inactive_state);

#endif //FEATURE_SO2R_BASE
    }
    ptt_line_activated = 0;
  }
}

//-------------------------------------------------------------------------------------------------------
void check_ptt_tail()
{

  static byte manual_ptt_invoke_ptt_input_pin = 0;

  if (ptt_input_pin)
  {
    if ((digitalRead(ptt_input_pin) == ptt_input_pin_active_state))
    {
      if (!manual_ptt_invoke)
      {
        manual_ptt_invoke = 1;
        manual_ptt_invoke_ptt_input_pin = 1;
        ptt_key();
        return;
      }
    }
    else
    {
      if ((manual_ptt_invoke) && (manual_ptt_invoke_ptt_input_pin))
      {
        manual_ptt_invoke = 0;
        manual_ptt_invoke_ptt_input_pin = 0;
        if (!key_state)
        {
          ptt_unkey();
        }
      }
    }
  }

#if !defined(FEATURE_WINKEY_EMULATION)
  if (key_state)
  {
    ptt_time = millis();
  }
  else
  {
    if ((ptt_line_activated) && (manual_ptt_invoke == 0))
    {
      //if ((millis() - ptt_time) > ptt_tail_time) {
      if (last_sending_mode == MANUAL_SENDING)
      {
#ifndef OPTION_INCLUDE_PTT_TAIL_FOR_MANUAL_SENDING

        // PTT Tail Time: N     PTT Hang Time: Y

        if ((millis() - ptt_time) >= ((configControl.configuration.length_wordspace * ptt_hang_time_wordspace_units) * float(1200 / configControl.configuration.wpm)))
        {
          ptt_unkey();
        }
#else //ndef OPTION_INCLUDE_PTT_TAIL_FOR_MANUAL_SENDING
#ifndef OPTION_EXCLUDE_PTT_HANG_TIME_FOR_MANUAL_SENDING

        // PTT Tail Time: Y     PTT Hang Time: Y

        if ((millis() - ptt_time) >= (((configControl.configuration.length_wordspace * ptt_hang_time_wordspace_units) * float(1200 / configControl.configuration.wpm)) + configControl.configuration.ptt_tail_time[configControl.configuration.current_tx - 1]))
        {
          ptt_unkey();
        }
#else  //OPTION_EXCLUDE_PTT_HANG_TIME_FOR_MANUAL_SENDING
        if ((millis() - ptt_time) >= configControl.configuration.ptt_tail_time[configControl.configuration.current_tx - 1])
        {

          // PTT Tail Time: Y    PTT Hang Time: N

          ptt_unkey();
        }
#endif //OPTION_EXCLUDE_PTT_HANG_TIME_FOR_MANUAL_SENDING
#endif //ndef OPTION_INCLUDE_PTT_TAIL_FOR_MANUAL_SENDING
      }
      else
      { // automatic sending
        if (((millis() - ptt_time) > configControl.configuration.ptt_tail_time[configControl.configuration.current_tx - 1]) && (!configControl.configuration.ptt_buffer_hold_active || ((!send_buffer_bytes) && configControl.configuration.ptt_buffer_hold_active) || (pause_sending_buffer)))
        {
          ptt_unkey();
        }
      }
    }
  }

#endif //FEATURE_WINKEY_EMULATION
}

//-------------------------------------------------------------------------------------------------------
void save_persistent_configuration()
{
  configControl.save();

  configControl.config_dirty = 0;
}

void check_dit_paddle()
{
  if (paddleReader != nullptr)
  {
    paddleReader->readPaddles();
  }
  //#if !defined M5CORE
  byte pin_value = 0;
  byte dit_paddle = 0;
#ifdef OPTION_DIT_PADDLE_NO_SEND_ON_MEM_RPT
  static byte memory_rpt_interrupt_flag = 0;
#endif

  if (configControl.configuration.paddle_mode == PADDLE_NORMAL)
  {
    dit_paddle = paddle_left;
  }
  else
  {
    dit_paddle = paddle_right;
  }

  // low is closed, high is open
  pin_value = paddle_pin_read(dit_paddle);

#if defined(FEATURE_USB_MOUSE) || defined(FEATURE_USB_KEYBOARD)
  if (usb_dit)
  {
    pin_value = 0;
  }
#endif

#ifdef OPTION_DIT_PADDLE_NO_SEND_ON_MEM_RPT
  if (pin_value && memory_rpt_interrupt_flag)
  {
    memory_rpt_interrupt_flag = 0;
    sending_mode = MANUAL_SENDING;
    loop_element_lengths(3, 0, configControl.configuration.wpm);
    dit_buffer = 0;
  }
#endif

#ifdef OPTION_DIT_PADDLE_NO_SEND_ON_MEM_RPT
  if ((pin_value == 0) && (memory_rpt_interrupt_flag == 0))
  {
#else
  if (pin_value == 0)
  {
#endif

    dit_buffer = 1;

    manual_ptt_invoke = 0;
  }
  //#endif
}

//-------------------------------------------------------------------------------------------------------

void check_dah_paddle()
{
if (paddleReader != nullptr)
  {
    paddleReader->readPaddles();
  }
  byte pin_value = 0;
  byte dah_paddle;

  if (configControl.configuration.paddle_mode == PADDLE_NORMAL)
  {
    dah_paddle = paddle_right;
  }
  else
  {
    dah_paddle = paddle_left;
  }
  //#if !defined M5CORE
  pin_value = paddle_pin_read(dah_paddle);

  if (pin_value == 0)
  {

    dah_buffer = 1;

    manual_ptt_invoke = 0;
  }
  //#endif
}

//-------------------------------------------------------------------------------------------------------

void send_dit()
{
#if !defined M5CORE
  sendEspNowDitDah(ESPNOW_DIT);
#endif
  // notes: key_compensation is a straight x mS lengthening or shortening of the key down time
  //        weighting is

  unsigned int character_wpm = configControl.configuration.wpm;

#ifdef FEATURE_FARNSWORTH
  if ((sending_mode == AUTOMATIC_SENDING) && (configControl.configuration.wpm_farnsworth > configControl.configuration.wpm))
  {
    character_wpm = configControl.configuration.wpm_farnsworth;
#if defined(DEBUG_FARNSWORTH)
    debug_serial_port->println(F("send_dit: farns act"));
#endif
  }
#if defined(DEBUG_FARNSWORTH)

  else
  {
    debug_serial_port->println(F("send_dit: farns inact"));
  }
#endif
#endif //FEATURE_FARNSWORTH

  if (keyer_machine_mode == KEYER_COMMAND_MODE)
  {
    character_wpm = configControl.configuration.wpm_command_mode;
  }

  being_sent = SENDING_DIT;
  tx_and_sidetone_key(1);
#if !defined M5CORE
  if ((tx_key_dit) && (key_tx))
  {
    digitalWrite(tx_key_dit, tx_key_dit_and_dah_pins_active_state);
  }
#endif
#ifdef FEATURE_QLF
  if (qlf_active)
  {
    loop_element_lengths((1.0 * (float(configControl.configuration.weighting) / 50) * (random(qlf_dit_min, qlf_dit_max) / 100.0)), keying_compensation, character_wpm);
  }
  else
  {
    loop_element_lengths((1.0 * (float(configControl.configuration.weighting) / 50)), keying_compensation, character_wpm);
  }
#else  //FEATURE_QLF
  loop_element_lengths((1.0 * (float(configControl.configuration.weighting) / 50)), keying_compensation, character_wpm);
#endif //FEATURE_QLF
#if !defined M5CORE
  if ((tx_key_dit) && (key_tx))
  {
    digitalWrite(tx_key_dit, tx_key_dit_and_dah_pins_inactive_state);
  }
#endif
#ifdef DEBUG_VARIABLE_DUMP
  dit_end_time = millis();
#endif
  tx_and_sidetone_key(0);

  loop_element_lengths((2.0 - (float(configControl.configuration.weighting) / 50)), (-1.0 * keying_compensation), character_wpm);

#ifdef FEATURE_AUTOSPACE

  byte autospace_end_of_character_flag = 0;

  if ((sending_mode == MANUAL_SENDING) && (configControl.configuration.autospace_active))
  {
    check_paddles();
  }
  if ((sending_mode == MANUAL_SENDING) && (configControl.configuration.autospace_active) && (dit_buffer == 0) && (dah_buffer == 0))
  {
    loop_element_lengths(2, 0, configControl.configuration.wpm);
    autospace_end_of_character_flag = 1;
  }
#endif

#ifdef FEATURE_WINKEY_EMULATION
  if ((winkey_host_open) && (winkey_paddle_echo_activated) && (sending_mode == MANUAL_SENDING))
  {
    winkey_paddle_echo_buffer = (winkey_paddle_echo_buffer * 10) + 1;
    //winkey_paddle_echo_buffer_decode_time = millis() + (float(winkey_paddle_echo_buffer_decode_time_factor/float(configControl.configuration.wpm))*length_letterspace);
    winkey_paddle_echo_buffer_decode_time = millis() + (float(winkey_paddle_echo_buffer_decode_timing_factor * 1200.0 / float(configControl.configuration.wpm)) * length_letterspace);

#ifdef FEATURE_AUTOSPACE
    if (autospace_end_of_character_flag)
    {
      winkey_paddle_echo_buffer_decode_time = 0;
    }
#endif //FEATURE_AUTOSPACE
  }
#endif

#ifdef FEATURE_PADDLE_ECHO
  if (sending_mode == MANUAL_SENDING)
  {
    paddle_echo_buffer = (paddle_echo_buffer * 10) + 1;
    paddle_echo_buffer_decode_time = millis() + (float((cw_echo_timing_factor * 1200.0) / configControl.configuration.wpm) * length_letterspace);

#ifdef FEATURE_AUTOSPACE
    if (autospace_end_of_character_flag)
    {
      paddle_echo_buffer_decode_time = 0;
    }
#endif //FEATURE_AUTOSPACE
  }
#endif //FEATURE_PADDLE_ECHO

#ifdef FEATURE_AUTOSPACE
  autospace_end_of_character_flag = 0;
#endif //FEATURE_AUTOSPACE

  being_sent = SENDING_NOTHING;
  last_sending_mode = sending_mode;

  check_paddles();
}

//-------------------------------------------------------------------------------------------------------

void send_dah()
{
#if !defined M5CORE
  sendEspNowDitDah(ESPNOW_DAH);
#endif
  unsigned int character_wpm = configControl.configuration.wpm;

#ifdef FEATURE_FARNSWORTH
  if ((sending_mode == AUTOMATIC_SENDING) && (configControl.configuration.wpm_farnsworth > configControl.configuration.wpm))
  {
    character_wpm = configControl.configuration.wpm_farnsworth;
  }
#endif //FEATURE_FARNSWORTH

  if (keyer_machine_mode == KEYER_COMMAND_MODE)
  {
    character_wpm = configControl.configuration.wpm_command_mode;
  }

  being_sent = SENDING_DAH;
  tx_and_sidetone_key(1);
#ifdef DEBUG_VARIABLE_DUMP
  dah_start_time = millis();
#endif
#if !defined M5CORE
  if ((tx_key_dah) && (key_tx))
  {
    digitalWrite(tx_key_dah, tx_key_dit_and_dah_pins_active_state);
  }
#endif
#ifdef FEATURE_QLF
  if (qlf_active)
  {
    loop_element_lengths((float(configControl.configuration.dah_to_dit_ratio / 100.0) * (float(configControl.configuration.weighting) / 50) * (random(qlf_dah_min, qlf_dah_max) / 100.0)), keying_compensation, character_wpm);
  }
  else
  {
    loop_element_lengths((float(configControl.configuration.dah_to_dit_ratio / 100.0) * (float(configControl.configuration.weighting) / 50)), keying_compensation, character_wpm);
  }
#else  //FEATURE_QLF
  loop_element_lengths((float(configControl.configuration.dah_to_dit_ratio / 100.0) * (float(configControl.configuration.weighting) / 50)), keying_compensation, character_wpm);
#endif //FEATURE_QLF
#if !defined M5CORE
  if ((tx_key_dah) && (key_tx))
  {
    digitalWrite(tx_key_dah, tx_key_dit_and_dah_pins_inactive_state);
  }
#endif
#ifdef DEBUG_VARIABLE_DUMP
  dah_end_time = millis();
#endif

  tx_and_sidetone_key(0);

  loop_element_lengths((4.0 - (3.0 * (float(configControl.configuration.weighting) / 50))), (-1.0 * keying_compensation), character_wpm);

#ifdef FEATURE_AUTOSPACE

  byte autospace_end_of_character_flag = 0;

  if ((sending_mode == MANUAL_SENDING) && (configControl.configuration.autospace_active))
  {
    check_paddles();
  }
  if ((sending_mode == MANUAL_SENDING) && (configControl.configuration.autospace_active) && (dit_buffer == 0) && (dah_buffer == 0))
  {
    loop_element_lengths(2, 0, configControl.configuration.wpm);
    autospace_end_of_character_flag = 1;
  }
#endif

#ifdef FEATURE_WINKEY_EMULATION
  if ((winkey_host_open) && (winkey_paddle_echo_activated) && (sending_mode == MANUAL_SENDING))
  {
    winkey_paddle_echo_buffer = (winkey_paddle_echo_buffer * 10) + 2;
    //winkey_paddle_echo_buffer_decode_time = millis() + (float(winkey_paddle_echo_buffer_decode_time_factor/float(configControl.configuration.wpm))*length_letterspace);
    winkey_paddle_echo_buffer_decode_time = millis() + (float(winkey_paddle_echo_buffer_decode_timing_factor * 1200.0 / float(configControl.configuration.wpm)) * length_letterspace);
#ifdef FEATURE_AUTOSPACE
    if (autospace_end_of_character_flag)
    {
      winkey_paddle_echo_buffer_decode_time = 0;
    }
#endif //FEATURE_AUTOSPACE
  }
#endif

#ifdef FEATURE_PADDLE_ECHO
  if (sending_mode == MANUAL_SENDING)
  {
    paddle_echo_buffer = (paddle_echo_buffer * 10) + 2;
    paddle_echo_buffer_decode_time = millis() + (float((cw_echo_timing_factor * 1200.0) / configControl.configuration.wpm) * length_letterspace);

#ifdef FEATURE_AUTOSPACE
    if (autospace_end_of_character_flag)
    {
      paddle_echo_buffer_decode_time = 0;
    }
#endif //FEATURE_AUTOSPACE
  }
#endif //FEATURE_PADDLE_ECHO

#ifdef FEATURE_AUTOSPACE
  autospace_end_of_character_flag = 0;
#endif //FEATURE_AUTOSPACE

  check_paddles();

  being_sent = SENDING_NOTHING;
  last_sending_mode = sending_mode;
}

//-------------------------------------------------------------------------------------------------------

void tx_and_sidetone_key(int state)
{

#if !defined(FEATURE_PTT_INTERLOCK)
  if ((state) && (key_state == 0))
  {
    if (key_tx)
    {
      byte previous_ptt_line_activated = ptt_line_activated;
      ptt_key();
#if !defined M5CORE
      if (current_tx_key_line)
      {
        digitalWrite(current_tx_key_line, tx_key_line_active_state);
      }
#endif
#if defined(OPTION_WINKEY_2_SUPPORT) && defined(FEATURE_WINKEY_EMULATION) && !defined(FEATURE_SO2R_BASE)
      if ((wk2_both_tx_activated) && (tx_key_line_2))
      {
        digitalWrite(tx_key_line_2, HIGH);
      }
#endif
      if ((first_extension_time) && (previous_ptt_line_activated == 0))
      {
        delay(first_extension_time);
      }
    }
    if ((configControl.configuration.sidetone_mode == SIDETONE_ON) || (keyer_machine_mode == KEYER_COMMAND_MODE) || ((configControl.configuration.sidetone_mode == SIDETONE_PADDLE_ONLY) && (sending_mode == MANUAL_SENDING)))
    {
#if !defined(OPTION_SIDETONE_DIGITAL_OUTPUT_NO_SQUARE_WAVE)
      //Serial.println("About send tone #1");
      toneControl.tone(configControl.configuration.hz_sidetone);
#else
      if (sidetone_line)
      {
        digitalWrite(sidetone_line, HIGH);
      }
#endif
    }
    key_state = 1;
  }
  else
  {
    if ((state == 0) && (key_state))
    {
      if (key_tx)
      {
#if !defined M5CORE
        if (current_tx_key_line)
        {
          digitalWrite(current_tx_key_line, tx_key_line_inactive_state);
        }
#endif
#if defined(OPTION_WINKEY_2_SUPPORT) && defined(FEATURE_WINKEY_EMULATION) && !defined(FEATURE_SO2R_BASE)
        if ((wk2_both_tx_activated) && (tx_key_line_2))
        {
          digitalWrite(tx_key_line_2, LOW);
        }
#endif
        ptt_key();
      }
      if ((configControl.configuration.sidetone_mode == SIDETONE_ON) || (keyer_machine_mode == KEYER_COMMAND_MODE) || ((configControl.configuration.sidetone_mode == SIDETONE_PADDLE_ONLY) && (sending_mode == MANUAL_SENDING)))
      {
#if !defined(OPTION_SIDETONE_DIGITAL_OUTPUT_NO_SQUARE_WAVE)
        toneControl.noTone();
#else
        if (sidetone_line)
        {
          digitalWrite(sidetone_line, LOW);
        }
#endif
      }
      key_state = 0;
    }
  }

#endif //FEATURE_PTT_INTERLOCK

#if defined(FEATURE_INTERNET_LINK)
  link_key(state);
#endif

  check_ptt_tail();
}

//-------------------------------------------------------------------------------------------------------

void loop_element_lengths(float lengths, float additional_time_ms, int speed_wpm_in)
{

  float element_length;

  if (lengths <= 0)
  {
    return;
  }

#if defined(FEATURE_FARNSWORTH)

  if ((lengths == 1) && (speed_wpm_in == 0))
  {
    element_length = additional_time_ms;
  }
  else
  {
    if (speed_mode == SPEED_NORMAL)
    {
      element_length = 1200 / speed_wpm_in;
    }
    else
    {
      element_length = qrss_dit_length * 1000;
    }
  }

#else  //FEATURE_FARNSWORTH
  if (speed_mode == SPEED_NORMAL)
  {
    element_length = 1200 / speed_wpm_in;
  }
  else
  {
    element_length = qrss_dit_length * 1000;
  }
#endif //FEATURE_FARNSWORTH

  unsigned long ticks = long(element_length * lengths * 1000) + long(additional_time_ms * 1000); // improvement from Paul, K1XM
  unsigned long start = micros();

#if defined(FEATURE_SERIAL) && !defined(OPTION_DISABLE_SERIAL_PORT_CHECKING_WHILE_SENDING_CW)
  while (((micros() - start) < ticks) && (service_tx_inhibit_and_pause() == 0) && loop_element_lengths_breakout_flag)
  {
#else
  while (((micros() - start) < ticks) && (service_tx_inhibit_and_pause() == 0))
  {
#endif

    check_ptt_tail();

#if defined(FEATURE_SERIAL) && !defined(OPTION_DISABLE_SERIAL_PORT_CHECKING_WHILE_SENDING_CW)
    if (((ticks - (micros() - start)) > (10 * 1000)) && (sending_mode == AUTOMATIC_SENDING))
    {
      check_serial();
      if (loop_element_lengths_breakout_flag == 0)
      {
        dump_current_character_flag = 1;
      }
    }
#endif //FEATURE_SERIAL

    if ((ticks - (micros() - start)) > (10 * 1000))
    {
      displayControl.service_display();
    }

    if ((configControl.configuration.keyer_mode != ULTIMATIC) && (configControl.configuration.keyer_mode != SINGLE_PADDLE))
    {
      if ((configControl.configuration.keyer_mode == IAMBIC_A)
#if !defined M5CORE
          &&
          (paddle_pin_read(paddle_left) == LOW) && (paddle_pin_read(paddle_right) == LOW)
#endif
      )
      {
        iambic_flag = 1;
      }

#ifndef FEATURE_CMOS_SUPER_KEYER_IAMBIC_B_TIMING
      if (being_sent == SENDING_DIT)
      {
        check_dah_paddle();
      }
      else
      {
        if (being_sent == SENDING_DAH)
        {
          check_dit_paddle();
        }
        else
        {
          check_dah_paddle();
          check_dit_paddle();
        }
      }
#else  ////FEATURE_CMOS_SUPER_KEYER_IAMBIC_B_TIMING
      if (configControl.configuration.cmos_super_keyer_iambic_b_timing_on)
      {
        if ((float(float(micros() - start) / float(ticks)) * 100) >= configControl.configuration.cmos_super_keyer_iambic_b_timing_percent)
        {
          //if ((float(float(millis()-starttime)/float(starttime-ticks))*100) >= configControl.configuration.cmos_super_keyer_iambic_b_timing_percent) {
          if (being_sent == SENDING_DIT)
          {
            check_dah_paddle();
          }
          else
          {
            if (being_sent == SENDING_DAH)
            {
              check_dit_paddle();
            }
          }
        }
        else
        {
          if (((being_sent == SENDING_DIT) || (being_sent == SENDING_DAH)) && (paddle_pin_read(paddle_left) == LOW) && (paddle_pin_read(paddle_right) == LOW))
          {
            dah_buffer = 0;
            dit_buffer = 0;
          }
        }
      }
      else
      {
        if (being_sent == SENDING_DIT)
        {
          check_dah_paddle();
        }
        else
        {
          if (being_sent == SENDING_DAH)
          {
            check_dit_paddle();
          }
          else
          {
            check_dah_paddle();
            check_dit_paddle();
          }
        }
      }
#endif //FEATURE_CMOS_SUPER_KEYER_IAMBIC_B_TIMING
    }
    else
    { //(configControl.configuration.keyer_mode != ULTIMATIC)

      if (being_sent == SENDING_DIT)
      {
        check_dah_paddle();
      }
      else
      {
        if (being_sent == SENDING_DAH)
        {
          check_dit_paddle();
        }
        else
        {
          check_dah_paddle();
          check_dit_paddle();
        }
      }
    }

// blow out prematurely if we're automatic sending and a paddle gets hit
#ifdef FEATURE_COMMAND_BUTTONS
    if (sending_mode == AUTOMATIC_SENDING && (paddle_pin_read(paddle_left) == LOW || paddle_pin_read(paddle_right) == LOW || analogbuttonread(0) || dit_buffer || dah_buffer))
    {
      if (keyer_machine_mode == KEYER_NORMAL)
      {
        sending_mode = AUTOMATIC_SENDING_INTERRUPTED;
        automatic_sending_interruption_time = millis();
        return;
      }
    }
#else
    if (sending_mode == AUTOMATIC_SENDING && (
#if !defined M5CORE
                                                 paddle_pin_read(paddle_left) == LOW || paddle_pin_read(paddle_right) == LOW ||
#endif
                                                 dit_buffer || dah_buffer))
    {
      if (keyer_machine_mode == KEYER_NORMAL)
      {
        sending_mode = AUTOMATIC_SENDING_INTERRUPTED;
        automatic_sending_interruption_time = millis();

        return;
      }
    }
#endif

  } //while ((millis() < endtime) && (millis() > 200))

  if ((configControl.configuration.keyer_mode == IAMBIC_A) && (iambic_flag)
#if !defined M5CORE
      && (paddle_pin_read(paddle_left) == HIGH) && (paddle_pin_read(paddle_right) == HIGH)
#endif
  )
  {
    iambic_flag = 0;
    dit_buffer = 0;
    dah_buffer = 0;
  }

  if ((being_sent == SENDING_DIT) || (being_sent == SENDING_DAH))
  {
    if (configControl.configuration.dit_buffer_off)
    {
      dit_buffer = 0;
    }
    if (configControl.configuration.dah_buffer_off)
    {
      dah_buffer = 0;
    }
  }

} //void loop_element_lengths

void speed_change(int change)
{
  if (((configControl.configuration.wpm + change) > wpm_limit_low) && ((configControl.configuration.wpm + change) < wpm_limit_high))
  {
    speed_set(configControl.configuration.wpm + change);
  }

  lcd_center_print_timed_wpm();
}

//-------------------------------------------------------------------------------------------------------

void speed_change_command_mode(int change)
{
  if (((configControl.configuration.wpm_command_mode + change) > wpm_limit_low) && ((configControl.configuration.wpm_command_mode + change) < wpm_limit_high))
  {
    configControl.configuration.wpm_command_mode = configControl.configuration.wpm_command_mode + change;
    configControl.config_dirty = 1;
  }

  displayControl.lcd_center_print_timed(String(configControl.configuration.wpm_command_mode) + " wpm", 0, default_display_msg_delay);
}

//-------------------------------------------------------------------------------------------------------

long get_cw_input_from_user(unsigned int exit_time_milliseconds)
{

  byte looping = 1;
  byte paddle_hit = 0;
  long cw_char = 0;
  unsigned long last_element_time = 0;
  byte button_hit = 0;
  unsigned long entry_time = millis();

  while (looping)
  {

#ifdef OPTION_WATCHDOG_TIMER
    wdt_reset();
#endif //OPTION_WATCHDOG_TIMER

#ifdef FEATURE_POTENTIOMETER
    if (configControl.configuration.pot_activated)
    {
#if !defined M5CORE
      wpmPot.checkPotentiometer(wpmSetCallBack);
//check_potentiometer();
#endif
    }
#endif

    check_paddles();

    if (dit_buffer)
    {
      sending_mode = MANUAL_SENDING;
      send_dit();
      dit_buffer = 0;
      paddle_hit = 1;
      cw_char = (cw_char * 10) + 1;
      last_element_time = millis();
    }
    if (dah_buffer)
    {
      sending_mode = MANUAL_SENDING;
      send_dah();
      dah_buffer = 0;
      paddle_hit = 1;
      cw_char = (cw_char * 10) + 2;
      last_element_time = millis();
    }
    if ((paddle_hit) && (millis() > (last_element_time + (float(600 / configControl.configuration.wpm) * length_letterspace))))
    {
#ifdef DEBUG_GET_CW_INPUT_FROM_USER
      debug_serial_port->println(F("get_cw_input_from_user: hit length_letterspace"));
#endif
      looping = 0;
    }

    if ((!paddle_hit) && (exit_time_milliseconds) && ((millis() - entry_time) > exit_time_milliseconds))
    { // if we were passed an exit time and no paddle was hit, blow out of here
      return 0;
    }

  } //while (looping)

  if (button_hit)
  {
#ifdef DEBUG_GET_CW_INPUT_FROM_USER
    debug_serial_port->println(F("get_cw_input_from_user: button_hit exit 9"));
#endif
    return 9;
  }
  else
  {
#ifdef DEBUG_GET_CW_INPUT_FROM_USER
    debug_serial_port->print(F("get_cw_input_from_user: exiting cw_char:"));
    debug_serial_port->println(cw_char);
#endif
    return cw_char;
  }
}

void adjust_dah_to_dit_ratio(int adjustment)
{

  if ((configControl.configuration.dah_to_dit_ratio + adjustment) > 150 && (configControl.configuration.dah_to_dit_ratio + adjustment) < 810)
  {
    configControl.configuration.dah_to_dit_ratio = configControl.configuration.dah_to_dit_ratio + adjustment;
#ifdef FEATURE_DISPLAY
#ifdef OPTION_MORE_DISPLAY_MSGS
    if (LCD_COLUMNS < 9)
    {
      displayControl.lcd_center_print_timed("DDR:" + String(configControl.configuration.dah_to_dit_ratio), 0, default_display_msg_delay);
    }
    else
    {
      displayControl.lcd_center_print_timed("Dah/Dit: " + String(configControl.configuration.dah_to_dit_ratio), 0, default_display_msg_delay);
    }
    service_display();
#endif
#endif
  }

  configControl.config_dirty = 1;
}

//-------------------------------------------------------------------------------------------------------

void sidetone_adj(int hz)
{

  if (((configControl.configuration.hz_sidetone + hz) > sidetone_hz_limit_low) && ((configControl.configuration.hz_sidetone + hz) < sidetone_hz_limit_high))
  {
    configControl.configuration.hz_sidetone = configControl.configuration.hz_sidetone + hz;
    configControl.config_dirty = 1;
#if defined(OPTION_MORE_DISPLAY_MSGS)
    if (LCD_COLUMNS < 9)
    {
      displayControl.lcd_center_print_timed(String(configControl.configuration.hz_sidetone) + " Hz", 0, default_display_msg_delay);
    }
    else
    {
      displayControl.lcd_center_print_timed("Sidetone " + String(configControl.configuration.hz_sidetone) + " Hz", 0, default_display_msg_delay);
    }
#endif
  }
}

//-------------------------------------------------------------------------------------------------------

//------------------------------------------------------------------

void switch_to_tx_silent(byte tx)
{
#if !defined M5CORE
  switch (tx)
  {
  case 1:
    if ((ptt_tx_1) || (tx_key_line_1))
    {
      configControl.configuration.current_ptt_line = ptt_tx_1;
      current_tx_key_line = tx_key_line_1;
      configControl.configuration.current_tx = 1;
      configControl.config_dirty = 1;
    }
    break;
  case 2:
    if ((ptt_tx_2) || (tx_key_line_2))
    {
      configControl.configuration.current_ptt_line = ptt_tx_2;
      current_tx_key_line = tx_key_line_2;
      configControl.configuration.current_tx = 2;
      configControl.config_dirty = 1;
    }
    break;
  case 3:
    if ((ptt_tx_3) || (tx_key_line_3))
    {
      configControl.configuration.current_ptt_line = ptt_tx_3;
      current_tx_key_line = tx_key_line_3;
      configControl.configuration.current_tx = 3;
      configControl.config_dirty = 1;
    }
    break;
  case 4:
    if ((ptt_tx_4) || (tx_key_line_4))
    {
      configControl.configuration.current_ptt_line = ptt_tx_4;
      current_tx_key_line = tx_key_line_4;
      configControl.configuration.current_tx = 4;
      configControl.config_dirty = 1;
    }
    break;
  case 5:
    if ((ptt_tx_5) || (tx_key_line_5))
    {
      configControl.configuration.current_ptt_line = ptt_tx_5;
      current_tx_key_line = tx_key_line_5;
      configControl.configuration.current_tx = 5;
      configControl.config_dirty = 1;
    }
    break;
  case 6:
    if ((ptt_tx_6) || (tx_key_line_6))
    {
      configControl.configuration.current_ptt_line = ptt_tx_6;
      current_tx_key_line = tx_key_line_6;
      configControl.configuration.current_tx = 6;
      configControl.config_dirty = 1;
    }
    break;
  }
#endif
}

//------------------------------------------------------------------
void switch_to_tx(byte tx)
{
#if !defined M5CORE
#ifdef FEATURE_MEMORIES
  repeat_memory = 255;
#endif

#ifdef FEATURE_DISPLAY
  switch (tx)
  {
  case 1:
    if ((ptt_tx_1) || (tx_key_line_1))
    {
      switch_to_tx_silent(1);
      displayControl.lcd_center_print_timed("TX 1", 0, default_display_msg_delay);
    }
    break;
  case 2:
    if ((ptt_tx_2) || (tx_key_line_2))
    {
      switch_to_tx_silent(2);
      displayControl.lcd_center_print_timed("TX 2", 0, default_display_msg_delay);
    }
    break;
  case 3:
    if ((ptt_tx_3) || (tx_key_line_3))
    {
      switch_to_tx_silent(3);
      displayControl.lcd_center_print_timed("TX 3", 0, default_display_msg_delay);
    }
    break;
  case 4:
    if ((ptt_tx_4) || (tx_key_line_4))
    {
      switch_to_tx_silent(4);
      displayControl.lcd_center_print_timed("TX 4", 0, default_display_msg_delay);
    }
    break;
  case 5:
    if ((ptt_tx_5) || (tx_key_line_5))
    {
      switch_to_tx_silent(5);
      displayControl.lcd_center_print_timed("TX 5", 0, default_display_msg_delay);
    }
    break;
  case 6:
    if ((ptt_tx_6) || (tx_key_line_6))
    {
      switch_to_tx_silent(6);
      displayControl.lcd_center_print_timed("TX 6", 0, default_display_msg_delay);
    }
    break;
  }
#else
  switch (tx)
  {
  case 1:
    if ((ptt_tx_1) || (tx_key_line_1))
    {
      switch_to_tx_silent(1);
      send_tx();
      send_char('1', KEYER_NORMAL);
    }
    break;
  case 2:
    if ((ptt_tx_2) || (tx_key_line_2))
    {
      switch_to_tx_silent(2);
      send_tx();
      send_char('2', KEYER_NORMAL);
    }
    break;
  case 3:
    if ((ptt_tx_3) || (tx_key_line_3))
    {
      switch_to_tx_silent(3);
      send_tx();
      send_char('3', KEYER_NORMAL);
    }
    break;
  case 4:
    if ((ptt_tx_4) || (tx_key_line_4))
    {
      switch_to_tx_silent(4);
      send_tx();
      send_char('4', KEYER_NORMAL);
    }
    break;
  case 5:
    if ((ptt_tx_5) || (tx_key_line_5))
    {
      switch_to_tx_silent(5);
      send_tx();
      send_char('5', KEYER_NORMAL);
    }
    break;
  case 6:
    if ((ptt_tx_6) || (tx_key_line_6))
    {
      switch_to_tx_silent(6);
      send_tx();
      send_char('6', KEYER_NORMAL);
    }
    break;
  }
#endif
#endif
}

//------------------------------------------------------------------

//------------------------------------------------------------------

void service_dit_dah_buffers()
{
#ifdef DEBUG_LOOP
  debug_serial_port->println(F("loop: entering service_dit_dah_buffers"));
#endif

  if (keyer_machine_mode == BEACON)
  {
    return;
  }

  if (automatic_sending_interruption_time != 0)
  {
    if ((millis() - automatic_sending_interruption_time) > (configControl.configuration.paddle_interruption_quiet_time_element_lengths * (1200 / configControl.configuration.wpm)))
    {
      automatic_sending_interruption_time = 0;
      sending_mode = MANUAL_SENDING;
    }
    else
    {
      dit_buffer = 0;
      dah_buffer = 0;
      return;
    }
  }

  static byte bug_dah_flag = 0;

#ifdef FEATURE_PADDLE_ECHO
  static unsigned long bug_dah_key_down_time = 0;
#endif //FEATURE_PADDLE_ECHO

  if ((configControl.configuration.keyer_mode == IAMBIC_A) || (configControl.configuration.keyer_mode == IAMBIC_B) || (configControl.configuration.keyer_mode == ULTIMATIC) || (configControl.configuration.keyer_mode == SINGLE_PADDLE))
  {
    if ((configControl.configuration.keyer_mode == IAMBIC_A) && (iambic_flag)
        //#if !defined M5CORE
        && (paddle_pin_read(paddle_left)) && (paddle_pin_read(paddle_right)
        
        && [](){
          if (paddleReader!=nullptr)
          {
            paddleReader->readPaddles();
          }
          return true;
        })
        //#endif
    )
    {
      iambic_flag = 0;
      dit_buffer = 0;
      dah_buffer = 0;
    }
    else
    {
      if (dit_buffer)
      {
        dit_buffer = 0;
        sending_mode = MANUAL_SENDING;
        //sendEspNowDitDah(ESPNOW_DIT);
        send_dit();
      }
      if (dah_buffer)
      {
        dah_buffer = 0;
        sending_mode = MANUAL_SENDING;
        //sendEspNowDitDah(ESPNOW_DAH);
        send_dah();
      }
    }
  }
  else
  {
    if (configControl.configuration.keyer_mode == BUG)
    {
      if (dit_buffer)
      {
        dit_buffer = 0;
        sending_mode = MANUAL_SENDING;
        send_dit();
      }

      if (dah_buffer)
      {
        dah_buffer = 0;
        if (!bug_dah_flag)
        {
          sending_mode = MANUAL_SENDING;
          tx_and_sidetone_key(1);
          bug_dah_flag = 1;
#ifdef FEATURE_PADDLE_ECHO
          bug_dah_key_down_time = millis();
#endif //FEATURE_PADDLE_ECHO
        }

#ifdef FEATURE_PADDLE_ECHO

        //zzzzzz

        //paddle_echo_buffer_decode_time = millis() + (float((cw_echo_timing_factor*3000.0)/configControl.configuration.wpm)*length_letterspace);
#endif //FEATURE_PADDLE_ECHO
      }
      else
      {
        if (bug_dah_flag)
        {
          sending_mode = MANUAL_SENDING;
          tx_and_sidetone_key(0);
#ifdef FEATURE_PADDLE_ECHO
          if ((millis() - bug_dah_key_down_time) > (0.5 * (1200.0 / configControl.configuration.wpm)))
          {
            if ((millis() - bug_dah_key_down_time) > (2 * (1200.0 / configControl.configuration.wpm)))
            {
              paddle_echo_buffer = (paddle_echo_buffer * 10) + 2;
            }
            else
            {
              paddle_echo_buffer = (paddle_echo_buffer * 10) + 1;
            }
            paddle_echo_buffer_decode_time = millis() + (float((cw_echo_timing_factor * 3000.0) / configControl.configuration.wpm) * length_letterspace);
          }
#endif //FEATURE_PADDLE_ECHO
          bug_dah_flag = 0;
        }
      }
#ifdef FEATURE_DEAD_OP_WATCHDOG
      dah_counter = 0;
#endif
    }
    else
    {
      if (configControl.configuration.keyer_mode == STRAIGHT)
      {
        if (dit_buffer)
        {
          dit_buffer = 0;
          sending_mode = MANUAL_SENDING;
          tx_and_sidetone_key(1);
        }
        else
        {
          sending_mode = MANUAL_SENDING;
          tx_and_sidetone_key(0);
        }
#ifdef FEATURE_DEAD_OP_WATCHDOG
        dit_counter = 0;
#endif
      }
    }
  }
}

//-------------------------------------------------------------------------------------------------------

void beep()
{
#if !defined(OPTION_SIDETONE_DIGITAL_OUTPUT_NO_SQUARE_WAVE)
  // #if defined(FEATURE_SINEWAVE_SIDETONE)
  //   tone(sidetone_line, hz_high_beep);
  //   delay(200);
  //   noTone(sidetone_line);
  // #else
  Serial.println("About send tone #2");
  toneControl.tone(hz_high_beep, 200);
  // #endif
#else
  if (sidetone_line)
  {
    digitalWrite(sidetone_line, HIGH);
    delay(200);
    digitalWrite(sidetone_line, LOW);
  }
#endif
}

//-------------------------------------------------------------------------------------------------------

void boop()
{
#if !defined(OPTION_SIDETONE_DIGITAL_OUTPUT_NO_SQUARE_WAVE)
  Serial.println("About send tone #3");
  toneControl.tone(hz_low_beep);
  delay(100);
  toneControl.noTone();
#else
  if (sidetone_line)
  {
    digitalWrite(sidetone_line, HIGH);
    delay(100);
    digitalWrite(sidetone_line, LOW);
  }
#endif
}

//-------------------------------------------------------------------------------------------------------

void beep_boop()
{
#if !defined(OPTION_SIDETONE_DIGITAL_OUTPUT_NO_SQUARE_WAVE)
  Serial.println("About send tone #4");
  toneControl.tone(hz_high_beep);
  delay(100);
  Serial.println("About send tone #5");
  toneControl.tone(hz_low_beep);
  delay(100);
  toneControl.noTone();
#else
  if (sidetone_line)
  {
    digitalWrite(sidetone_line, HIGH);
    delay(200);
    digitalWrite(sidetone_line, LOW);
  }
#endif
}

//-------------------------------------------------------------------------------------------------------

void boop_beep()
{
#if !defined(OPTION_SIDETONE_DIGITAL_OUTPUT_NO_SQUARE_WAVE)
  Serial.println("About send tone #6");
  toneControl.tone(hz_low_beep);
  delay(100);
  Serial.println("About send tone #7");
  toneControl.tone(hz_high_beep);
  delay(100);
  toneControl.noTone();
#else
  if (sidetone_line)
  {
    digitalWrite(sidetone_line, HIGH);
    delay(200);
    digitalWrite(sidetone_line, LOW);
  }
#endif
}

//-------------------------------------------------------------------------------------------------------
void send_the_dits_and_dahs(char const *cw_to_send)
{

  /* American Morse - Special Symbols

    ~  long dah (4 units)

    =  very long dah (5 units)

    &  an extra space (1 unit)

  */

  //debug_serial_port->println(F("send_the_dits_and_dahs()"));

  sending_mode = AUTOMATIC_SENDING;

#if defined(FEATURE_SERIAL) && !defined(OPTION_DISABLE_SERIAL_PORT_CHECKING_WHILE_SENDING_CW)
  dump_current_character_flag = 0;
#endif

#if defined(FEATURE_FARNSWORTH)
  float additional_intercharacter_time_ms;
#endif

  for (int x = 0; x < 12; x++)
  {
    switch (cw_to_send[x])
    {
    case '.':
      //Serial.println("About to send dit...");
      send_dit();
      break;
    case '-':
      send_dah();
      break;
#if defined(FEATURE_AMERICAN_MORSE) // this is a bit of a hack, but who cares!  :-)
    case '~':

      being_sent = SENDING_DAH;
      tx_and_sidetone_key(1);
      if ((tx_key_dah) && (key_tx))
      {
        digitalWrite(tx_key_dah, tx_key_dit_and_dah_pins_active_state);
      }
      loop_element_lengths((float(4.0) * (float(configControl.configuration.weighting) / 50)), keying_compensation, configControl.configuration.wpm);
      if ((tx_key_dah) && (key_tx))
      {
        digitalWrite(tx_key_dah, tx_key_dit_and_dah_pins_inactive_state);
      }
      tx_and_sidetone_key(0);
      loop_element_lengths((4.0 - (3.0 * (float(configControl.configuration.weighting) / 50))), (-1.0 * keying_compensation), configControl.configuration.wpm);
      break;

    case '=':
      being_sent = SENDING_DAH;
      tx_and_sidetone_key(1);
      if ((tx_key_dah) && (key_tx))
      {
        digitalWrite(tx_key_dah, tx_key_dit_and_dah_pins_active_state);
      }
      loop_element_lengths((float(5.0) * (float(configControl.configuration.weighting) / 50)), keying_compensation, configControl.configuration.wpm);
      if ((tx_key_dah) && (key_tx))
      {
        digitalWrite(tx_key_dah, tx_key_dit_and_dah_pins_inactive_state);
      }
      tx_and_sidetone_key(0);
      loop_element_lengths((4.0 - (3.0 * (float(configControl.configuration.weighting) / 50))), (-1.0 * keying_compensation), configControl.configuration.wpm);
      break;

    case '&':
      loop_element_lengths((4.0 - (3.0 * (float(configControl.configuration.weighting) / 50))), (-1.0 * keying_compensation), configControl.configuration.wpm);
      break;
#endif //FEATURE_AMERICAN_MORSE
    default:
      //return;
      x = 12;
      break;
    }

    if ((dit_buffer || dah_buffer || sending_mode == AUTOMATIC_SENDING_INTERRUPTED) && (keyer_machine_mode != BEACON))
    {
      dit_buffer = 0;
      dah_buffer = 0;
      //debug_serial_port->println(F("send_the_dits_and_dahs: AUTOMATIC_SENDING_INTERRUPTED"));
      //return;
      x = 12;
    }
#if defined(FEATURE_SERIAL)
    check_serial();
#if !defined(OPTION_DISABLE_SERIAL_PORT_CHECKING_WHILE_SENDING_CW)
    if (dump_current_character_flag)
    {
      x = 12;
    }
#endif
#endif

#ifdef OPTION_WATCHDOG_TIMER
    wdt_reset();
#endif //OPTION_WATCHDOG_TIMER

  } // for (int x = 0;x < 12;x++)
}

//-------------------------------------------------------------------------------------------------------

void send_char(byte cw_char, byte omit_letterspace)
{

  if ((cw_char == 10) || (cw_char == 13))
  {
    return;
  } // don't attempt to send carriage return or line feed

  sending_mode = AUTOMATIC_SENDING;

  if (char_send_mode == CW)
  {
    bool cwHandled = false;

    //special cases
    switch (cw_char)
    {
    case '\n':
      cwHandled = true;
      break;
    case '\r':
      cwHandled = true;
      break;
    case ' ':
      loop_element_lengths((configControl.configuration.length_wordspace - length_letterspace - 2), 0, configControl.configuration.wpm);
      cwHandled = true;
      break;
    case '|':
#if !defined(OPTION_WINKEY_DO_NOT_SEND_7C_BYTE_HALF_SPACE)
      loop_element_lengths(0.5, 0, configControl.configuration.wpm);
#endif
      cwHandled = true;
      break;
    default:
      cwHandled = false;
      break;
    }

    //most cases
    if (!cwHandled)
    {
      //get the dits and dahs
      String ditsanddahs = "" + convertCharToDitsAndDahs(cw_char);
      if (ditsanddahs.length() > 0)
      {
        send_the_dits_and_dahs(ditsanddahs.c_str());
      }
    }

    if (omit_letterspace != OMIT_LETTERSPACE)
    {

      loop_element_lengths((length_letterspace - 1), 0, configControl.configuration.wpm); //this is minus one because send_dit and send_dah have a trailing element space
    }

#ifdef FEATURE_FARNSWORTH
    // Farnsworth Timing : http://www.arrl.org/files/file/Technology/x9004008.pdf
    if (configControl.configuration.wpm_farnsworth > configControl.configuration.wpm)
    {
      float additional_intercharacter_time_ms = ((((1.0 * farnsworth_timing_calibration) * ((60.0 * float(configControl.configuration.wpm_farnsworth)) - (37.2 * float(configControl.configuration.wpm))) / (float(configControl.configuration.wpm) * float(configControl.configuration.wpm_farnsworth))) / 19.0) * 1000.0) - (1200.0 / float(configControl.configuration.wpm_farnsworth));
      loop_element_lengths(1, additional_intercharacter_time_ms, 0);
    }
#endif
  }
}

//-------------------------------------------------------------------------------------------------------

void service_send_buffer(byte no_print)
{
  // send one character out of the send buffer

#ifdef DEBUG_LOOP
  debug_serial_port->println(F("loop: entering service_send_buffer"));
#endif

  static unsigned long timed_command_end_time;
  static byte timed_command_in_progress = 0;

#if defined(DEBUG_SERVICE_SEND_BUFFER)

  byte no_bytes_flag = 0;

  if (send_buffer_bytes > 0)
  {
    debug_serial_port->print("service_send_buffer: enter:");
    for (int x = 0; x < send_buffer_bytes; x++)
    {
      debug_serial_port->write(send_buffer_array[x]);
      debug_serial_port->print("[");
      debug_serial_port->print(send_buffer_array[x]);
      debug_serial_port->print("]");
    }
    debug_serial_port->println();
  }
  else
  {
    no_bytes_flag = 1;
  }
  Serial.flush();
#endif

  if (service_tx_inhibit_and_pause() == 1)
  {
#if defined(DEBUG_SERVICE_SEND_BUFFER)
    debug_serial_port->println("service_send_buffer: tx_inhib");
#endif
    return;
  }

#ifdef FEATURE_MEMORIES
  play_memory_prempt = 0;
#endif

  if (send_buffer_status == SERIAL_SEND_BUFFER_NORMAL)
  {
    if ((send_buffer_bytes) && (pause_sending_buffer == 0))
    {
#ifdef FEATURE_SLEEP
      last_activity_time = millis();
#endif //FEATURE_SLEEP

      if ((send_buffer_array[0] > SERIAL_SEND_BUFFER_SPECIAL_START) && (send_buffer_array[0] < SERIAL_SEND_BUFFER_SPECIAL_END))
      {

#if defined(DEBUG_SERVICE_SEND_BUFFER)
        debug_serial_port->println("service_send_buffer: SERIAL_SEND_BUFFER_SPECIAL");
#endif

        if (send_buffer_array[0] == SERIAL_SEND_BUFFER_HOLD_SEND)
        {

#if defined(DEBUG_SERVICE_SEND_BUFFER)
          debug_serial_port->println("service_send_buffer: SERIAL_SEND_BUFFER_HOLD_SEND");
#endif

          send_buffer_status = SERIAL_SEND_BUFFER_HOLD;
          remove_from_send_buffer();
        }

        if (send_buffer_array[0] == SERIAL_SEND_BUFFER_HOLD_SEND_RELEASE)
        {

#if defined(DEBUG_SERVICE_SEND_BUFFER)
          debug_serial_port->println("service_send_buffer: SERIAL_SEND_BUFFER_HOLD_SEND_RELEASE");
#endif

          remove_from_send_buffer();
        }

        if (send_buffer_array[0] == SERIAL_SEND_BUFFER_MEMORY_NUMBER)
        {

#if defined(DEBUG_SERVICE_SEND_BUFFER)
          debug_serial_port->println("service_send_buffer: SERIAL_SEND_BUFFER_MEMORY_NUMBER");
#endif

#ifdef DEBUG_SEND_BUFFER
          debug_serial_port->println(F("service_send_buffer: SERIAL_SEND_BUFFER_MEMORY_NUMBER"));
#endif
#ifdef FEATURE_WINKEY_EMULATION
          if (winkey_sending && winkey_host_open)
          {
#if !defined(OPTION_WINKEY_UCXLOG_SUPRESS_C4_STATUS_BYTE)
            winkey_port_write(0xc0 | winkey_sending | winkey_xoff, 0);
#endif
            winkey_interrupted = 1;
          }
#endif
          remove_from_send_buffer();
          if (send_buffer_bytes)
          {
            if (send_buffer_array[0] < number_of_memories)
            {
#ifdef FEATURE_MEMORIES
              play_memory(send_buffer_array[0]);
#endif
            }
            remove_from_send_buffer();
          }
        }

        if (send_buffer_array[0] == SERIAL_SEND_BUFFER_WPM_CHANGE)
        { // two bytes for wpm
//remove_from_send_buffer();
#if defined(DEBUG_SERVICE_SEND_BUFFER)
          debug_serial_port->println("service_send_buffer: SERIAL_SEND_BUFFER_WPM_CHANGE");
#endif
          if (send_buffer_bytes > 2)
          {
#if defined(DEBUG_SERVICE_SEND_BUFFER)
            debug_serial_port->println("service_send_buffer: SERIAL_SEND_BUFFER_WPM_CHANGE: send_buffer_bytes>2");
#endif
            remove_from_send_buffer();
#ifdef FEATURE_WINKEY_EMULATION
            if ((winkey_host_open) && (winkey_speed_state == WINKEY_UNBUFFERED_SPEED))
            {
              winkey_speed_state = WINKEY_BUFFERED_SPEED;
              winkey_last_unbuffered_speed_wpm = configControl.configuration.wpm;
            }
#endif
            configControl.configuration.wpm = send_buffer_array[0] * 256;
            remove_from_send_buffer();
            configControl.configuration.wpm = configControl.configuration.wpm + send_buffer_array[0];
            remove_from_send_buffer();

#ifdef FEATURE_LED_RING
            update_led_ring();
#endif //FEATURE_LED_RING
          }
          else
          {
#if defined(DEBUG_SERVICE_SEND_BUFFER)
            debug_serial_port->println("service_send_buffer:SERIAL_SEND_BUFFER_WPM_CHANGE < 2");
#endif
          }

#if defined(DEBUG_SERVICE_SEND_BUFFER)
          debug_serial_port->print("service_send_buffer: SERIAL_SEND_BUFFER_WPM_CHANGE: exit send_buffer_bytes:");
          debug_serial_port->println(send_buffer_bytes);
#endif
        }

        if (send_buffer_array[0] == SERIAL_SEND_BUFFER_TX_CHANGE)
        { // one byte for transmitter #

#if defined(DEBUG_SERVICE_SEND_BUFFER)
          debug_serial_port->println("service_send_buffer: SERIAL_SEND_BUFFER_TX_CHANGE");
#endif

          remove_from_send_buffer();
          if (send_buffer_bytes > 1)
          {
// if ((send_buffer_array[0] > 0) && (send_buffer_array[0] < 7)){
//   switch_to_tx_silent(send_buffer_array[0]);
// }
#ifdef FEATURE_SO2R_BASE
            if ((send_buffer_array[0] > 0) && (send_buffer_array[0] < 3))
            {
              if (ptt_line_activated)
              {
                so2r_pending_tx = send_buffer_array[0];
              }
              else
              {
                so2r_tx = send_buffer_array[0];
                so2r_set_tx();
              }
            }
#else
            if ((send_buffer_array[0] > 0) && (send_buffer_array[0] < 7))
            {
              switch_to_tx_silent(send_buffer_array[0]);
            }
#endif //FEATURE_SO2R_BASE

            remove_from_send_buffer();
          }
        }

        if (send_buffer_array[0] == SERIAL_SEND_BUFFER_NULL)
        {

#if defined(DEBUG_SERVICE_SEND_BUFFER)
          debug_serial_port->println("service_send_buffer: SERIAL_SEND_BUFFER_NULL");
#endif

          remove_from_send_buffer();
        }

        if (send_buffer_array[0] == SERIAL_SEND_BUFFER_PROSIGN)
        {

#if defined(DEBUG_SERVICE_SEND_BUFFER)
          debug_serial_port->println("service_send_buffer: SERIAL_SEND_BUFFER_PROSIGN");
#endif

          remove_from_send_buffer();
          if (send_buffer_bytes)
          {
            send_char(send_buffer_array[0], OMIT_LETTERSPACE);
#ifdef FEATURE_WINKEY_EMULATION
            if (winkey_host_open)
            {
              // Must echo back PROSIGN characters sent  N6TV
              winkey_port_write(0xc4 | winkey_sending | winkey_xoff, 0); // N6TV
              winkey_port_write(send_buffer_array[0], 0);                // N6TV
            }
#endif //FEATURE_WINKEY_EMULATION
            remove_from_send_buffer();
          }
          if (send_buffer_bytes)
          {
            send_char(send_buffer_array[0], KEYER_NORMAL);
#ifdef FEATURE_WINKEY_EMULATION
            if (winkey_host_open)
            {
              // Must echo back PROSIGN characters sent  N6TV
              winkey_port_write(0xc4 | winkey_sending | winkey_xoff, 0); // N6TV
              winkey_port_write(send_buffer_array[0], 0);                // N6TV
            }
#endif //FEATURE_WINKEY_EMULATION
            remove_from_send_buffer();
          }
        }

        if (send_buffer_array[0] == SERIAL_SEND_BUFFER_TIMED_KEY_DOWN)
        {

#if defined(DEBUG_SERVICE_SEND_BUFFER)
          debug_serial_port->println("service_send_buffer: SERIAL_SEND_BUFFER_TIMED_KEY_DOWN");
#endif

          remove_from_send_buffer();
          if (send_buffer_bytes)
          {
            send_buffer_status = SERIAL_SEND_BUFFER_TIMED_COMMAND;
            sending_mode = AUTOMATIC_SENDING;
            tx_and_sidetone_key(1);
            timed_command_end_time = millis() + (send_buffer_array[0] * 1000);
            timed_command_in_progress = SERIAL_SEND_BUFFER_TIMED_KEY_DOWN;
            remove_from_send_buffer();
          }
        }

        if (send_buffer_array[0] == SERIAL_SEND_BUFFER_TIMED_WAIT)
        {

#if defined(DEBUG_SERVICE_SEND_BUFFER)
          debug_serial_port->println("service_send_buffer: SERIAL_SEND_BUFFER_TIMED_WAIT");
#endif

          remove_from_send_buffer();
          if (send_buffer_bytes)
          {
            send_buffer_status = SERIAL_SEND_BUFFER_TIMED_COMMAND;
            timed_command_end_time = millis() + (send_buffer_array[0] * 1000);
            timed_command_in_progress = SERIAL_SEND_BUFFER_TIMED_WAIT;
            remove_from_send_buffer();
          }
        }

        if (send_buffer_array[0] == SERIAL_SEND_BUFFER_PTT_ON)
        {

#if defined(DEBUG_SERVICE_SEND_BUFFER)
          debug_serial_port->println("service_send_buffer: SERIAL_SEND_BUFFER_PTT_ON");
#endif

          remove_from_send_buffer();
          manual_ptt_invoke = 1;
          ptt_key();
        }

        if (send_buffer_array[0] == SERIAL_SEND_BUFFER_PTT_OFF)
        {

#if defined(DEBUG_SERVICE_SEND_BUFFER)
          debug_serial_port->println("service_send_buffer: SERIAL_SEND_BUFFER_PTT_OFF");
#endif

          remove_from_send_buffer();
          manual_ptt_invoke = 0;
          ptt_unkey();
        }
      }
      else
      { // if ((send_buffer_array[0] > SERIAL_SEND_BUFFER_SPECIAL_START) && (send_buffer_array[0] < SERIAL_SEND_BUFFER_SPECIAL_END))

#ifdef FEATURE_DISPLAY
        if (displayControl.lcd_send_echo)
        {
          displayControl.displayUpdate(send_buffer_array[0]);

          displayControl.service_display();
        }
#endif //FEATURE_DISPLAY

        send_char(send_buffer_array[0], KEYER_NORMAL); //****************

        if (!pause_sending_buffer)
        {

          if (!((send_buffer_array[0] > SERIAL_SEND_BUFFER_SPECIAL_START) && (send_buffer_array[0] < SERIAL_SEND_BUFFER_SPECIAL_END)))
          { // this is a friggin hack to fix something I can't explain with SO2R - Goody 20191217
            remove_from_send_buffer();
          }
          else
          {
          }
        }
      }
    } //if ((send_buffer_bytes) && (pause_sending_buffer == 0))
  }
  else
  { //if (send_buffer_status == SERIAL_SEND_BUFFER_NORMAL)

    if (send_buffer_status == SERIAL_SEND_BUFFER_TIMED_COMMAND)
    { // we're in a timed command

      if ((timed_command_in_progress == SERIAL_SEND_BUFFER_TIMED_KEY_DOWN) && (millis() > timed_command_end_time))
      {
        sending_mode = AUTOMATIC_SENDING;
        tx_and_sidetone_key(0);
        timed_command_in_progress = 0;
        send_buffer_status = SERIAL_SEND_BUFFER_NORMAL;
      }

      if ((timed_command_in_progress == SERIAL_SEND_BUFFER_TIMED_WAIT) && (millis() > timed_command_end_time))
      {
        timed_command_in_progress = 0;
        send_buffer_status = SERIAL_SEND_BUFFER_NORMAL;
      }
    }

    if (send_buffer_status == SERIAL_SEND_BUFFER_HOLD)
    { // we're in a send hold ; see if there's a SERIAL_SEND_BUFFER_HOLD_SEND_RELEASE in the buffer
      if (send_buffer_bytes == 0)
      {
        send_buffer_status = SERIAL_SEND_BUFFER_NORMAL; // this should never happen, but what the hell, we'll catch it here if it ever does happen
      }
      else
      {
        for (int z = 0; z < send_buffer_bytes; z++)
        {
          if (send_buffer_array[z] == SERIAL_SEND_BUFFER_HOLD_SEND_RELEASE)
          {
            send_buffer_status = SERIAL_SEND_BUFFER_NORMAL;
            z = send_buffer_bytes;
          }
        }
      }
    }

  } //if (send_buffer_status == SERIAL_SEND_BUFFER_NORMAL)

  //if the paddles are hit, dump the buffer
  check_paddles();
  if (((dit_buffer || dah_buffer) && (send_buffer_bytes)) && (keyer_machine_mode != BEACON))
  {

    clear_send_buffer();
    send_buffer_status = SERIAL_SEND_BUFFER_NORMAL;
    dit_buffer = 0;
    dah_buffer = 0;
  }
}

//-------------------------------------------------------------------------------------------------------
void clear_send_buffer()
{

  send_buffer_bytes = 0;
}

//-------------------------------------------------------------------------------------------------------
void remove_from_send_buffer()
{

  if (send_buffer_bytes)
  {
    send_buffer_bytes--;
  }
  if (send_buffer_bytes)
  {
    for (int x = 0; x < send_buffer_bytes; x++)
    {
      send_buffer_array[x] = send_buffer_array[x + 1];
    }
  }
}

//-------------------------------------------------------------------------------------------------------

void add_to_send_buffer(byte incoming_serial_byte)
{

  if (send_buffer_bytes < send_buffer_size)
  {
    if (incoming_serial_byte != 127)
    {
      send_buffer_bytes++;
      send_buffer_array[send_buffer_bytes - 1] = incoming_serial_byte;
    }
    else
    { // we got a backspace
      if (send_buffer_bytes)
      {
        send_buffer_bytes--;
      }
    }
  }
  else
  {
  }
}

void service_injected_text()
{
  while (!injectedText.empty())
  {
    char x;
    String s = injectedText.front();
    s.toUpperCase();
    for (int i = 0; i < s.length(); i++)
    {
      x = s.charAt(i);
      add_to_send_buffer(x);
    }
    injectedText.pop();
  }
}

void service_paddle_echo()
{
#ifdef FEATURE_PADDLE_ECHO
  static byte paddle_echo_space_sent = 1;

  if ((paddle_echo_buffer) && (millis() > paddle_echo_buffer_decode_time))
  {

#ifdef FEATURE_DISPLAY
    if (displayControl.lcd_paddle_echo)
    {

      displayControl.displayUpdate(convert_cw_number_to_string(paddle_echo_buffer));
    }
#endif //FEATURE_DISPLAY

    paddle_echo_buffer = 0;
    paddle_echo_buffer_decode_time = millis() + (float(600 / configControl.configuration.wpm) * length_letterspace);
    paddle_echo_space_sent = 0;
  }

  // is it time to echo a space?
  if ((paddle_echo_buffer == 0) && (millis() > (paddle_echo_buffer_decode_time + (float(1200 / configControl.configuration.wpm) * (configControl.configuration.length_wordspace - length_letterspace)))) && (!paddle_echo_space_sent))
  {

#ifdef FEATURE_DISPLAY
    if (displayControl.lcd_paddle_echo)
    {
      displayControl.displayUpdate(' ');
    }
#endif //FEATURE_DISPLAY

    paddle_echo_space_sent = 1;
  }
#endif //FEATURE_PADDLE_ECHO
}

void initialize_pins()
{
#if !defined M5CORE
#if defined(ARDUINO_MAPLE_MINI) || defined(ARDUINO_GENERIC_STM32F103C) || defined(ESP32) //sp5iou 20180329
  pinMode(paddle_left, INPUT_PULLUP);
  pinMode(paddle_right, INPUT_PULLUP);
#else
  pinMode(paddle_left, INPUT);
  digitalWrite(paddle_left, HIGH);
  pinMode(paddle_right, INPUT);
  digitalWrite(paddle_right, HIGH);
#endif //defined (ARDUINO_MAPLE_MINI)||defined(ARDUINO_GENERIC_STM32F103C) sp5iou 20180329

  if (tx_key_line_1)
  {
    pinMode(tx_key_line_1, OUTPUT);
    digitalWrite(tx_key_line_1, tx_key_line_inactive_state);
  }
  if (tx_key_line_2)
  {
    pinMode(tx_key_line_2, OUTPUT);
    digitalWrite(tx_key_line_2, tx_key_line_inactive_state);
  }
  if (tx_key_line_3)
  {
    pinMode(tx_key_line_3, OUTPUT);
    digitalWrite(tx_key_line_3, tx_key_line_inactive_state);
  }
  if (tx_key_line_4)
  {
    pinMode(tx_key_line_4, OUTPUT);
    digitalWrite(tx_key_line_4, tx_key_line_inactive_state);
  }
  if (tx_key_line_5)
  {
    pinMode(tx_key_line_5, OUTPUT);
    digitalWrite(tx_key_line_5, tx_key_line_inactive_state);
  }
  if (tx_key_line_6)
  {
    pinMode(tx_key_line_6, OUTPUT);
    digitalWrite(tx_key_line_6, tx_key_line_inactive_state);
  }

  if (ptt_tx_1)
  {
    pinMode(ptt_tx_1, OUTPUT);
    digitalWrite(ptt_tx_1, ptt_line_inactive_state);
  }
  if (ptt_tx_2)
  {
    pinMode(ptt_tx_2, OUTPUT);
    digitalWrite(ptt_tx_2, ptt_line_inactive_state);
  }
  if (ptt_tx_3)
  {
    pinMode(ptt_tx_3, OUTPUT);
    digitalWrite(ptt_tx_3, ptt_line_inactive_state);
  }
  if (ptt_tx_4)
  {
    pinMode(ptt_tx_4, OUTPUT);
    digitalWrite(ptt_tx_4, ptt_line_inactive_state);
  }
  if (ptt_tx_5)
  {
    pinMode(ptt_tx_5, OUTPUT);
    digitalWrite(ptt_tx_5, ptt_line_inactive_state);
  }
  if (ptt_tx_6)
  {
    pinMode(ptt_tx_6, OUTPUT);
    digitalWrite(ptt_tx_6, ptt_line_inactive_state);
  }
  /*
  if (sidetone_line)
  {
    pinMode(sidetone_line, OUTPUT);
    digitalWrite(sidetone_line, LOW);
  }
  */
  if (tx_key_dit)
  {
    pinMode(tx_key_dit, OUTPUT);
    digitalWrite(tx_key_dit, tx_key_dit_and_dah_pins_inactive_state);
  }
  if (tx_key_dah)
  {
    pinMode(tx_key_dah, OUTPUT);
    digitalWrite(tx_key_dah, tx_key_dit_and_dah_pins_inactive_state);
  }

  if (ptt_input_pin)
  {
    pinMode(ptt_input_pin, INPUT_PULLUP);
  }

  if (tx_inhibit_pin)
  {
    pinMode(tx_inhibit_pin, INPUT_PULLUP);
  }

  if (tx_pause_pin)
  {
    pinMode(tx_pause_pin, INPUT_PULLUP);
  }

  if (potentiometer_enable_pin)
  {
    pinMode(potentiometer_enable_pin, INPUT_PULLUP);
  }
#endif
} //initialize_pins()

//---------------------------------------------------------------------

//---------------------------------------------------------------------

//---------------------------------------------------------------------

void initialize_keyer_state()
{

  key_state = 0;
  key_tx = 1;

#ifndef FEATURE_SO2R_BASE
  switch_to_tx_silent(1);
#endif
}

//---------------------------------------------------------------------

//---------------------------------------------------------------------

//---------------------------------------------------------------------

void initialize_default_modes()
{

  // setup default modes
  keyer_machine_mode = KEYER_NORMAL;

  char_send_mode = CW;

#if defined(FEATURE_CMOS_SUPER_KEYER_IAMBIC_B_TIMING) && defined(OPTION_CMOS_SUPER_KEYER_IAMBIC_B_TIMING_ON_BY_DEFAULT) // DL1HTB initialize CMOS Super Keyer if feature is enabled
  configControl.configuration.cmos_super_keyer_iambic_b_timing_on = 1;
#endif //FEATURE_CMOS_SUPER_KEYER_IAMBIC_B_TIMING // #end DL1HTB initialize CMOS Super Keyer if feature is enabled

  delay(250); // wait a little bit for the caps to charge up on the paddle lines
}

//---------------------------------------------------------------------

//---------------------------------------------------------------------

//---------------------------------------------------------------------

void check_for_beacon_mode()
{
#if !defined M5CORE
#ifndef OPTION_SAVE_MEMORY_NANOKEYER
  // check for beacon mode (paddle_left == low) or straight key mode (paddle_right == low)
  if (paddle_pin_read(paddle_left) == LOW)
  {
#ifdef FEATURE_BEACON
    keyer_machine_mode = BEACON;
#endif
  }
  else
  {
    if (paddle_pin_read(paddle_right) == LOW)
    {
      configControl.configuration.keyer_mode = STRAIGHT;
    }
  }
#endif //OPTION_SAVE_MEMORY_NANOKEYER
#endif
}

//---------------------------------------------------------------------

//---------------------------------------------------------------------

int paddle_pin_read(int pin_to_read)
{
  return virtualPins.pinsets.at(pin_to_read)->digRead();
}

void service_millis_rollover()
{

  static unsigned long last_millis = 0;

  if (millis() < last_millis)
  {
    millis_rollover++;
  }
  last_millis = millis();
}

byte is_visible_character(byte char_in)
{

  if ((char_in > 31) || (char_in == 9) || (char_in == 10) || (char_in == 13))
  {
    return 1;
  }
  else
  {
    return 0;
  }
}

void lcd_center_print_timed_wpm()
{

#if defined(OPTION_ADVANCED_SPEED_DISPLAY)
  displayControl.lcd_center_print_timed(String(configuration.wpm) + " wpm - " + (configuration.wpm * 5) + " cpm", 0, default_display_msg_delay);
  displayControl.lcd_center_print_timed(String(1200 / configuration.wpm) + ":" + (((1200 / configuration.wpm) * configuration.dah_to_dit_ratio) / 100) + "ms 1:" + (float(configuration.dah_to_dit_ratio) / 100.00), 1, default_display_msg_delay);
#else
  displayControl.lcd_center_print_timed(String(configControl.configuration.wpm) + " wpm", 0, default_display_msg_delay);
#endif
}

void speed_set(int wpm_set)
{

  if ((wpm_set > 0) && (wpm_set < 1000))
  {
    configControl.configuration.wpm = wpm_set;
    configControl.config_dirty = 1;

#ifdef FEATURE_DYNAMIC_DAH_TO_DIT_RATIO
    if ((configuration.wpm >= DYNAMIC_DAH_TO_DIT_RATIO_LOWER_LIMIT_WPM) && (configuration.wpm <= DYNAMIC_DAH_TO_DIT_RATIO_UPPER_LIMIT_WPM))
    {
      int dynamicweightvalue = map(configuration.wpm, DYNAMIC_DAH_TO_DIT_RATIO_LOWER_LIMIT_WPM, DYNAMIC_DAH_TO_DIT_RATIO_UPPER_LIMIT_WPM, DYNAMIC_DAH_TO_DIT_RATIO_LOWER_LIMIT_RATIO, DYNAMIC_DAH_TO_DIT_RATIO_UPPER_LIMIT_RATIO);
      configuration.dah_to_dit_ratio = dynamicweightvalue;
    }
#endif //FEATURE_DYNAMIC_DAH_TO_DIT_RATIO

#ifdef FEATURE_LED_RING
    update_led_ring();
#endif //FEATURE_LED_RING

    lcd_center_print_timed_wpm();
  }
}

void command_speed_set(int wpm_set)
{
  if ((wpm_set > 0) && (wpm_set < 1000))
  {
    configControl.configuration.wpm_command_mode = wpm_set;
    configControl.config_dirty = 1;

    displayControl.lcd_center_print_timed("Cmd Spd " + String(configControl.configuration.wpm_command_mode) + " wpm", 0, default_display_msg_delay);

  } // end if
} // end command_speed_set

void wpmSetCallBack(byte pot_value_wpm_read)
{
  if (keyer_machine_mode == KEYER_COMMAND_MODE)
    command_speed_set(pot_value_wpm_read);
  else
    speed_set(pot_value_wpm_read);
}