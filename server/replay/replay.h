#ifndef FC__SERVER_REPLAY_H
#define FC__SERVER_REPLAY_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* utility */
#include "support.h"

struct conn_list;
struct connection;

bool replay_recorder_is_active(void);
bool replay_recorder_should_send(const struct conn_list *dest);
struct connection *replay_recorder_connection(void);
struct conn_list *replay_recorder_dest(void);

bool replay_recorder_start(void);
void replay_recorder_begin_snapshot(void);
void replay_recorder_end_snapshot(void);
void replay_recorder_stop(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* FC__SERVER_REPLAY_H */
