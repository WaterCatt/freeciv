#ifndef FC__CLIENT_REPLAY_H
#define FC__CLIENT_REPLAY_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "support.h"

void client_replay_set_file(char *filename);
void client_replay_set_start_paused(bool paused);
void client_replay_set_startup_steps(int steps);
bool client_replay_set_speed_name(const char *name);
void client_replay_toggle_pause(void);
void client_replay_step_forward(void);
void client_replay_set_speed_level(int level);
bool client_replay_requested(void);
bool client_replay_start_requested(void);
bool client_replay_active(void);
bool client_replay_paused(void);
bool client_replay_step(void);
int client_replay_timer_interval_ms(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* FC__CLIENT_REPLAY_H */
