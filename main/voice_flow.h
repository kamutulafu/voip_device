#ifndef VOICE_FLOW_H
#define VOICE_FLOW_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the voice interaction subsystem (idempotent).
 *
 * Sets the backend base URL. Does not start any interaction; call
 * voice_flow_wake() (or the "voice_wake" console command) to begin a session.
 */
esp_err_t voice_flow_init(void);

/**
 * @brief Trigger a wake event and run one full voice interaction session.
 *
 * Runs the state machine (greeting -> face recognition -> menu/messages/... ->
 * farewell) on a dedicated task. Returns immediately; only one session runs at
 * a time (further triggers are ignored while a session is active).
 */
void voice_flow_wake(void);

/** Console command: simulate the wake word. Usage: voice_wake */
int cmd_voice_wake(int argc, char **argv);

/** Register the voice_wake console command. */
void register_voice_wake_cmd(void);

#ifdef __cplusplus
}
#endif

#endif // VOICE_FLOW_H
