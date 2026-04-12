#ifdef HAVE_CONFIG_H
#include <fc_config.h>
#endif

#include "fc_prehdrs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef FREECIV_HAVE_LIBZ
#include <zlib.h>
#endif

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
  unsigned char *data;
  size_t data_len;
  size_t data_pos;
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
  int final_turn;
  int snapshot_frames;
  int event_frames;
  int total_event_frames;
};

struct replay_data_cursor {
  const unsigned char *data;
  size_t data_len;
  size_t data_pos;
};

struct replay_preview_state {
  int width;
  int height;
  int terrain_colors_count;
  unsigned char *terrain_colors;
  int *terrain_ids;
};

static struct client_replay replay;

static void replay_close(void);
static bool replay_load_from_path(void);
static bool replay_load_bytes(void);
static bool replay_append_loaded_bytes(unsigned char **buffer, size_t *used,
                                       size_t *capacity,
                                       const unsigned char *src, size_t len);
static bool replay_buffer_append(struct connection *pconn,
                                 const unsigned char *data, size_t len);
static bool replay_scan_totals(void);
static bool replay_step_frame(void);
static bool replay_step_turn_forward_internal(void);
static bool replay_scan_turn_limits(void);
static bool replay_cursor_read_bytes(struct replay_data_cursor *cursor,
                                     void *dst, size_t size);
static bool replay_cursor_read_u32(struct replay_data_cursor *cursor,
                                   uint32_t *value);

static void replay_preview_state_init(struct replay_preview_state *state)
{
  memset(state, 0, sizeof(*state));
}

static void replay_preview_state_free(struct replay_preview_state *state)
{
  FC_FREE(state->terrain_colors);
  FC_FREE(state->terrain_ids);
  memset(state, 0, sizeof(*state));
}

static void client_replay_preview_reset(struct client_replay_preview *preview)
{
  if (preview == NULL) {
    return;
  }

  FC_FREE(preview->rgb);
  memset(preview, 0, sizeof(*preview));
}

static bool replay_preview_ensure_terrain_color(struct replay_preview_state *state,
                                                int terrain_id)
{
  int new_count;
  unsigned char *new_colors;

  if (terrain_id < 0) {
    return FALSE;
  }

  if (terrain_id < state->terrain_colors_count) {
    return TRUE;
  }

  new_count = MAX(terrain_id + 1, state->terrain_colors_count * 2 + 8);
  new_colors = fc_calloc(new_count, 3 * sizeof(*new_colors));
  if (state->terrain_colors != NULL) {
    memcpy(new_colors, state->terrain_colors,
           state->terrain_colors_count * 3 * sizeof(*new_colors));
    FC_FREE(state->terrain_colors);
  }

  state->terrain_colors = new_colors;
  state->terrain_colors_count = new_count;
  return TRUE;
}

static bool replay_preview_init_map(struct replay_preview_state *state,
                                    int width, int height)
{
  size_t count;

  if (width <= 0 || height <= 0) {
    return FALSE;
  }

  count = (size_t) width * height;
  FC_FREE(state->terrain_ids);
  state->terrain_ids = fc_malloc(count * sizeof(*state->terrain_ids));
  for (size_t i = 0; i < count; i++) {
    state->terrain_ids[i] = -1;
  }

  state->width = width;
  state->height = height;
  return TRUE;
}

static bool replay_cursor_read_chunk_header(struct replay_data_cursor *cursor,
                                            char chunk_type[5],
                                            uint32_t *chunk_remaining)
{
  if (!replay_cursor_read_bytes(cursor, chunk_type, 4)
      || !replay_cursor_read_u32(cursor, chunk_remaining)) {
    return FALSE;
  }

  chunk_type[4] = '\0';
  return TRUE;
}

static bool replay_preview_handle_packet(enum packet_type type, void *packet,
                                         struct replay_preview_state *state)
{
  switch (type) {
  case PACKET_RULESET_CONTROL:
    game.control = *((const struct packet_ruleset_control *) packet);
    break;
  case PACKET_RULESET_TERRAIN:
    {
      const struct packet_ruleset_terrain *terrain = packet;

      if (!replay_preview_ensure_terrain_color(state, terrain->id)) {
        return FALSE;
      }

      state->terrain_colors[terrain->id * 3 + 0] = terrain->color_red;
      state->terrain_colors[terrain->id * 3 + 1] = terrain->color_green;
      state->terrain_colors[terrain->id * 3 + 2] = terrain->color_blue;
    }
    break;
  case PACKET_MAP_INFO:
    {
      const struct packet_map_info *map_info = packet;

      if (!replay_preview_init_map(state, map_info->xsize, map_info->ysize)) {
        return FALSE;
      }
    }
    break;
  case PACKET_TILE_INFO:
    {
      const struct packet_tile_info *tile_info = packet;
      int tile_index = tile_info->tile;

      if (state->terrain_ids != NULL
          && tile_index >= 0
          && tile_index < state->width * state->height) {
        state->terrain_ids[tile_index] = tile_info->terrain;
      }
    }
    break;
  default:
    break;
  }

  return TRUE;
}

static bool replay_preview_inject_frame(struct connection *conn,
                                        const unsigned char *frame,
                                        uint32_t len,
                                        struct replay_preview_state *state)
{
  if (!replay_buffer_append(conn, frame, len)) {
    return FALSE;
  }

  while (conn->used) {
    enum packet_type type;
    void *packet = get_packet_from_connection(conn, &type, FALSE);

    if (packet == NULL) {
      break;
    }

    if (!replay_preview_handle_packet(type, packet, state)) {
      packet_destroy(packet, type);
      return FALSE;
    }

    packet_destroy(packet, type);
  }

  return conn->used;
}

static bool replay_build_preview_image(const struct replay_preview_state *state,
                                       struct client_replay_preview *preview)
{
  size_t pixel_count;
  size_t idx;

  if (state->width <= 0 || state->height <= 0 || state->terrain_ids == NULL) {
    return FALSE;
  }

  pixel_count = (size_t) state->width * state->height;
  preview->rgb = fc_malloc(pixel_count * 3 * sizeof(*preview->rgb));

  for (idx = 0; idx < pixel_count; idx++) {
    int terrain_id = state->terrain_ids[idx];
    unsigned char *pixel = preview->rgb + idx * 3;

    if (terrain_id >= 0 && terrain_id < state->terrain_colors_count) {
      pixel[0] = state->terrain_colors[terrain_id * 3 + 0];
      pixel[1] = state->terrain_colors[terrain_id * 3 + 1];
      pixel[2] = state->terrain_colors[terrain_id * 3 + 2];
    } else {
      pixel[0] = 0;
      pixel[1] = 0;
      pixel[2] = 0;
    }
  }

  preview->valid = TRUE;
  preview->width = state->width;
  preview->height = state->height;
  return TRUE;
}

static int replay_speed_numerator(int speed)
{
  static const int numerators[] = { 1, 1, 2, 4, 8 };

  return numerators[speed];
}

static int replay_speed_denominator(int speed)
{
  static const int denominators[] = { 2, 1, 1, 1, 1 };

  return denominators[speed];
}

static bool replay_cursor_read_bytes(struct replay_data_cursor *cursor,
                                     void *dst, size_t size)
{
  if (cursor->data_pos + size > cursor->data_len) {
    return FALSE;
  }

  memcpy(dst, cursor->data + cursor->data_pos, size);
  cursor->data_pos += size;
  return TRUE;
}

static bool replay_cursor_read_u16(struct replay_data_cursor *cursor,
                                   uint16_t *value)
{
  unsigned char buf[2];

  if (!replay_cursor_read_bytes(cursor, buf, sizeof(buf))) {
    return FALSE;
  }

  *value = buf[0] | (buf[1] << 8);
  return TRUE;
}

static bool replay_cursor_read_u32(struct replay_data_cursor *cursor,
                                   uint32_t *value)
{
  unsigned char buf[4];

  if (!replay_cursor_read_bytes(cursor, buf, sizeof(buf))) {
    return FALSE;
  }

  *value = buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((uint32_t) buf[3] << 24);
  return TRUE;
}

static bool replay_cursor_read_u64(struct replay_data_cursor *cursor,
                                   uint64_t *value)
{
  unsigned char buf[8];
  int i;

  if (!replay_cursor_read_bytes(cursor, buf, sizeof(buf))) {
    return FALSE;
  }

  *value = 0;
  for (i = 0; i < 8; i++) {
    *value |= ((uint64_t) buf[i]) << (8 * i);
  }

  return TRUE;
}

static bool replay_cursor_read_string(struct replay_data_cursor *cursor,
                                      char **value)
{
  uint16_t len;

  *value = NULL;
  if (!replay_cursor_read_u16(cursor, &len)) {
    return FALSE;
  }

  *value = fc_malloc(len + 1);
  if (len > 0 && !replay_cursor_read_bytes(cursor, *value, len)) {
    FC_FREE(*value);
    return FALSE;
  }

  (*value)[len] = '\0';
  return TRUE;
}

static bool replay_load_plain_file_bytes(const char *path,
                                         unsigned char **data,
                                         size_t *data_len)
{
  FILE *file;
  unsigned char buffer[8192];
  unsigned char *loaded = NULL;
  size_t used = 0;
  size_t capacity = 0;

  file = fc_fopen(path, "rb");
  if (file == NULL) {
    return FALSE;
  }

  while (!feof(file)) {
    size_t nread = fread(buffer, 1, sizeof(buffer), file);

    if (nread > 0) {
      replay_append_loaded_bytes(&loaded, &used, &capacity, buffer, nread);
    }

    if (ferror(file)) {
      FC_FREE(loaded);
      fclose(file);
      return FALSE;
    }
  }

  fclose(file);
  *data = loaded;
  *data_len = used;
  return TRUE;
}

#ifdef FREECIV_HAVE_LIBZ
static bool replay_load_gzip_file_bytes(const char *path,
                                        unsigned char **data,
                                        size_t *data_len)
{
  gzFile file;
  unsigned char buffer[8192];
  unsigned char *loaded = NULL;
  size_t used = 0;
  size_t capacity = 0;

  file = fc_gzopen(path, "rb");
  if (file == NULL) {
    return FALSE;
  }

  while (TRUE) {
    int nread = fc_gzread(file, buffer, sizeof(buffer));

    if (nread > 0) {
      replay_append_loaded_bytes(&loaded, &used, &capacity, buffer, nread);
      continue;
    }

    if (nread < 0) {
      FC_FREE(loaded);
      fc_gzclose(file);
      return FALSE;
    }

    break;
  }

  fc_gzclose(file);
  *data = loaded;
  *data_len = used;
  return TRUE;
}
#endif /* FREECIV_HAVE_LIBZ */

static bool replay_load_file_bytes(const char *path, unsigned char **data,
                                   size_t *data_len)
{
  FILE *probe;
  unsigned char magic[2];
  size_t got;

  *data = NULL;
  *data_len = 0;

  probe = fc_fopen(path, "rb");
  if (probe == NULL) {
    return FALSE;
  }

  got = fread(magic, 1, sizeof(magic), probe);
  fclose(probe);

#ifdef FREECIV_HAVE_LIBZ
  if (got == sizeof(magic) && magic[0] == 0x1f && magic[1] == 0x8b) {
    return replay_load_gzip_file_bytes(path, data, data_len);
  }
#endif /* FREECIV_HAVE_LIBZ */

  return replay_load_plain_file_bytes(path, data, data_len);
}

static bool replay_parse_info_data(const unsigned char *data, size_t data_len,
                                   struct client_replay_info *info)
{
  struct replay_data_cursor cursor = {
    .data = data,
    .data_len = data_len,
    .data_pos = 0
  };
  char magic[8];
  char chunk_type[5];
  char *version = NULL;
  char *capability = NULL;
  char *ruleset = NULL;
  char *scenario = NULL;
  uint16_t format_version;
  uint16_t flags;
  uint32_t chunk_size;
  uint32_t turn;
  uint32_t year;
  uint64_t timestamp;

  memset(info, 0, sizeof(*info));
  sz_strlcpy(info->ruleset, "-");
  sz_strlcpy(info->scenario, "-");

  if (!replay_cursor_read_bytes(&cursor, magic, sizeof(magic))
      || memcmp(magic, "FCREPLAY", sizeof(magic)) != 0
      || !replay_cursor_read_u16(&cursor, &format_version)
      || !replay_cursor_read_u16(&cursor, &flags)) {
    return FALSE;
  }

  if (format_version != 1) {
    return FALSE;
  }

  if (!replay_cursor_read_bytes(&cursor, chunk_type, 4)
      || !replay_cursor_read_u32(&cursor, &chunk_size)) {
    return FALSE;
  }

  chunk_type[4] = '\0';
  if (strcmp(chunk_type, "INFO") != 0) {
    return FALSE;
  }

  if (!replay_cursor_read_string(&cursor, &version)
      || !replay_cursor_read_string(&cursor, &capability)
      || !replay_cursor_read_string(&cursor, &ruleset)
      || !replay_cursor_read_string(&cursor, &scenario)
      || !replay_cursor_read_u32(&cursor, &turn)
      || !replay_cursor_read_u32(&cursor, &year)
      || !replay_cursor_read_u64(&cursor, &timestamp)) {
    FC_FREE(version);
    FC_FREE(capability);
    FC_FREE(ruleset);
    FC_FREE(scenario);
    return FALSE;
  }

  info->valid = TRUE;
  sz_strlcpy(info->ruleset, ruleset != NULL && ruleset[0] != '\0' ? ruleset : "-");
  sz_strlcpy(info->scenario, scenario != NULL && scenario[0] != '\0' ? scenario : "-");
  info->start_turn = turn;
  info->start_year = year;

  FC_FREE(version);
  FC_FREE(capability);
  FC_FREE(ruleset);
  FC_FREE(scenario);
  return TRUE;
}

static const char *replay_speed_name(int speed)
{
  switch (speed) {
  case 0:
    return "0.5x";
  case 1:
    return "1x";
  case 2:
    return "2x";
  case 3:
    return "4x";
  case 4:
    return "8x";
  default:
    return "1x";
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
  if (replay.data_pos + size > replay.data_len) {
    return FALSE;
  }

  memcpy(dst, replay.data + replay.data_pos, size);
  replay.data_pos += size;
  return TRUE;
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
  if (replay.data_pos + size > replay.data_len) {
    return FALSE;
  }

  replay.data_pos += size;
  return TRUE;
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

  replay.initial_turn = turn;
  replay.final_turn = turn;

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
  size_t saved_offset = replay.data_pos;
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

  replay.data_pos = saved_offset;
  replay.current_chunk_remaining = saved_remaining;
  sz_strlcpy(replay.current_chunk, saved_chunk);
  return TRUE;

fail:
  replay.data_pos = saved_offset;
  replay.current_chunk_remaining = saved_remaining;
  sz_strlcpy(replay.current_chunk, saved_chunk);
  return FALSE;
}

static bool replay_scan_turn_limits(void)
{
  struct replay_data_cursor cursor = {
    .data = replay.data,
    .data_len = replay.data_len,
    .data_pos = 0
  };
  struct connection conn;
  struct packet_server_join_reply join_reply = {
    .you_can_join = TRUE
  };
  char magic[8];
  char chunk_type[5];
  char *version = NULL;
  char *capability = NULL;
  char *ruleset = NULL;
  char *scenario = NULL;
  uint16_t format_version;
  uint16_t flags;
  uint32_t chunk_remaining;
  uint32_t frame_len;
  uint32_t turn;
  uint32_t year;
  uint64_t timestamp;
  struct packet_ruleset_control saved_control = game.control;
  int latest_turn = replay.initial_turn;
  bool ok = FALSE;

  memset(&conn, 0, sizeof(conn));

  if (!replay_cursor_read_bytes(&cursor, magic, sizeof(magic))
      || memcmp(magic, "FCREPLAY", sizeof(magic)) != 0
      || !replay_cursor_read_u16(&cursor, &format_version)
      || !replay_cursor_read_u16(&cursor, &flags)
      || format_version != 1
      || !replay_cursor_read_chunk_header(&cursor, chunk_type, &chunk_remaining)
      || strcmp(chunk_type, "INFO") != 0
      || !replay_cursor_read_string(&cursor, &version)
      || !replay_cursor_read_string(&cursor, &capability)
      || !replay_cursor_read_string(&cursor, &ruleset)
      || !replay_cursor_read_string(&cursor, &scenario)
      || !replay_cursor_read_u32(&cursor, &turn)
      || !replay_cursor_read_u32(&cursor, &year)
      || !replay_cursor_read_u64(&cursor, &timestamp)) {
    goto cleanup;
  }

  latest_turn = turn;

  connection_common_init(&conn);
  conn.self = conn_list_new();
  conn_list_append(conn.self, &conn);
  conn.used = TRUE;
  conn.established = TRUE;
  conn.observer = TRUE;
  conn.access_level = ALLOW_INFO;
  conn_set_capability(&conn, capability);
  post_receive_packet_server_join_reply(&conn, &join_reply);

  while (cursor.data_pos < cursor.data_len) {
    if (!replay_cursor_read_chunk_header(&cursor, chunk_type, &chunk_remaining)) {
      goto cleanup;
    }

    if (strcmp(chunk_type, "DONE") == 0) {
      ok = TRUE;
      break;
    }

    while (chunk_remaining > 0) {
      unsigned char *frame;

      if (chunk_remaining < 4 || !replay_cursor_read_u32(&cursor, &frame_len)) {
        goto cleanup;
      }
      chunk_remaining -= 4;

      if (frame_len > chunk_remaining || cursor.data_pos + frame_len > cursor.data_len) {
        goto cleanup;
      }

      frame = fc_malloc(frame_len);
      if (!replay_cursor_read_bytes(&cursor, frame, frame_len)) {
        FC_FREE(frame);
        goto cleanup;
      }

      if (!replay_buffer_append(&conn, frame, frame_len)) {
        FC_FREE(frame);
        goto cleanup;
      }
      FC_FREE(frame);

      while (conn.used) {
        enum packet_type type;
        void *packet = get_packet_from_connection(&conn, &type, FALSE);

        if (packet == NULL) {
          break;
        }

        if (type == PACKET_RULESET_CONTROL) {
          game.control = *((const struct packet_ruleset_control *) packet);
        } else if (type == PACKET_GAME_INFO) {
          latest_turn = ((const struct packet_game_info *) packet)->turn;
        }

        packet_destroy(packet, type);
      }

      chunk_remaining -= frame_len;
    }
  }

cleanup:
  FC_FREE(version);
  FC_FREE(capability);
  FC_FREE(ruleset);
  FC_FREE(scenario);
  if (conn.self != NULL) {
    connection_common_close(&conn);
    conn_list_destroy(conn.self);
  }
  if (ok) {
    replay.final_turn = MAX(replay.initial_turn, latest_turn);
  }
  game.control = saved_control;

  return ok;
}

static bool replay_append_loaded_bytes(unsigned char **buffer, size_t *used,
                                       size_t *capacity,
                                       const unsigned char *src, size_t len)
{
  size_t needed = *used + len;

  if (needed > *capacity) {
    size_t new_capacity = MAX(needed, *capacity * 2 + 8192);

    *buffer = fc_realloc(*buffer, new_capacity);
    *capacity = new_capacity;
  }

  memcpy(*buffer + *used, src, len);
  *used += len;
  return TRUE;
}

static bool replay_load_plain_bytes(void)
{
  FILE *file;
  unsigned char buffer[8192];
  unsigned char *data = NULL;
  size_t used = 0;
  size_t capacity = 0;

  file = fc_fopen(replay.path, "rb");
  if (file == NULL) {
    log_error("Failed opening replay '%s': %s", replay.path, strerror(errno));
    return FALSE;
  }

  while (!feof(file)) {
    size_t nread = fread(buffer, 1, sizeof(buffer), file);

    if (nread > 0) {
      replay_append_loaded_bytes(&data, &used, &capacity, buffer, nread);
    }

    if (ferror(file)) {
      log_error("Failed reading replay '%s': %s", replay.path, strerror(errno));
      free(data);
      fclose(file);
      return FALSE;
    }
  }

  fclose(file);
  replay.data = data;
  replay.data_len = used;
  replay.data_pos = 0;
  return TRUE;
}

#ifdef FREECIV_HAVE_LIBZ
static bool replay_load_gzip_bytes(void)
{
  gzFile file;
  unsigned char buffer[8192];
  unsigned char *data = NULL;
  size_t used = 0;
  size_t capacity = 0;

  file = fc_gzopen(replay.path, "rb");
  if (file == NULL) {
    log_error("Failed opening compressed replay '%s'.", replay.path);
    return FALSE;
  }

  while (TRUE) {
    int nread = fc_gzread(file, buffer, sizeof(buffer));

    if (nread > 0) {
      replay_append_loaded_bytes(&data, &used, &capacity, buffer, nread);
      continue;
    }

    if (nread < 0) {
      int errnum;
      const char *errmsg = fc_gzerror(file, &errnum);

      if (errnum != Z_OK && errnum != Z_STREAM_END) {
        log_error("Failed reading compressed replay '%s': %s",
                  replay.path, errmsg);
        free(data);
        fc_gzclose(file);
        return FALSE;
      }
    }

    break;
  }

  fc_gzclose(file);
  replay.data = data;
  replay.data_len = used;
  replay.data_pos = 0;
  return TRUE;
}
#endif /* FREECIV_HAVE_LIBZ */

static bool replay_load_bytes(void)
{
  FILE *probe;
  unsigned char magic[2];
  size_t got;

  probe = fc_fopen(replay.path, "rb");
  if (probe == NULL) {
    log_error("Failed opening replay '%s': %s", replay.path, strerror(errno));
    return FALSE;
  }

  got = fread(magic, 1, sizeof(magic), probe);
  fclose(probe);

#ifdef FREECIV_HAVE_LIBZ
  if (got == sizeof(magic) && magic[0] == 0x1f && magic[1] == 0x8b) {
    return replay_load_gzip_bytes();
  }
#endif /* FREECIV_HAVE_LIBZ */

  return replay_load_plain_bytes();
}

static void replay_close(void)
{
  if (replay.conn_init) {
    connection_common_close(&replay.conn);
    conn_list_destroy(replay.conn.self);
    replay.conn.self = NULL;
    replay.conn_init = FALSE;
  }

  FC_FREE(replay.data);
  replay.data = NULL;
  replay.data_len = 0;
  replay.data_pos = 0;

  FC_FREE(replay.capability);
  replay.capability = NULL;
  replay.current_chunk_remaining = 0;
  replay.current_chunk[0] = '\0';
  replay.paused = FALSE;
  replay.speed = 1;
  replay.startup_steps = 0;
  replay.initial_turn = 0;
  replay.final_turn = 0;
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
  replay.final_turn = 0;
  replay.snapshot_frames = 0;
  replay.event_frames = 0;
  replay.total_event_frames = 0;

  if (!replay_load_bytes()) {
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

  if (!replay_scan_turn_limits()) {
    log_error("Replay '%s' turn scan failed.", replay.path);
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

bool client_replay_read_info(const char *filename,
                             struct client_replay_info *info)
{
  unsigned char *data = NULL;
  size_t data_len = 0;
  bool ok;

  fc_assert_ret_val(filename != NULL, FALSE);
  fc_assert_ret_val(info != NULL, FALSE);

  ok = replay_load_file_bytes(filename, &data, &data_len)
       && replay_parse_info_data(data, data_len, info);
  FC_FREE(data);

  if (!ok) {
    memset(info, 0, sizeof(*info));
    sz_strlcpy(info->ruleset, "-");
    sz_strlcpy(info->scenario, "-");
  }

  return ok;
}

bool client_replay_read_preview(const char *filename,
                                struct client_replay_preview *preview)
{
  unsigned char *data = NULL;
  size_t data_len = 0;
  struct replay_data_cursor cursor;
  struct replay_preview_state state;
  struct connection conn;
  struct packet_server_join_reply join_reply = {
    .you_can_join = TRUE
  };
  char magic[8];
  char chunk_type[5];
  char *version = NULL;
  char *capability = NULL;
  char *ruleset = NULL;
  char *scenario = NULL;
  uint16_t format_version;
  uint16_t flags;
  uint32_t chunk_remaining;
  uint32_t frame_len;
  uint32_t turn;
  uint32_t year;
  uint64_t timestamp;
  struct packet_ruleset_control saved_control = game.control;
  bool ok = FALSE;

  fc_assert_ret_val(filename != NULL, FALSE);
  fc_assert_ret_val(preview != NULL, FALSE);
  client_replay_preview_reset(preview);
  replay_preview_state_init(&state);
  memset(&conn, 0, sizeof(conn));

  if (!replay_load_file_bytes(filename, &data, &data_len)) {
    return FALSE;
  }

  cursor.data = data;
  cursor.data_len = data_len;
  cursor.data_pos = 0;

  if (!replay_cursor_read_bytes(&cursor, magic, sizeof(magic))
      || memcmp(magic, "FCREPLAY", sizeof(magic)) != 0
      || !replay_cursor_read_u16(&cursor, &format_version)
      || !replay_cursor_read_u16(&cursor, &flags)
      || format_version != 1
      || !replay_cursor_read_chunk_header(&cursor, chunk_type, &chunk_remaining)
      || strcmp(chunk_type, "INFO") != 0
      || !replay_cursor_read_string(&cursor, &version)
      || !replay_cursor_read_string(&cursor, &capability)
      || !replay_cursor_read_string(&cursor, &ruleset)
      || !replay_cursor_read_string(&cursor, &scenario)
      || !replay_cursor_read_u32(&cursor, &turn)
      || !replay_cursor_read_u32(&cursor, &year)
      || !replay_cursor_read_u64(&cursor, &timestamp)) {
    goto cleanup;
  }

  while (cursor.data_pos < cursor.data_len) {
    if (!replay_cursor_read_chunk_header(&cursor, chunk_type, &chunk_remaining)) {
      goto cleanup;
    }

    if (strcmp(chunk_type, "SNAP") == 0) {
      break;
    }

    if (cursor.data_pos + chunk_remaining > cursor.data_len) {
      goto cleanup;
    }
    cursor.data_pos += chunk_remaining;
  }

  if (strcmp(chunk_type, "SNAP") != 0) {
    goto cleanup;
  }

  connection_common_init(&conn);
  conn.self = conn_list_new();
  conn_list_append(conn.self, &conn);
  conn.used = TRUE;
  conn.established = TRUE;
  conn.observer = TRUE;
  conn.access_level = ALLOW_INFO;
  conn_set_capability(&conn, capability);
  post_receive_packet_server_join_reply(&conn, &join_reply);

  while (chunk_remaining > 0) {
    unsigned char *frame;

    if (chunk_remaining < 4 || !replay_cursor_read_u32(&cursor, &frame_len)) {
      goto cleanup;
    }
    chunk_remaining -= 4;

    if (frame_len > chunk_remaining || cursor.data_pos + frame_len > cursor.data_len) {
      goto cleanup;
    }

    frame = fc_malloc(frame_len);
    if (!replay_cursor_read_bytes(&cursor, frame, frame_len)) {
      FC_FREE(frame);
      goto cleanup;
    }

    if (!replay_preview_inject_frame(&conn, frame, frame_len, &state)) {
      FC_FREE(frame);
      goto cleanup;
    }

    FC_FREE(frame);
    chunk_remaining -= frame_len;
  }

  ok = replay_build_preview_image(&state, preview);

cleanup:
  FC_FREE(version);
  FC_FREE(capability);
  FC_FREE(ruleset);
  FC_FREE(scenario);
  FC_FREE(data);
  replay_preview_state_free(&state);
  if (conn.self != NULL) {
    connection_common_close(&conn);
    conn_list_destroy(conn.self);
  }
  if (!ok) {
    client_replay_preview_reset(preview);
  }
  game.control = saved_control;

  return ok;
}

void client_replay_free_preview(struct client_replay_preview *preview)
{
  client_replay_preview_reset(preview);
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
  if (!fc_strcasecmp(name, "0.5x") || !fc_strcasecmp(name, "slow")) {
    replay.speed = 0;
  } else if (!fc_strcasecmp(name, "1x") || !fc_strcasecmp(name, "normal")) {
    replay.speed = 1;
  } else if (!fc_strcasecmp(name, "2x")) {
    replay.speed = 2;
  } else if (!fc_strcasecmp(name, "4x") || !fc_strcasecmp(name, "fast")) {
    replay.speed = 3;
  } else if (!fc_strcasecmp(name, "8x")) {
    replay.speed = 4;
  } else {
    return FALSE;
  }

  return TRUE;
}

void client_replay_set_speed_level(int level)
{
  if (level < 0 || level > 4) {
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
  static const int base_interval = 200;

  if (replay.paused) {
    return 50;
  }

  return MAX(1, base_interval * replay_speed_denominator(replay.speed)
                / replay_speed_numerator(replay.speed));
}

int client_replay_speed_level(void)
{
  return replay.speed;
}

int client_replay_initial_turn(void)
{
  return replay.initial_turn;
}

int client_replay_final_turn(void)
{
  return replay.final_turn;
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

void client_replay_seek_turn(int turn)
{
  bool paused_before;
  int target_turn;

  if (!replay.active) {
    return;
  }

  paused_before = replay.paused;
  target_turn = CLIP(replay.initial_turn, turn, replay.final_turn);

  if (target_turn == game.info.turn) {
    return;
  }

  if (!replay_restart_at_turn(target_turn)) {
    return;
  }

  replay.paused = paused_before;
  log_normal("Replay jumped to turn %d, year %d.",
             game.info.turn, game.info.year);
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
