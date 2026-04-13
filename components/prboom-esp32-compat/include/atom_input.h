#ifndef ATOM_INPUT_H
#define ATOM_INPUT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize AtomS3 input system (I2C joystick + GPIO button)
 *
 * Initializes I2C communication with M5Stack Atom Joystick Unit and
 * configures the built-in GPIO button. Starts a polling task to read
 * joystick and button state and post Doom events.
 */
void atomInputInit(void);

/**
 * @brief Get joystick state as PS2-compatible bitmask
 *
 * Returns the current joystick and button state as a PS2 controller
 * compatible bitmask for compatibility with existing Doom code.
 *
 * @return PS2 controller bitmask (0xFFFF = no buttons pressed)
 */
int atomJsInputGet(void);

#ifdef __cplusplus
}
#endif

#endif // ATOM_INPUT_H
