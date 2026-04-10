#ifndef FC__CLIENT_REPLAY_H
#define FC__CLIENT_REPLAY_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "support.h"

void client_replay_set_file(char *filename);
bool client_replay_requested(void);
bool client_replay_start_requested(void);
bool client_replay_active(void);
bool client_replay_step(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* FC__CLIENT_REPLAY_H */
