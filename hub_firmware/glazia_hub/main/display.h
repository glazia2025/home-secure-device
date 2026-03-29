#pragma once

// Call once at boot
void display_init(void);

// Show two lines on the TFT
// line1 = title (big), line2 = subtitle (small)
void display_show(const char *line1, const char *line2);
