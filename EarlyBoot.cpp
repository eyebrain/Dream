// Early boot test: very small image that toggles the LED using DaisySeed only.
// Avoids display and SDRAM initialization so we can confirm firmware starts.
#include "daisy_seed.h"

using namespace daisy;

DaisySeed hw;

int main(void)
{
    // Minimal init for the seed hardware
    hw.Configure();
    hw.Init();

    // Blink onboard LED (200ms) to show early boot reached
    bool led_state = false;
    while(1)
    {
        hw.SetLed(led_state);
        led_state = !led_state;
        System::Delay(200);
    }
}
