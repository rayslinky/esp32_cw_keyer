#include "FS.h"
#include "SPIFFS.h"
#include "ArduinoJson.h"

#define GENERIC_CHARGRAB
#define OLED_DISPLAY_TFT_ESPI

#ifdef OLED_DISPLAY_TFT_ESPI
#include <TFT_eSPI.h>
#include <SPI.h>
#include <Wire.h>

int _DISPLAY_INITIALIZED = 0;
int _DISPLAY_HEIGHT = 0; //my little sisplay is 64x128
int _DISPLAY_WIDTH = 0;  //128

// EDIT .pio/libdeps/ttgo-t1/TFT_eSPI/User_Setup_Select.h
// To select User_Setups/Setup25_TTGO_T_Display.h

TFT_eSPI display = TFT_eSPI(135, 240); 
#endif

#define CONFIG_JSON_FILENAME "/configuration.json"
#define FORMAT_SPIFFS_IF_FAILED true
#define SPIFFS_LOG_SERIAL false
#define SOUND_PWM_CHANNEL 0
#define SOUND_RESOLUTION 8                     // 8 bit resolution

PRIMARY_SERIAL_CLS *esp32_port_to_use;
int _SPIFFS_MADE_READY = 0;

String displayContents = "";

//#define DEBUG_LOOP

void tone(uint8_t pin, unsigned short freq, unsigned duration = 0)
{
    ledcSetup(SOUND_PWM_CHANNEL, freq, SOUND_RESOLUTION); // Set up PWM channel
    ledcAttachPin(pin, SOUND_PWM_CHANNEL);
    ledcWriteTone(SOUND_PWM_CHANNEL, freq); // Attach channel to pin
    //ledcWrite(SOUND_PWM_CHANNEL, SOUND_ON);

    delay(duration);
    //ledcWrite(SOUND_PWM_CHANNEL, SOUND_OFF);
}
void noTone(uint8_t pin)
{
    tone(pin, -1);
}

void makeSpiffsReady()
{
    if (!SPIFFS.begin(FORMAT_SPIFFS_IF_FAILED))
    {
        //Serial.println("SPIFFS Mount Failed");
        if (SPIFFS_LOG_SERIAL)
        {
            esp32_port_to_use->println("SPIFFS Mount Failed");
        }
    }
    else
    {
        File root = SPIFFS.open("/");

        File file = root.openNextFile();

        while (file)
        {

            //Serial.print("FILE: ");
            if (SPIFFS_LOG_SERIAL)
            {
                esp32_port_to_use->print("FILE: ");
            }
            //Serial.println(file.name());
            if (SPIFFS_LOG_SERIAL)
            {
                esp32_port_to_use->println(file.name());
            }
            file = root.openNextFile();
        }

        _SPIFFS_MADE_READY = 1;
    }
}

int configFileExists()
{
    if (!_SPIFFS_MADE_READY)
    {
        makeSpiffsReady();
    }
    File file = SPIFFS.open(CONFIG_JSON_FILENAME, FILE_READ);
    int exists = 1;
    if (!file || file.isDirectory())
    {
        if (SPIFFS_LOG_SERIAL)
        {
            esp32_port_to_use->println("Config file doesn't exist");
        }
        exists = 0;
    }
    else
    {
        if (SPIFFS_LOG_SERIAL)
        {
            esp32_port_to_use->println("Config file was found.");
        }
    }
    file.close();
    return exists;
}

String getConfigFileJsonString()
{
    if (!_SPIFFS_MADE_READY)
    {
        makeSpiffsReady();
    }
    File file = SPIFFS.open(CONFIG_JSON_FILENAME, FILE_READ);
    String jsonReturn;
    while (file.available())
    {
        jsonReturn = jsonReturn + file.readString();
    }
    file.close();
    return jsonReturn;
}

void setConfigurationFromFile()
{
    String jsonString = getConfigFileJsonString();
    //const size_t capacity = 6 * JSON_ARRAY_SIZE(2) + 4 * JSON_ARRAY_SIZE(4) + 2 * JSON_ARRAY_SIZE(5) + 2 * JSON_ARRAY_SIZE(6) + JSON_OBJECT_SIZE(23) + 410;
    const size_t capacity = 4096;
    DynamicJsonDocument doc(capacity);

    //const char *json = "{\"cli_mode\":1,\"ptt_buffer_hold_active\":1,\"wpm\":1,\"hz_sidetone\":1,\"dah_to_dit_ratio\":1,\"wpm_farnsworth\":1,\"memory_repeat_time\":1,\"wpm_command_mode\":1,\"link_receive_udp_port\":1,\"wpm_ps2_usb_keyboard\":1,\"wpm_cli\":1,\"wpm_winkey\":1,\"ip\":[4,4,4,4],\"gateway\":[4,4,4,4],\"subnet\":[4,4,4,4],\"link_send_ip\":[[2,2],[2,2],[2,2],[2,2]],\"link_send_enabled\":[2,2],\"link_send_udp_port\":[2,2],\"ptt_lead_time\":[6,6,6,6,6,6],\"ptt_tail_time\":[6,6,6,6,6,6],\"ptt_active_to_sequencer_active_time\":[5,5,5,5,5],\"ptt_inactive_to_sequencer_inactive_time\":[5,5,5,5,5],\"sidetone_volume\":1}";

    if (SPIFFS_LOG_SERIAL)
    {
        esp32_port_to_use->println(jsonString);
    }                                                                   // 1

    deserializeJson(doc, jsonString);

    configuration.cli_mode = doc["cli_mode"];                             // 1
    configuration.ptt_buffer_hold_active = doc["ptt_buffer_hold_active"]; // 1
    configuration.wpm = doc["wpm"];
    if (SPIFFS_LOG_SERIAL)
    {
        esp32_port_to_use->print("Configuration.wpm was just set from json to:");
        esp32_port_to_use->println(configuration.wpm);
    }                                                                   // 1
    configuration.hz_sidetone = doc["hz_sidetone"];                     // 1
    configuration.dah_to_dit_ratio = doc["dah_to_dit_ratio"];           // 1
    configuration.wpm_farnsworth = doc["wpm_farnsworth"];               // 1
    configuration.memory_repeat_time = doc["memory_repeat_time"];       // 1
    configuration.wpm_command_mode = doc["wpm_command_mode"];           // 1
    configuration.link_receive_udp_port = doc["link_receive_udp_port"]; // 1
    configuration.wpm_ps2_usb_keyboard = doc["wpm_ps2_usb_keyboard"];   // 1
    configuration.wpm_cli = doc["wpm_cli"];                             // 1
    configuration.wpm_winkey = doc["wpm_winkey"];                       // 1
    configuration.paddle_mode = doc["paddle_mode"];                     // ?
    configuration.sidetone_volume = doc["sidetone_volume"]; // 1
}

String getJsonStringFromConfiguration()
{
    //const size_t capacity = 6 * JSON_ARRAY_SIZE(2) + 4 * JSON_ARRAY_SIZE(4) + 2 * JSON_ARRAY_SIZE(5) + 2 * JSON_ARRAY_SIZE(6) + JSON_OBJECT_SIZE(23);
    const size_t capacity = 4096;
    DynamicJsonDocument doc(capacity);

    doc["cli_mode"] = configuration.cli_mode;
    doc["ptt_buffer_hold_active"] = configuration.ptt_buffer_hold_active;
    if (SPIFFS_LOG_SERIAL)
    {
        esp32_port_to_use->print("Configuration.wpm is about to set json to:");
        esp32_port_to_use->println(configuration.wpm);
    }
    doc["wpm"] = configuration.wpm;
    doc["hz_sidetone"] = configuration.hz_sidetone;
    doc["dah_to_dit_ratio"] = configuration.dah_to_dit_ratio;
    doc["wpm_farnsworth"] = configuration.wpm_farnsworth;
    doc["memory_repeat_time"] = configuration.memory_repeat_time;
    doc["wpm_command_mode"] = configuration.wpm_command_mode;
    doc["link_receive_udp_port"] = configuration.link_receive_udp_port;
    doc["wpm_ps2_usb_keyboard"] = configuration.wpm_ps2_usb_keyboard;
    doc["wpm_cli"] = configuration.wpm_cli;
    doc["wpm_winkey"] = configuration.wpm_winkey;
    doc["paddle_mode"] = configuration.paddle_mode;
    doc["sidetone_volume"] = configuration.sidetone_volume;

    String returnString;
    serializeJson(doc, returnString);

    return returnString;
}

void writeConfigurationToFile()
{
    if (!_SPIFFS_MADE_READY)
    {
        makeSpiffsReady();
    }
    String jsonToWrite = getJsonStringFromConfiguration();
    File newfile = SPIFFS.open(CONFIG_JSON_FILENAME, FILE_WRITE);
    if (!newfile)
    {
        if (SPIFFS_LOG_SERIAL)
        {
            esp32_port_to_use->println("- failed to open file for writing");
        }
        //return;
    }
    if (newfile.print(jsonToWrite))
    {
        if (SPIFFS_LOG_SERIAL)
        {
            esp32_port_to_use->println("- file written");
        }
        newfile.close();
    }
    else
    {
        if (SPIFFS_LOG_SERIAL)
        {
            esp32_port_to_use->println("- write failed");
        }
    }
    delay(200);
}

void initializeSpiffs(PRIMARY_SERIAL_CLS *port_to_use)
{
    esp32_port_to_use = port_to_use;
}

void initDisplay()
{
#ifdef OLED_DISPLAY_TFT_ESPI
    display.init();
    display.setRotation(1);
    display.fillScreen(TFT_BLACK);
    display.setTextSize(3);
    display.setTextColor(TFT_GREEN, TFT_BLACK);
    display.setTextDatum(ML_DATUM);
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
    display.setSwapBytes(true);
    _DISPLAY_HEIGHT = display.height();
    _DISPLAY_WIDTH = display.width();
    _DISPLAY_INITIALIZED = 1;
#endif
}

void displayUpdate(char character)
{
#ifdef ESPNOW_WIRELESS_KEYER
    sendEspNowData(character);
#endif
#ifdef OLED_DISPLAY_TFT_ESPI
    displayContents += character;
    if (SPIFFS_LOG_SERIAL)
    {
        esp32_port_to_use->print("displayupdatecalled with: ");
        esp32_port_to_use->println(displayContents);
    }
    if (!_DISPLAY_INITIALIZED)
    {
        initDisplay();
    }
    int overflowing = 0;
    int rowLength = 13;
    String displayLine1 = "";
    String displayLine2 = "";
    String displayLine3 = "";
    String displayLine4 = "";
    do
    {
        if (overflowing)
        {
            int skip = displayLine1.length();
            displayContents = displayContents.substring(skip);
        }
        overflowing = 0;
        displayLine1 = "";
        displayLine2 = "";
        displayLine3 = "";
        displayLine4 = "";

        for (int i = 0; i < displayContents.length(); i++)
        {
            if (displayLine2.length() == 0 && ((displayLine1 + displayContents[i]).length() <= rowLength))
            {
                displayLine1 += displayContents[i];
            }
            else if ((displayLine2 + displayContents[i]).length() <= rowLength)
            {
                displayLine2 += displayContents[i];
            }
            else if ((displayLine3 + displayContents[i]).length() <= rowLength)
            {
                displayLine3 += displayContents[i];
            }
            else if ((displayLine4 + displayContents[i]).length() <= rowLength)
            {
                displayLine4 += displayContents[i];
            }
            else
            {
                overflowing = 1;
            }
        }
    } while (overflowing);
    display.fillScreen(TFT_BLACK);
    // 15,50,86,123   // 4 line
    // 15,42,68,96,123 // 5 line
    display.drawString(displayLine1, 1, 15);
    display.drawString(displayLine2, 1, 50);
    display.drawString(displayLine3, 1, 86);
    display.drawString(displayLine4, 1, 123);
#endif
#ifdef OLED_DISPLAY_TFT_ESPI
#endif
}
