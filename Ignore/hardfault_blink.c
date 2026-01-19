// Minimal HardFault handler that toggles the user LED (PC7) so we can detect
// early crashes before higher-level init runs.
#include "stm32h7xx.h"

// Keep this extern "C" with the exact handler name used by the vector table.
void HardFault_Handler(void)
{
    // Enable GPIOC clock (AHB4ENR -> GPIOCEN)
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOCEN;

    // Configure PC7 as general-purpose output (MODER bits = 01)
    // Clear the two bits first
    GPIOC->MODER &= ~(0x3u << (7 * 2));
    // Set to output
    GPIOC->MODER |= (0x1u << (7 * 2));

    // Blink PC7 in a distinctive pattern: 4 quick blinks, pause, repeat
    for(;;)
    {
        for(int k = 0; k < 4; k++)
        {
            GPIOC->ODR ^= (1u << 7);
            // crude busy delay
            for(volatile uint32_t d = 0; d < 200000; d++)
                ;
        }
        // pause
        for(volatile uint32_t d = 0; d < 800000; d++)
            ;
    }
}
