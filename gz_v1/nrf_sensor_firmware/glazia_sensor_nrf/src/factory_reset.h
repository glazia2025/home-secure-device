#pragma once

/* Debugger-free credential wipe. Called once, synchronously, at the very start of boot
 * (before led_sensor_init / thread_join_init). If the factory-reset button (D1 / P0.03,
 * active-low) is held ~3 s, erases the stored Thread dataset via otInstanceFactoryReset()
 * and reboots. Returns immediately (no boot delay) if the button is not pressed at boot. */
void factory_reset_check(void);
