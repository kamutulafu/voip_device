#ifndef VOICE_FLOW_H
#define VOICE_FLOW_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the voice interaction subsystem (idempotent).
 *
 * Sets the backend base URL and installs the physical wake button on
 * WAKE_BUTTON_GPIO (GPIO45). Does not start any interaction; call
 * voice_flow_wake(), press the button, or use the "voice_wake" console
 * command to begin a session.
 */
esp_err_t voice_flow_init(void);

/**
 * @brief Trigger a wake event and run one full voice interaction session.
 *
 * Runs the state machine (greeting -> face recognition -> menu/messages/... ->
 * farewell) on a dedicated task. Returns immediately; only one session runs at
 * a time (further triggers are ignored while a session is active).
 *
 * Trigger sources: physical wake button (GPIO45), or console "voice_wake".
 */
void voice_flow_wake(void);

/** Console command: simulate the wake word. Usage: voice_wake */
int cmd_voice_wake(int argc, char **argv);

/** Register the voice_wake console command. */
void register_voice_wake_cmd(void);

/** Get current device use type (0=收费/校外, 1=免费/校内). */
int get_device_use_type(void);

/** Set device use type (0=收费/校外, 1=免费/校内). */
void set_device_use_type(int use_type);

#ifdef __cplusplus
}
#endif

#endif // VOICE_FLOW_H
