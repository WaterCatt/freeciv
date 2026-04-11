#ifdef HAVE_CONFIG_H
#include <fc_config.h>
#endif

#include "fc_prehdrs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* utility */
#include "fcintl.h"
#include "log.h"
#include "mem.h"
#include "support.h"

/* common */
#include "game.h"
#include "packets.h"

/* common/networking */
#include "connection.h"

/* client */
#include "client_main.h"
#include "update_queue.h"

#include "replay.h"

struct client_replay {
  FILE *file;
  char *path;
  char *capability;
  struct connection conn;
  bool active;
  bool paused;
  bool start_paused;
  bool conn_init;
  uint32_t current_chunk_remaining;
  char current_chunk[5];
  int speed;
  int startup_steps;
  int initial_turn;
  int snapshot_frames;
  int event_frames;
  int total_event_frames;
};

static struct client_replay replay;

static void replay_close(void);
static bool replay_load_from_path(void);
static bool replay_scan_totals(void);
static bool replay_step_frame(void);
static bool replay_step_turn_forward_internal(void);

static const char *replay_speed_name(int speed)
{
  switch (speed) {
  case 0:
    return "slow";
  case 2:
    return "fast";
  default:
    return "normal";
  }
}

static void replay_finish(void)
{
  log_normal("Replay playback finished at turn %d, year %d (%d snapshot frames, %d event frames).",
             game.info.turn, game.info.year,
             replay.snapshot_frames, replay.event_frames);

  replay.active = FALSE;
  replay_close();
}

static bool replay_advance_frame(void)
{
  if (!replay.active) {
    return FALSE;
  }

  if (!replay_step_frame()) {
    replay_finish();
    return FALSE;
  }

  return TRUE;
}

static bool replay_read_bytes(void *dst, size_t size)
{
  return fread(dst, 1, size, replay.file) == size;
}

static bool replay_read_u16(uint16_t *value)
{
  unsigned char buf[2];

  if (!replay_read_bytes(buf, sizeof(buf))) {
    return FALSE;
  }

  *value = buf[0] | (buf[1] << 8);
  return TRUE;
}

static bool replay_read_u32(uint32_t *value)
{
  unsigned char buf[4];

  if (!replay_read_bytes(buf, sizeof(buf))) {
    return FALSE;
  }

  *value = buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((uint32_t) buf[3] << 24);
  return TRUE;
}

static bool replay_read_u64(uint64_t *value)
{
  unsigned char buf[8];
  int i;

  if (!replay_read_bytes(buf, sizeof(buf))) {
    return FALSE;
  }

  *value = 0;
  for (i = 0; i < 8; i++) {
    *value |= ((uint64_t) buf[i]) << (8 * i);
  }

  return TRUE;
}

static bool replay_read_string(char **value)
{
  uint16_t len;

  *value = NULL;

  if (!replay_read_u16(&len)) {
    return FALSE;
  }

  *value = fc_malloc(len + 1);
  if (len > 0 && !replay_read_bytes(*value, len)) {
    FC_FREE(*value);
    return FALSE;
  }

  (*value)[len] = '\0';
  return TRUE;
}

static bool replay_skip_bytes(uint32_t size)
{
  return fseek(replay.file, size, SEEK_CUR) == 0;
}

static bool replay_read_chunk_header(void)
{
  uint32_t size;

  if (!replay_read_bytes(replay.current_chunk, 4)) {
    return FALSE;
  }

  replay.current_chunk[4] = '\0';

  if (!replay_read_u32(&size)) {
    return FALSE;
  }

  replay.current_chunk_remaining = size;
  return TRUE;
}

static bool replay_parse_info_chunk(void)
{
  char *version = NULL;
  char *ruleset = NULL;
  char *scenario = NULL;
  uint32_t turn;
  uint32_t year;
  uint64_t timestamp;

  if (!replay_read_string(&version)
      || !replay_read_string(&replay.capability)
      || !replay_read_string(&ruleset)
      || !replay_read_string(&scenario)
      || !replay_read_u32(&turn)
      || !replay_read_u32(&year)
      || !replay_read_u64(&timestamp)) {
    FC_FREE(version);
    FC_FREE(ruleset);
    FC_FREE(scenario);
    return FALSE;
  }

  FC_FREE(version);
  FC_FREE(ruleset);
  FC_FREE(scenario);

  replay.current_chunk_remaining = 0;
  return TRUE;
}

static bool replay_prepare_next_chunk(void)
{
  while (replay.current_chunk_remaining == 0) {
    if (!replay_read_chunk_header()) {
      return FALSE;
    }

    if (strcmp(replay.current_chunk, "SNAP") == 0
        || strcmp(replay.current_chunk, "EVNT") == 0) {
      return TRUE;
    }

    if (strcmp(replay.current_chunk, "DONE") == 0) {
      return FALSE;
    }

    if (!replay_skip_bytes(replay.current_chunk_remaining)) {
      return FALSE;
    }

    replay.current_chunk_remaining = 0;
  }

  return TRUE;
}

static bool replay_buffer_append(struct connection *pconn,
                                 const unsigned char *data, size_t len)
{
  struct socket_packet_buffer *buffer = pconn->buffer;
  int needed = buffer->ndata + len;

  if (needed > buffer->nsize) {
    int new_size = MAX(needed, buffer->nsize * 2 + 1024);

    buffer->data = fc_realloc(buffer->data, new_size);
    buffer->nsize = new_size;
  }

  memcpy(buffer->data + buffer->ndata, data, len);
  buffer->ndata += len;

  return TRUE;
}

static bool replay_inject_frame(const unsigned char *frame, uint32_t len)
{
  if (!replay_buffer_append(&replay.conn, frame, len)) {
    return FALSE;
  }

  while (replay.conn.used) {
    enum packet_type type;
    void *packet = get_packet_from_connection(&replay.conn, &type, FALSE);

    if (packet == NULL) {
      break;
    }

    client_packet_input(packet, type);
    packet_destroy(packet, type);
  }

  return replay.conn.used;
}

static bool replay_step_frame(void)
{
  uint32_t frame_len;
  unsigned char *frame;
  bool is_snapshot;

  if (!replay_prepare_next_chunk()) {
    return FALSE;
  }

  if (replay.current_chunk_remaining < 4 || !replay_read_u32(&frame_len)) {
    return FALSE;
  }

  replay.current_chunk_remaining -= 4;

  if (frame_len > replay.current_chunk_remaining) {
    log_error("Replay frame length %u exceeds chunk remainder %u.",
              frame_len, replay.current_chunk_remaining);
    return FALSE;
  }

  frame = fc_malloc(frame_len);
  if (!replay_read_bytes(frame, frame_len)) {
    free(frame);
    return FALSE;
  }

  replay.current_chunk_remaining -= frame_len;
  is_snapshot = strcmp(replay.current_chunk, "SNAP") == 0;

  if (!replay_inject_frame(frame, frame_len)) {
    free(frame);
    return FALSE;
  }

  free(frame);

  if (is_snapshot) {
    replay.snapshot_frames++;
  } else {
    replay.event_frames++;
  }

  return TRUE;
}

static bool replay_scan_totals(void)
{
  long saved_offset = ftell(replay.file);
  uint32_t saved_remaining = replay.current_chunk_remaining;
  char saved_chunk[5];

  sz_strlcpy(saved_chunk, replay.current_chunk);
  replay.total_event_frames = 0;

  while (TRUE) {
    while (replay.current_chunk_remaining > 0) {
      uint32_t frame_len;

      if (replay.current_chunk_remaining < 4 || !replay_read_u32(&frame_len)) {
        goto fail;
      }

      replay.current_chunk_remaining -= 4;

      if (frame_len > replay.current_chunk_remaining
          || !replay_skip_bytes(frame_len)) {
        goto fail;
      }

      replay.current_chunk_remaining -= frame_len;

      if (strcmp(replay.current_chunk, "EVNT") == 0) {
        replay.total_event_frames++;
      }
    }

    if (!replay_prepare_next_chunk()) {
      if (strcmp(replay.current_chunk, "DONE") != 0) {
        goto fail;
      }
      break;
    }
  }

  if (fseek(replay.file, saved_offset, SEEK_SET) != 0) {
    return FALSE;
  }

  replay.current_chunk_remaining = saved_remaining;
  sz_strlcpy(replay.current_chunk, saved_chunk);
  return TRUE;

fail:
  if (saved_offset >= 0) {
    fseek(replay.file, saved_offset, SEEK_SET);
  }
  replay.current_chunk_remaining = saved_remaining;
  sz_strlcpy(replay.current_chunk, saved_chunk);
  return FALSE;
}

static void replay_close(void)
{
  if (replay.conn_init) {
    connection_common_close(&replay.conn);
    conn_list_destroy(replay.conn.self);
    replay.conn.self = NULL;
    replay.conn_init = FALSE;
  }

  if (replay.file != NULL) {
    fclose(replay.file);
    replay.file = NULL;
  }

  FC_FREE(replay.capability);
  replay.capability = NULL;
  replay.current_chunk_remaining = 0;
  replay.current_chunk[0] = '\0';
  replay.paused = FALSE;
  replay.speed = 1;
  replay.startup_steps = 0;
  replay.initial_turn = 0;
  replay.total_event_frames = 0;
}

static bool replay_open_and_parse(void)
{
  char magic[8];
  uint16_t version;
  uint16_t flags;
  struct packet_server_join_reply join_reply = {
    .you_can_join = TRUE
  };

  replay.initial_turn = 0;
  replay.snapshot_frames = 0;
  replay.event_frames = 0;
  replay.total_event_frames = 0;

  replay.file = fopen(replay.path, "rb");
  if (replay.file == NULL) {
    log_error("Failed opening replay '%s': %s", replay.path, strerror(errno));
    return FALSE;
  }

  if (!replay_read_bytes(magic, sizeof(magic))
      || memcmp(magic, "FCREPLAY", sizeof(magic)) != 0
      || !replay_read_u16(&version)
      || !replay_read_u16(&flags)) {
    log_error("Replay '%s' is not a supported .fcreplay file.", replay.path);
    replay_close();
    return FALSE;
  }

  if (version != 1) {
    log_error("Replay '%s' format version %u is unsupported.",
              replay.path, version);
    replay_close();
    return FALSE;
  }

  if (!replay_read_chunk_header() || strcmp(replay.current_chunk, "INFO") != 0
      || !replay_parse_info_chunk()) {
    log_error("Replay '%s' is missing a valid INFO chunk.", replay.path);
    replay_close();
    return FALSE;
  }

  if (!replay_prepare_next_chunk() || strcmp(replay.current_chunk, "SNAP") != 0) {
    log_error("Replay '%s' is missing a SNAP chunk.", replay.path);
    replay_close();
    return FALSE;
  }

  if (!replay_scan_totals()) {
    log_error("Replay '%s' totals scan failed.", replay.path);
    replay_close();
    return FALSE;
  }

  memset(&replay.conn, 0, sizeof(replay.conn));
  connection_common_init(&replay.conn);
  replay.conn.self = conn_list_new();
  conn_list_append(replay.conn.self, &replay.conn);
  replay.conn.used = TRUE;
  replay.conn.established = TRUE;
  replay.conn.observer = TRUE;
  replay.conn.access_level = ALLOW_INFO;
  replay.conn_init = TRUE;

  conn_set_capability(&replay.conn, replay.capability);
  post_receive_packet_server_join_reply(&replay.conn, &join_reply);

  return TRUE;
}

static bool replay_load_from_path(void)
{
  if (client_state() != C_S_DISCONNECTED) {
    log_error("Replay playback can only start from disconnected client state.");
    return FALSE;
  }

  if (!replay_open_and_parse()) {
    return FALSE;
  }

  client.conn.established = TRUE;
  client.conn.observer = TRUE;
  client.conn.id = -1;
  client.conn.playing = NULL;
  client.conn.access_level = ALLOW_INFO;
  if (replay.capability != NULL) {
    sz_strlcpy(client.conn.capability, replay.capability);
  }

  set_client_state(C_S_PREPARING);
  client.conn.established = TRUE;
  client.conn.observer = TRUE;
  client.conn.id = -1;
  client.conn.playing = NULL;
  client.conn.access_level = ALLOW_INFO;
  set_client_page(PAGE_GAME + 1);

  while (strcmp(replay.current_chunk, "SNAP") == 0 && replay_step_frame()) {
    /* Step snapshot frames through the normal client packet pipeline. */
  }

  set_client_page(PAGE_GAME);
  replay.initial_turn = game.info.turn;

  if (strcmp(replay.current_chunk, "EVNT") == 0) {
    replay.active = TRUE;
    replay.paused = replay.start_paused;
    log_normal("Replay snapshot loaded at turn %d, year %d (%d snapshot frames).",
               game.info.turn, game.info.year, replay.snapshot_frames);
  } else {
    replay_finish();
  }

  log_normal("Loaded replay '%s'.", replay.path);
  return TRUE;
}

static bool replay_restart_at_turn(int target_turn)
{
  int speed = replay.speed;
  bool start_paused = replay.start_paused;

  replay.active = FALSE;
  replay_close();

  if (client_state() != C_S_DISCONNECTED) {
    set_client_state(C_S_DISCONNECTED);
  }

  replay.speed = speed;
  replay.start_paused = TRUE;
  if (!replay_load_from_path()) {
    replay.start_paused = start_paused;
    return FALSE;
  }

  replay.speed = speed;
  replay.start_paused = start_paused;
  replay.paused = TRUE;

  while (replay.active && game.info.turn < target_turn) {
    if (!replay_step_turn_forward_internal()) {
      return FALSE;
    }
  }

  return TRUE;
}

void client_replay_set_file(char *filename)
{
  FC_FREE(replay.path);
  replay.path = filename;
}

void client_replay_stop_mode(void)
{
  replay.active = FALSE;
  replay_close();
  FC_FREE(replay.path);
  replay.path = NULL;
}

void client_replay_set_start_paused(bool paused)
{
  replay.start_paused = paused;
}

void client_replay_set_startup_steps(int steps)
{
  replay.startup_steps = MAX(0, steps);
}

bool client_replay_set_speed_name(const char *name)
{
  if (!fc_strcasecmp(name, "slow")) {
    replay.speed = 0;
  } else if (!fc_strcasecmp(name, "normal")) {
    replay.speed = 1;
  } else if (!fc_strcasecmp(name, "fast")) {
    replay.speed = 2;
  } else {
    return FALSE;
  }

  return TRUE;
}

void client_replay_set_speed_level(int level)
{
  if (level < 0 || level > 2) {
    return;
  }

  replay.speed = level;

  if (replay.active) {
    log_normal("Replay speed set to %s.", replay_speed_name(replay.speed));
  }
}

void client_replay_toggle_pause(void)
{
  if (!replay.active) {
    return;
  }

  replay.paused = !replay.paused;
  log_normal("Replay %s.", replay.paused ? "paused" : "resumed");
}

static bool replay_step_turn_forward_internal(void)
{
  int current_turn;

  if (!replay.active) {
    return FALSE;
  }

  current_turn = game.info.turn;

  while (replay.active && game.info.turn == current_turn) {
    if (!replay_advance_frame()) {
      return FALSE;
    }
  }

  return TRUE;
}

void client_replay_step_backward(void)
{
  int target_turn;

  if (!replay.active) {
    return;
  }

  if (!replay.paused) {
    replay.paused = TRUE;
    log_normal("Replay paused for single-step.");
  }

  target_turn = MAX(replay.initial_turn, game.info.turn - 1);

  if (!replay_restart_at_turn(target_turn)) {
    return;
  }

  log_normal("Replay stepped backward to turn %d, year %d.",
             game.info.turn, game.info.year);
}

void client_replay_step_forward(void)
{
  if (!replay.active) {
    return;
  }

  if (!replay.paused) {
    replay.paused = TRUE;
    log_normal("Replay paused for single-step.");
  }

  if (!replay_step_turn_forward_internal()) {
    return;
  }

  log_normal("Replay stepped to turn %d, year %d.",
             game.info.turn, game.info.year);
}

bool client_replay_requested(void)
{
  return replay.path != NULL;
}

bool client_replay_mode(void)
{
  return replay.path != NULL;
}

bool client_replay_active(void)
{
  return replay.active;
}

bool client_replay_paused(void)
{
  return replay.paused;
}

int client_replay_timer_interval_ms(void)
{
  static const int intervals[] = { 250, 50, 5 };

  if (replay.paused) {
    return 50;
  }

  return intervals[replay.speed];
}

int client_replay_speed_level(void)
{
  return replay.speed;
}

int client_replay_position(void)
{
  return replay.event_frames;
}

int client_replay_length(void)
{
  return replay.total_event_frames;
}

bool client_replay_step(void)
{
  if (!replay.active) {
    return FALSE;
  }

  if (replay.paused) {
    return TRUE;
  }

  return replay_advance_frame();
}

bool client_replay_start_requested(void)
{
  if (!client_replay_requested()) {
    return FALSE;
  }

  if (!replay_load_from_path()) {
    return FALSE;
  }

  {
    int stepped = 0;

    while (replay.active && replay.startup_steps > 0) {
      replay.paused = FALSE;
      if (!client_replay_step()) {
        break;
      }
      replay.startup_steps--;
      stepped++;
    }

    if (stepped > 0) {
      log_normal("Replay applied %d startup step(s); now at turn %d, year %d.",
                 stepped, game.info.turn, game.info.year);
    }
  }

  replay.paused = replay.start_paused;
  return TRUE;
}
