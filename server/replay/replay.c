#ifdef HAVE_CONFIG_H
#include <fc_config.h>
#endif

#include "fc_prehdrs.h"

#ifdef FREECIV_REPLAY_RECORDER

#ifdef FREECIV_HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef FREECIV_HAVE_LIBZ
#include <zlib.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

/* utility */
#include "astring.h"
#include "log.h"
#include "mem.h"
#include "netintf.h"
#include "support.h"

/* common */
#include "capstr.h"
#include "dataio.h"
#include "game.h"
#include "packets.h"
#include "player.h"
#include "version.h"

/* common/networking */
#include "connection.h"

#include "replay.h"

enum replay_chunk_phase {
  REPLAY_PHASE_NONE,
  REPLAY_PHASE_SNAPSHOT,
  REPLAY_PHASE_EVENTS
};

/* Mirror packets.c transport framing so replay records preserve exactly one
 * full transport frame, including compressed and jumbo-compressed frames. */
#define REPLAY_JUMBO_SIZE 0xffff
#define REPLAY_COMPRESSION_BORDER (16 * 1024 + 1)
#define REPLAY_INFO_PLAYERS_TEXT_LEN 512
#define REPLAY_INFO_RESULT_TEXT_LEN 128
#define REPLAY_INFO_WINNER_TEXT_LEN 128

struct replay_recorder_state {
  bool active;
  enum replay_chunk_phase phase;
  FILE *file;
  char *path;
  char *tmp_path;
  struct connection *conn;
  int peer_fd;
  long chunk_size_offset;
  unsigned char *pending;
  size_t pending_len;
  size_t pending_cap;
  time_t started_at;
  long info_final_turn_offset;
  long info_duration_offset;
  long info_players_offset;
  long info_result_offset;
  long info_winner_offset;
};

static struct replay_recorder_state replay_state = {
  .peer_fd = -1
};

static void replay_recorder_flush_peer(void);
static bool replay_compress_file(void);
static bool replay_write_bytes(const void *data, size_t size);
static bool replay_write_u32(uint32_t value);

static bool replay_write_fixed_string(const char *value, size_t size)
{
  char *buffer = fc_calloc(size, sizeof(*buffer));
  bool ok;

  if (value != NULL) {
    fc_strlcpy(buffer, value, size);
  }

  ok = replay_write_bytes(buffer, size);
  FC_FREE(buffer);
  return ok;
}

static bool replay_patch_u32(long offset, uint32_t value)
{
  long saved_pos;
  bool ok;

  if (offset <= 0) {
    return FALSE;
  }

  saved_pos = ftell(replay_state.file);
  if (saved_pos < 0 || fseek(replay_state.file, offset, SEEK_SET) != 0) {
    return FALSE;
  }

  ok = replay_write_u32(value);
  fseek(replay_state.file, saved_pos, SEEK_SET);
  return ok;
}

static bool replay_patch_fixed_string(long offset, size_t size, const char *value)
{
  long saved_pos;
  bool ok;

  if (offset <= 0) {
    return FALSE;
  }

  saved_pos = ftell(replay_state.file);
  if (saved_pos < 0 || fseek(replay_state.file, offset, SEEK_SET) != 0) {
    return FALSE;
  }

  ok = replay_write_fixed_string(value, size);
  fseek(replay_state.file, saved_pos, SEEK_SET);
  return ok;
}

static void replay_build_player_list(char *buffer, size_t buffer_size)
{
  bool first = TRUE;

  if (buffer_size == 0) {
    return;
  }

  buffer[0] = '\0';
  players_iterate(pplayer) {
    if (!first) {
      fc_strlcat(buffer, ", ", buffer_size);
    }
    fc_strlcat(buffer, player_name(pplayer), buffer_size);
    first = FALSE;
  } players_iterate_end;
}

static void replay_build_winner_list(char *buffer, size_t buffer_size)
{
  bool first = TRUE;

  if (buffer_size == 0) {
    return;
  }

  buffer[0] = '\0';
  players_iterate(pplayer) {
    if (!pplayer->is_winner) {
      continue;
    }

    if (!first) {
      fc_strlcat(buffer, ", ", buffer_size);
    }
    fc_strlcat(buffer, player_name(pplayer), buffer_size);
    first = FALSE;
  } players_iterate_end;
}

static const char *replay_result_text(void)
{
  players_iterate(pplayer) {
    if (pplayer->is_winner) {
      return "Victory";
    }
  } players_iterate_end;

  return "";
}

static void replay_notify_writable(struct connection *pc,
                                   bool data_available_and_socket_full)
{
  if (pc == replay_state.conn && replay_state.active) {
    replay_recorder_flush_peer();
  }
}

static bool replay_write_bytes(const void *data, size_t size)
{
  return fwrite(data, 1, size, replay_state.file) == size;
}

static bool replay_write_u16(uint16_t value)
{
  unsigned char buf[2];

  buf[0] = value & 0xff;
  buf[1] = (value >> 8) & 0xff;

  return replay_write_bytes(buf, sizeof(buf));
}

static bool replay_write_u32(uint32_t value)
{
  unsigned char buf[4];

  buf[0] = value & 0xff;
  buf[1] = (value >> 8) & 0xff;
  buf[2] = (value >> 16) & 0xff;
  buf[3] = (value >> 24) & 0xff;

  return replay_write_bytes(buf, sizeof(buf));
}

static bool replay_write_u64(uint64_t value)
{
  unsigned char buf[8];
  int i;

  for (i = 0; i < 8; i++) {
    buf[i] = (value >> (8 * i)) & 0xff;
  }

  return replay_write_bytes(buf, sizeof(buf));
}

static bool replay_write_string(const char *value)
{
  uint16_t len = value != nullptr ? strlen(value) : 0;

  return replay_write_u16(len)
         && (len == 0 || replay_write_bytes(value, len));
}

static bool replay_begin_chunk(const char type[4])
{
  replay_recorder_flush_peer();
  replay_state.chunk_size_offset = 0;

  if (!replay_write_bytes(type, 4)) {
    return FALSE;
  }

  replay_state.chunk_size_offset = ftell(replay_state.file);

  return replay_write_u32(0);
}

static void replay_finish_chunk(void)
{
  long end_pos;
  uint32_t size;

  if (replay_state.file == nullptr || replay_state.chunk_size_offset == 0) {
    return;
  }

  end_pos = ftell(replay_state.file);
  size = end_pos - (replay_state.chunk_size_offset + 4);

  fseek(replay_state.file, replay_state.chunk_size_offset, SEEK_SET);
  replay_write_u32(size);
  fseek(replay_state.file, end_pos, SEEK_SET);

  replay_state.chunk_size_offset = 0;
}

static void replay_queue_pending(const unsigned char *data, size_t len)
{
  size_t needed = replay_state.pending_len + len;

  if (needed > replay_state.pending_cap) {
    replay_state.pending_cap = MAX(needed, replay_state.pending_cap * 2 + 1024);
    replay_state.pending = fc_realloc(replay_state.pending, replay_state.pending_cap);
  }

  memcpy(replay_state.pending + replay_state.pending_len, data, len);
  replay_state.pending_len += len;
}

static bool replay_write_packet_record(const unsigned char *packet, size_t len)
{
  return replay_write_u32(len) && replay_write_bytes(packet, len);
}

static bool replay_compress_file(void)
{
  bool ok = TRUE;

  if (replay_state.tmp_path == NULL || replay_state.path == NULL) {
    return FALSE;
  }

#ifdef FREECIV_HAVE_LIBZ
  {
    FILE *src = fc_fopen(replay_state.tmp_path, "rb");
    gzFile dst;
    unsigned char buf[8192];

    if (src == NULL) {
      log_error("Replay recorder failed opening temporary file '%s': %s",
                replay_state.tmp_path, strerror(errno));
      return FALSE;
    }

    dst = fc_gzopen(replay_state.path, "wb6");
    if (dst == NULL) {
      log_error("Replay recorder failed creating compressed replay '%s'.",
                replay_state.path);
      fclose(src);
      return FALSE;
    }

    while (!feof(src)) {
      size_t nread = fread(buf, 1, sizeof(buf), src);

      if (nread > 0 && fc_gzwrite(dst, buf, nread) != (int) nread) {
        log_error("Replay recorder failed writing compressed replay '%s'.",
                  replay_state.path);
        ok = FALSE;
        break;
      }

      if (ferror(src)) {
        log_error("Replay recorder failed reading temporary replay '%s'.",
                  replay_state.tmp_path);
        ok = FALSE;
        break;
      }
    }

    fclose(src);
    if (fc_gzclose(dst) != Z_OK) {
      log_error("Replay recorder failed finalizing compressed replay '%s'.",
                replay_state.path);
      ok = FALSE;
    }
  }
#else  /* FREECIV_HAVE_LIBZ */
  if (rename(replay_state.tmp_path, replay_state.path) != 0) {
    log_error("Replay recorder failed renaming '%s' to '%s': %s",
              replay_state.tmp_path, replay_state.path, strerror(errno));
    ok = FALSE;
  }
#endif /* FREECIV_HAVE_LIBZ */

  if (ok) {
    if (fc_remove(replay_state.tmp_path) != 0 && errno != ENOENT) {
      log_error("Replay recorder failed removing temporary replay '%s': %s",
                replay_state.tmp_path, strerror(errno));
    }
  } else {
    fc_remove(replay_state.path);
  }

  return ok;
}

static void replay_drain_pending_packets(void)
{
  while (replay_state.pending_len >= 2) {
    struct data_in din;
    int packet_len;
    int whole_packet_len;

    dio_input_init(&din, replay_state.pending, replay_state.pending_len);
    dio_get_uint16_raw(&din, &packet_len);

    if (packet_len <= 0) {
      log_error("Replay recorder got invalid packet length %d.", packet_len);
      replay_state.active = FALSE;
      return;
    }

    if (packet_len == REPLAY_JUMBO_SIZE) {
      if (replay_state.pending_len < 6) {
        return;
      }

      dio_get_uint32_raw(&din, &whole_packet_len);
    } else if (packet_len >= REPLAY_COMPRESSION_BORDER) {
      whole_packet_len = packet_len - REPLAY_COMPRESSION_BORDER;
    } else {
      whole_packet_len = packet_len;
    }

    if (whole_packet_len <= 0) {
      log_error("Replay recorder got invalid framed length %d.",
                whole_packet_len);
      replay_state.active = FALSE;
      return;
    }

    if ((size_t) whole_packet_len > replay_state.pending_len) {
      return;
    }

    if (!replay_write_packet_record(replay_state.pending, whole_packet_len)) {
      log_error("Replay recorder failed writing packet data.");
      replay_state.active = FALSE;
      return;
    }

    replay_state.pending_len -= whole_packet_len;
    memmove(replay_state.pending,
            replay_state.pending + whole_packet_len,
            replay_state.pending_len);
  }
}

static void replay_recorder_flush_peer(void)
{
  unsigned char buffer[4096];

  if (!replay_state.active || replay_state.peer_fd < 0) {
    return;
  }

  while (TRUE) {
    int nread = read(replay_state.peer_fd, buffer, sizeof(buffer));

    if (nread > 0) {
      replay_queue_pending(buffer, nread);
      replay_drain_pending_packets();
      continue;
    }

    if (nread == 0) {
      break;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }

    log_error("Replay recorder failed reading capture socket: %s", strerror(errno));
    replay_state.active = FALSE;
    break;
  }
}

static void replay_close_connection(void)
{
  if (replay_state.conn == nullptr) {
    return;
  }

  conn_list_remove(game.all_connections, replay_state.conn);
  conn_list_remove(game.est_connections, replay_state.conn);
  conn_list_remove(game.glob_observers, replay_state.conn);

  connection_common_close(replay_state.conn);
  conn_list_destroy(replay_state.conn->self);
  free(replay_state.conn);
  replay_state.conn = nullptr;

  if (replay_state.peer_fd >= 0) {
    fc_closesocket(replay_state.peer_fd);
    replay_state.peer_fd = -1;
  }
}

bool replay_recorder_is_active(void)
{
  return replay_state.active;
}

bool replay_recorder_should_send(const struct conn_list *dest)
{
  return replay_state.active && dest != nullptr && replay_state.conn != nullptr
         && dest != replay_state.conn->self;
}

struct connection *replay_recorder_connection(void)
{
  return replay_state.conn;
}

struct conn_list *replay_recorder_dest(void)
{
  return replay_state.conn != nullptr ? replay_state.conn->self : nullptr;
}

bool replay_recorder_start(void)
{
  int fds[2];
  char filename[128];
  char tmp_filename[160];
  time_t now;
  struct tm *tm_now;
  struct packet_server_join_reply join_reply = {
    .you_can_join = TRUE
  };

  if (replay_state.active) {
    return TRUE;
  }

  now = time(nullptr);
  tm_now = localtime(&now);
  if (tm_now == nullptr) {
    return FALSE;
  }

  strftime(filename, sizeof(filename), "replay-%Y%m%d-%H%M%S.fcreplay", tm_now);
  fc_snprintf(tmp_filename, sizeof(tmp_filename), "%s.tmp", filename);

  FC_FREE(replay_state.path);
  FC_FREE(replay_state.tmp_path);
  replay_state.path = fc_strdup(filename);
  replay_state.tmp_path = fc_strdup(tmp_filename);

  replay_state.file = fc_fopen(replay_state.tmp_path, "wb+");
  if (replay_state.file == nullptr) {
    log_error("Replay recorder failed opening '%s': %s",
              replay_state.tmp_path, strerror(errno));
    return FALSE;
  }

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    log_error("Replay recorder failed creating socketpair: %s", strerror(errno));
    fclose(replay_state.file);
    replay_state.file = nullptr;
    FC_FREE(replay_state.path);
    FC_FREE(replay_state.tmp_path);
    return FALSE;
  }

  fc_nonblock(fds[0]);
  fc_nonblock(fds[1]);

  replay_state.conn = fc_calloc(1, sizeof(*replay_state.conn));
  replay_state.peer_fd = fds[1];

  connection_common_init(replay_state.conn);
  replay_state.conn->sock = fds[0];
  replay_state.conn->self = conn_list_new();
  conn_list_append(replay_state.conn->self, replay_state.conn);
  replay_state.conn->observer = TRUE;
  replay_state.conn->established = TRUE;
  replay_state.conn->access_level = ALLOW_INFO;
  replay_state.conn->server.status = AS_ESTABLISHED;
  replay_state.conn->notify_of_writable_data = replay_notify_writable;
  sz_strlcpy(replay_state.conn->username, "[replay]");
  sz_strlcpy(replay_state.conn->addr, "local-replay");
  conn_set_capability(replay_state.conn, our_capability);
  post_receive_packet_server_join_reply(replay_state.conn, &join_reply);

  conn_list_append(game.all_connections, replay_state.conn);
  conn_list_append(game.est_connections, replay_state.conn);
  conn_list_append(game.glob_observers, replay_state.conn);

  replay_state.active = TRUE;
  replay_state.phase = REPLAY_PHASE_NONE;
  replay_state.started_at = now;

  if (!replay_write_bytes("FCREPLAY", 8)
      || !replay_write_u16(1)
      || !replay_write_u16(0)
      || !replay_begin_chunk("INFO")
      || !replay_write_string(freeciv_name_version())
      || !replay_write_string(our_capability)
      || !replay_write_string(game.server.rulesetdir)
      || !replay_write_string(game.scenario.is_scenario ? game.scenario.name : "")
      || !replay_write_u32(game.info.turn)
      || !replay_write_u32(game.info.year)
      || !replay_write_u64((uint64_t) now)) {
    log_error("Replay recorder failed writing metadata.");
    replay_recorder_stop();
    return FALSE;
  }

  replay_state.info_final_turn_offset = ftell(replay_state.file);
  if (!replay_write_u32(game.info.turn)) {
    log_error("Replay recorder failed writing final turn metadata.");
    replay_recorder_stop();
    return FALSE;
  }

  replay_state.info_duration_offset = ftell(replay_state.file);
  if (!replay_write_u32(0)) {
    log_error("Replay recorder failed writing duration metadata.");
    replay_recorder_stop();
    return FALSE;
  }

  replay_state.info_players_offset = ftell(replay_state.file);
  {
    char players[REPLAY_INFO_PLAYERS_TEXT_LEN];

    replay_build_player_list(players, sizeof(players));
    if (!replay_write_fixed_string(players, sizeof(players))) {
      log_error("Replay recorder failed writing player list metadata.");
      replay_recorder_stop();
      return FALSE;
    }
  }

  replay_state.info_result_offset = ftell(replay_state.file);
  replay_state.info_winner_offset = replay_state.info_result_offset
                                   + REPLAY_INFO_RESULT_TEXT_LEN;
  if (!replay_write_fixed_string("", REPLAY_INFO_RESULT_TEXT_LEN)
      || !replay_write_fixed_string("", REPLAY_INFO_WINNER_TEXT_LEN)) {
    log_error("Replay recorder failed writing result metadata.");
    replay_recorder_stop();
    return FALSE;
  }

  replay_finish_chunk();
  log_normal("Replay recorder writing to %s", replay_state.path);

  return TRUE;
}

void replay_recorder_begin_snapshot(void)
{
  if (!replay_state.active || replay_state.phase != REPLAY_PHASE_NONE) {
    return;
  }

  if (!replay_begin_chunk("SNAP")) {
    log_error("Replay recorder failed starting snapshot chunk.");
    replay_recorder_stop();
    return;
  }

  replay_state.phase = REPLAY_PHASE_SNAPSHOT;
}

void replay_recorder_end_snapshot(void)
{
  if (!replay_state.active || replay_state.phase != REPLAY_PHASE_SNAPSHOT) {
    return;
  }

  replay_recorder_flush_peer();
  replay_finish_chunk();

  if (!replay_begin_chunk("EVNT")) {
    log_error("Replay recorder failed starting event chunk.");
    replay_recorder_stop();
    return;
  }

  replay_state.phase = REPLAY_PHASE_EVENTS;
}

void replay_recorder_stop(void)
{
  bool finalize = replay_state.active;

  if (replay_state.file != nullptr) {
    replay_recorder_flush_peer();

    if (replay_state.chunk_size_offset != 0) {
      replay_finish_chunk();
    }

    if (replay_state.active) {
      char winners[REPLAY_INFO_WINNER_TEXT_LEN];

      replay_build_winner_list(winners, sizeof(winners));
      if (!replay_patch_u32(replay_state.info_final_turn_offset, game.info.turn)
          || !replay_patch_u32(replay_state.info_duration_offset,
                               (uint32_t) MAX(0, time(nullptr) - replay_state.started_at))
          || !replay_patch_fixed_string(replay_state.info_result_offset,
                                        REPLAY_INFO_RESULT_TEXT_LEN,
                                        replay_result_text())
          || !replay_patch_fixed_string(replay_state.info_winner_offset,
                                        REPLAY_INFO_WINNER_TEXT_LEN,
                                        winners)) {
        log_error("Replay recorder failed patching final metadata.");
      }

      replay_write_bytes("DONE", 4);
      replay_write_u32(0);
    }

    fclose(replay_state.file);
    replay_state.file = nullptr;
  }

  if (finalize && !replay_compress_file()) {
    log_error("Replay recorder failed to finalize compressed replay '%s'.",
              replay_state.path != NULL ? replay_state.path : "(unknown)");
  }

  replay_close_connection();

  FC_FREE(replay_state.pending);
  replay_state.pending = nullptr;
  replay_state.pending_len = 0;
  replay_state.pending_cap = 0;
  replay_state.phase = REPLAY_PHASE_NONE;
  replay_state.active = FALSE;
  replay_state.started_at = 0;
  replay_state.info_final_turn_offset = 0;
  replay_state.info_duration_offset = 0;
  replay_state.info_players_offset = 0;
  replay_state.info_result_offset = 0;
  replay_state.info_winner_offset = 0;
  FC_FREE(replay_state.path);
  FC_FREE(replay_state.tmp_path);
}

#else  /* FREECIV_REPLAY_RECORDER */

#include "replay.h"

bool replay_recorder_is_active(void)
{
  return FALSE;
}

bool replay_recorder_should_send(const struct conn_list *dest)
{
  return false;
}

struct connection *replay_recorder_connection(void)
{
  return NULL;
}

struct conn_list *replay_recorder_dest(void)
{
  return NULL;
}

bool replay_recorder_start(void)
{
  return false;
}

void replay_recorder_begin_snapshot(void)
{
}

void replay_recorder_end_snapshot(void)
{
}

void replay_recorder_stop(void)
{
}

#endif /* FREECIV_REPLAY_RECORDER */
