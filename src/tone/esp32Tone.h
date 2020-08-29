#include "toneBase.h"

class esp32Tone : public toneBase
{
public:
    esp32Tone(uint8_t pin);
    virtual void noTone();
    virtual void tone(unsigned short freq, unsigned duration = 0);
};