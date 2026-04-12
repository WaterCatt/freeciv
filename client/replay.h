#ifndef FC__CLIENT_REPLAY_H
#define FC__CLIENT_REPLAY_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "support.h"

#define CLIENT_REPLAY_INFO_TEXT_LEN 128
#define CLIENT_REPLAY_PLAYERS_TEXT_LEN 512

struct client_replay_info {
  bool valid;
  char ruleset[CLIENT_REPLAY_INFO_TEXT_LEN];
  char scenario[CLIENT_REPLAY_INFO_TEXT_LEN];
  int start_turn;
  int start_year;
  int final_turn;
  int duration_seconds;
  char players[CLIENT_REPLAY_PLAYERS_TEXT_LEN];
  char result[CLIENT_REPLAY_INFO_TEXT_LEN];
  char winner[CLIENT_REPLAY_INFO_TEXT_LEN];
};

struct client_replay_preview {
  bool valid;
  int width;
  int height;
  unsigned char *rgb;
};

void client_replay_set_file(char *filename);
bool client_replay_read_info(const char *filename,
                             struct client_replay_info *info);
bool client_replay_read_preview(const char *filename,
                                struct client_replay_preview *preview);
void client_replay_free_preview(struct client_replay_preview *preview);
void client_replay_set_start_paused(bool paused);
void client_replay_set_startup_steps(int steps);
bool client_replay_set_speed_name(const char *name);
void client_replay_toggle_pause(void);
void client_replay_step_backward(void);
void client_replay_step_forward(void);
void client_replay_set_speed_level(int level);
bool client_replay_requested(void);
bool client_replay_mode(void);
bool client_replay_start_requested(void);
void client_replay_stop_mode(void);
bool client_replay_active(void);
bool client_replay_paused(void);
bool client_replay_step(void);
void client_replay_seek_position(int position);
void client_replay_seek_turn(int turn);
void client_replay_set_pov_player(int player_number);
int client_replay_pov_player_number(void);
int client_replay_timer_interval_ms(void);
int client_replay_speed_level(void);
int client_replay_initial_turn(void);
int client_replay_final_turn(void);
int client_replay_position(void);
int client_replay_length(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* FC__CLIENT_REPLAY_H */
