#include <stdint.h>

extern volatile uint16_t ChannelData[16];

// Read RC channel value in microseconds (1000-2000)
// ch = 1-16 (1-based indexing)
uint16_t rc_read_us(uint8_t ch)
{
    if (ch < 1 || ch > 16) return 1500; // default center
    
    // ChannelData uses 0-based indexing (0-15)
    // Convert from CRSF range (172-1811) to PWM (1000-2000)
    uint16_t crsf_val = ChannelData[ch - 1];
    
    // CRSF standard: 172 = 1000us, 992 = 1500us, 1811 = 2000us
    int32_t us = ((int32_t)crsf_val - 172) * 1000 / 1639 + 1000;
    
    // Clamp to valid range
    if (us < 1000) us = 1000;
    if (us > 2000) us = 2000;
    
    return (uint16_t)us;
}
