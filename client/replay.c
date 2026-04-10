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

#include "replay.h"

struct client_replay {
  FILE *file;
  char *path;
  char *capability;
  struct connection conn;
  bool conn_init;
  uint32_t current_chunk_remaining;
  char current_chunk[5];
  int snapshot_frames;
  int event_frames;
};

static struct client_replay replay;

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
}

static bool replay_open_and_parse(void)
{
  char magic[8];
  uint16_t version;
  uint16_t flags;
  struct packet_server_join_reply join_reply = {
    .you_can_join = TRUE
  };

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

void client_replay_set_file(char *filename)
{
  FC_FREE(replay.path);
  replay.path = filename;
}

bool client_replay_requested(void)
{
  return replay.path != NULL;
}

bool client_replay_start_requested(void)
{
  if (!client_replay_requested()) {
    return FALSE;
  }

  if (client_state() != C_S_DISCONNECTED) {
    log_error("Replay playback can only start from disconnected client state.");
    return FALSE;
  }

  if (!replay_open_and_parse()) {
    return FALSE;
  }

  client.conn.established = TRUE;
  client.conn.observer = TRUE;
  client.conn.playing = NULL;
  client.conn.access_level = ALLOW_INFO;
  if (replay.capability != NULL) {
    sz_strlcpy(client.conn.capability, replay.capability);
  }

  set_client_state(C_S_PREPARING);

  while (strcmp(replay.current_chunk, "SNAP") == 0 && replay_step_frame()) {
    /* Step snapshot frames through the normal client packet pipeline. */
  }

  while (strcmp(replay.current_chunk, "EVNT") == 0 && replay_step_frame()) {
    /* Step event frames through the normal client packet pipeline. */
  }

  log_normal("Loaded replay '%s' (%d snapshot frames, %d event frames).",
             replay.path, replay.snapshot_frames, replay.event_frames);

  replay_close();
  return TRUE;
}
