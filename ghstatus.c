/*
 GitHub Actions Build Monitor
   usage: ghstatus [-p seconds>=0] [-c count>=1] [-w [addr:]port] [-s
 secret-file] user1 [user2 [user3 [...]]] build: gcc ghstatus.c -o ghstatus
 -lncursesw
*/

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <ncursesw/ncurses.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define MAX_REPOS 2048
#define POLL_INTERVAL_S 300       // seconds between full refresh
#define SPIN_INTERVAL_MS 125      // ms between spinner frame changes
#define MAX_CONCURRENT_FETCHES 32 // max number of simultaneous fetches

// hook mode: GitHub delivers webhooks here and each delivery refreshes only
// the repository it belongs to, instead of re-polling every repository.
#define MAX_WEBHOOK_CLIENTS 16
#define WEBHOOK_MAX_BODY (1024 * 1024) // largest accepted delivery payload
#define WEBHOOK_MAX_HEADERS 16384      // largest accepted header block
#define WEBHOOK_CLIENT_TIMEOUT_MS 15000
#define WEBHOOK_RECONCILE_S 900 // safety-net refresh while in hook mode
#define WEBHOOK_DEFAULT_HOST "127.0.0.1"
#define WEBHOOK_SECRET_MAX 256
#define WEBHOOK_SECRET_ENV "GHSTATUS_WEBHOOK_SECRET"

#define MAX_POLL_FDS (MAX_REPOS + MAX_WEBHOOK_CLIENTS + 1)

char *REPOS[MAX_REPOS];
int NUM_REPOS = 0;
char STATUS[MAX_REPOS][64];
static char status_buf[MAX_REPOS][128];
int status_received[MAX_REPOS];
int refresh_pending[MAX_REPOS];
const wchar_t spinner_chars[] = L"🌑🌒🌓🌔🌕🌖🌗🌘";

typedef struct {
  const char *match;
  const wchar_t *icon;
  const char *label;
  int color;
} StatusEntry;

StatusEntry status_map[] = {
    {"success", L"✅", "Conclusion: success", 1},
    {"failure", L"❌", "Conclusion: failure", 2},
    {"timed_out", L"⌛", "Conclusion: timed out", 2},
    {"cancelled", L"🛑", "Conclusion: cancelled", 4},
    {"skipped", L"⏭️", "Conclusion: skipped", 5},
    {"in_progress", L"🔁", "Status: in progress", 7},
    {"action_required", L"⛔", "Status: action required", 6},
    {"neutral", L"⭕", "Conclusion: neutral", 3},
    {"stale", L"🥖", "Status: stale", 4},
    {"queued", L"📋", "Status: queued", 3},
    {"loading", L"🌀", "Status: loading", 3},
    {"no_runs", L"🚫", "Status: no runs", 3},
    {NULL, L"➖", "Unknown status", 3},
};

#define STATUS_COUNT (sizeof(status_map) / sizeof(status_map[0]))
#define STATUS_KNOWN (STATUS_COUNT - 1)

typedef enum { SORT_DEFAULT, SORT_ALPHA, SORT_STATUS } SortMode;
SortMode sort_mode = SORT_STATUS;

int ORIGINAL_INDEX[MAX_REPOS]; // for restoring original order
int order[MAX_REPOS];          // active display order

int pipes[MAX_REPOS][2];
pid_t fetch_pids[MAX_REPOS];

// button hover state
int hover_x = -1, hover_y = -1;

// hook mode state
typedef struct {
  int fd;
  char *buf;
  size_t len;
  size_t cap;
  long long deadline_ms;
} WebhookClient;

WebhookClient webhook_clients[MAX_WEBHOOK_CLIENTS];
int webhook_listen_fd = -1;
bool webhook_enabled = false;
char webhook_secret[WEBHOOK_SECRET_MAX];
size_t webhook_secret_len = 0;
char webhook_listen_desc[128] = "";
char webhook_last_repo[128] = "";
unsigned long webhook_deliveries = 0;
unsigned long webhook_rejected = 0;

void apply_sort(void);
long long now_ms(void);
void request_refresh(int index);

void load_repos(const char *user) {
  int fds[2];
  if (pipe(fds) == -1)
    return;

  pid_t pid = fork();
  if (pid == -1) {
    close(fds[0]);
    close(fds[1]);
    fprintf(stderr, "Failed to fork 'gh'. GitHub CLI is required.\n");
    return;
  }

  if (pid == 0) { // child
    dup2(fds[1], STDOUT_FILENO);
    close(fds[0]);
    close(fds[1]);

    int err = dup(STDERR_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }

    execlp("gh", "gh", "repo", "list", user, "--limit",
           "500", "--json", "nameWithOwner", "--jq", ".[].nameWithOwner",
           (char *)NULL);
    if (err != -1) {
      dup2(err, STDERR_FILENO);
      close(err);
    }
    fprintf(stderr, "Failed to execute 'gh'. GitHub CLI is required.\n");
    _exit(1); // exec failed
  }

  close(fds[1]);
  FILE *fp = fdopen(fds[0], "r");
  if (!fp) {
    close(fds[0]);
    waitpid(pid, NULL, 0);
    return;
  }

  int old_num = NUM_REPOS;
  char line[256];
  while (fgets(line, sizeof(line), fp) && NUM_REPOS < MAX_REPOS) {
    line[strcspn(line, "\n")] = 0;
    char *copy = strdup(line);
    if (!copy) {
      // allocation failed; roll back any repos added in this call
      fprintf(stderr, "Failed to allocate repo name\n");
      while (NUM_REPOS > old_num) {
        free(REPOS[--NUM_REPOS]);
      }
      break; // stop reading further repositories
    }
    REPOS[NUM_REPOS++] = copy;
  }
  fclose(fp);

  int status;
  if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0) {
    while (NUM_REPOS > old_num) {
      free(REPOS[--NUM_REPOS]);
    }
  }
}

const StatusEntry *status_details(const char *status) {
  for (size_t i = 0; i < STATUS_KNOWN; i++) {
    if (status && strstr(status, status_map[i].match))
      return &status_map[i];
  }
  return &status_map[STATUS_KNOWN];
}

const wchar_t *status_icon(const char *status) {
  return status_details(status)->icon;
}

int status_color(const char *status) { return status_details(status)->color; }

void describe_status(const char *status, const char *fallback, char *buf,
                     size_t len) {
  if (!buf || len == 0)
    return;
  if (status && *status) {
    size_t bi = 0;
    for (const char *p = status; *p && bi + 1 < len; ++p) {
      char ch = (*p == '_') ? ' ' : *p;
      buf[bi++] = ch;
    }
    buf[bi] = '\0';
    if (bi > 0)
      return;
  }
  if (fallback) {
    snprintf(buf, len, "%s", fallback);
  } else {
    buf[0] = '\0';
  }
}

// Number of `gh` fetches currently in flight.
int active_fetches(void) {
  int active = 0;
  for (int i = 0; i < NUM_REPOS; i++) {
    if (fetch_pids[i] > 0)
      active++;
  }
  return active;
}

// Queue a refresh for a single repository. The fetch is started by
// start_pending_fetches() as soon as a concurrency slot frees up, so a burst
// of hook deliveries can never fork more children than the -c limit allows.
void request_refresh(int index) {
  if (index < 0 || index >= NUM_REPOS)
    return;
  refresh_pending[index] = 1;
}

void request_refresh_all(void) {
  for (int i = 0; i < NUM_REPOS; i++)
    refresh_pending[i] = 1;
}

// Look up a repository by "owner/name". Returns -1 when it is not tracked.
int find_repo(const char *full_name) {
  if (!full_name || !*full_name)
    return -1;
  for (int i = 0; i < NUM_REPOS; i++) {
    if (REPOS[i] && strcasecmp(REPOS[i], full_name) == 0)
      return i;
  }
  return -1;
}

// Start a `gh run list` fetch for one repository. Returns false when the fetch
// could not be started (a fetch is already in flight, or fork/pipe failed), in
// which case the pending flag is left set and the attempt is retried later.
bool spawn_fetch(int i, bool *status_changed) {
  if (i < 0 || i >= NUM_REPOS || pipes[i][0] != -1)
    return false;

  if (pipe(pipes[i]) == -1) {
    pipes[i][0] = pipes[i][1] = -1;
    return false;
  }

  pid_t pid = fork();
  if (pid == 0) {
    dup2(pipes[i][1], STDOUT_FILENO);
    close(pipes[i][0]);
    close(pipes[i][1]);
    int err = dup(STDERR_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }

    execlp("gh", "gh", "run", "list", "-L", "1", "-R", REPOS[i], "--json",
           "status,conclusion", "--jq",
           ".[0] | \"\\(.status) \\(.conclusion)\"", (char *)NULL);
    if (err != -1) {
      dup2(err, STDERR_FILENO);
      close(err);
    }
    fprintf(stderr, "Failed to execute 'gh'. GitHub CLI is required.\n");
    _exit(1);
  }

  if (pid < 0) {
    close(pipes[i][0]);
    close(pipes[i][1]);
    pipes[i][0] = pipes[i][1] = -1;
    return false;
  }

  fetch_pids[i] = pid;
  close(pipes[i][1]);
  pipes[i][1] = -1;
  fcntl(pipes[i][0], F_SETFL, O_NONBLOCK);
  // keep the pipe out of later `gh` children so they cannot hold it open
  fcntl(pipes[i][0], F_SETFD, FD_CLOEXEC);

  if (strcmp(STATUS[i], "loading") != 0) {
    strcpy(STATUS[i], "loading");
    if (status_changed)
      *status_changed = true;
  }
  status_received[i] = 0;
  status_buf[i][0] = '\0';
  return true;
}

// Start as many queued fetches as the concurrency limit allows.
void start_pending_fetches(int max_concurrent_fetches) {
  int active = active_fetches();
  bool status_changed = false;

  for (int i = 0; i < NUM_REPOS && active < max_concurrent_fetches; i++) {
    if (!refresh_pending[i] || pipes[i][0] != -1)
      continue;
    if (spawn_fetch(i, &status_changed)) {
      refresh_pending[i] = 0;
      active++;
    }
  }

  if (status_changed && sort_mode != SORT_DEFAULT)
    apply_sort();
}

/* ------------------------------------------------------------------------ */
/* SHA-256 / HMAC-SHA256, used to verify GitHub's X-Hub-Signature-256 header. */
/* ------------------------------------------------------------------------ */

typedef struct {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t data[64];
  size_t datalen;
} Sha256Ctx;

#define SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static void sha256_transform(Sha256Ctx *ctx, const uint8_t data[64]) {
  uint32_t m[64];
  for (int i = 0; i < 16; i++) {
    m[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
           ((uint32_t)data[i * 4 + 2] << 8) | ((uint32_t)data[i * 4 + 3]);
  }
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = SHA256_ROTR(m[i - 15], 7) ^ SHA256_ROTR(m[i - 15], 18) ^
                  (m[i - 15] >> 3);
    uint32_t s1 = SHA256_ROTR(m[i - 2], 17) ^ SHA256_ROTR(m[i - 2], 19) ^
                  (m[i - 2] >> 10);
    m[i] = m[i - 16] + s0 + m[i - 7] + s1;
  }

  uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2],
           d = ctx->state[3], e = ctx->state[4], f = ctx->state[5],
           g = ctx->state[6], h = ctx->state[7];

  for (int i = 0; i < 64; i++) {
    uint32_t s1 = SHA256_ROTR(e, 6) ^ SHA256_ROTR(e, 11) ^ SHA256_ROTR(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t t1 = h + s1 + ch + sha256_k[i] + m[i];
    uint32_t s0 = SHA256_ROTR(a, 2) ^ SHA256_ROTR(a, 13) ^ SHA256_ROTR(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = s0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
  ctx->state[5] += f;
  ctx->state[6] += g;
  ctx->state[7] += h;
}

static void sha256_init(Sha256Ctx *ctx) {
  ctx->datalen = 0;
  ctx->bitlen = 0;
  ctx->state[0] = 0x6a09e667;
  ctx->state[1] = 0xbb67ae85;
  ctx->state[2] = 0x3c6ef372;
  ctx->state[3] = 0xa54ff53a;
  ctx->state[4] = 0x510e527f;
  ctx->state[5] = 0x9b05688c;
  ctx->state[6] = 0x1f83d9ab;
  ctx->state[7] = 0x5be0cd19;
}

static void sha256_update(Sha256Ctx *ctx, const void *data, size_t len) {
  const uint8_t *bytes = (const uint8_t *)data;
  for (size_t i = 0; i < len; i++) {
    ctx->data[ctx->datalen++] = bytes[i];
    if (ctx->datalen == 64) {
      sha256_transform(ctx, ctx->data);
      ctx->bitlen += 512;
      ctx->datalen = 0;
    }
  }
}

static void sha256_final(Sha256Ctx *ctx, uint8_t out[32]) {
  size_t i = ctx->datalen;

  ctx->data[i++] = 0x80;
  if (i > 56) {
    while (i < 64)
      ctx->data[i++] = 0;
    sha256_transform(ctx, ctx->data);
    i = 0;
  }
  while (i < 56)
    ctx->data[i++] = 0;

  ctx->bitlen += (uint64_t)ctx->datalen * 8;
  for (int j = 0; j < 8; j++)
    ctx->data[56 + j] = (uint8_t)(ctx->bitlen >> (56 - 8 * j));
  sha256_transform(ctx, ctx->data);

  for (int j = 0; j < 8; j++) {
    out[j * 4] = (uint8_t)(ctx->state[j] >> 24);
    out[j * 4 + 1] = (uint8_t)(ctx->state[j] >> 16);
    out[j * 4 + 2] = (uint8_t)(ctx->state[j] >> 8);
    out[j * 4 + 3] = (uint8_t)(ctx->state[j]);
  }
}

void sha256(const void *data, size_t len, uint8_t out[32]) {
  Sha256Ctx ctx;
  sha256_init(&ctx);
  sha256_update(&ctx, data, len);
  sha256_final(&ctx, out);
}

void hmac_sha256(const void *key, size_t keylen, const void *msg, size_t msglen,
                 uint8_t out[32]) {
  uint8_t block[64] = {0};
  uint8_t ipad[64], opad[64];
  uint8_t inner[32];
  Sha256Ctx ctx;

  if (keylen > sizeof(block)) {
    sha256(key, keylen, block);
  } else if (keylen > 0) {
    memcpy(block, key, keylen);
  }

  for (size_t i = 0; i < sizeof(block); i++) {
    ipad[i] = (uint8_t)(block[i] ^ 0x36);
    opad[i] = (uint8_t)(block[i] ^ 0x5c);
  }

  sha256_init(&ctx);
  sha256_update(&ctx, ipad, sizeof(ipad));
  sha256_update(&ctx, msg, msglen);
  sha256_final(&ctx, inner);

  sha256_init(&ctx);
  sha256_update(&ctx, opad, sizeof(opad));
  sha256_update(&ctx, inner, sizeof(inner));
  sha256_final(&ctx, out);
}

void hex_encode(const uint8_t *bytes, size_t len, char *out) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = digits[bytes[i] >> 4];
    out[i * 2 + 1] = digits[bytes[i] & 0x0f];
  }
  out[len * 2] = '\0';
}

// Constant-time, case-insensitive comparison of two NUL-terminated hex digests.
int hex_equal_const_time(const char *a, const char *b) {
  size_t alen = strlen(a);
  if (alen != strlen(b))
    return 0;
  unsigned char diff = 0;
  for (size_t i = 0; i < alen; i++)
    diff |= (unsigned char)(tolower((unsigned char)a[i]) ^
                            tolower((unsigned char)b[i]));
  return diff == 0;
}

// Verify an "X-Hub-Signature-256: sha256=<hex>" header against the raw body.
bool webhook_signature_ok(const char *header, const char *body,
                          size_t body_len) {
  if (webhook_secret_len == 0)
    return true; // no secret configured: signatures are not required
  if (!header)
    return false;

  while (*header == ' ' || *header == '\t')
    header++;
  if (strncasecmp(header, "sha256=", 7) != 0)
    return false;

  uint8_t mac[32];
  char expected[65];
  hmac_sha256(webhook_secret, webhook_secret_len, body ? body : "", body_len,
              mac);
  hex_encode(mac, sizeof(mac), expected);
  return hex_equal_const_time(expected, header + 7) != 0;
}

/* ------------------------------------------------------------------------ */
/* Just enough JSON to pull repository.full_name out of a delivery payload.   */
/* ------------------------------------------------------------------------ */

static const char *json_skip_ws(const char *p, const char *end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
    p++;
  return p;
}

// `p` points at the opening quote; returns the position just past the closing
// quote, or NULL when the string is unterminated.
static const char *json_skip_string(const char *p, const char *end) {
  if (p >= end || *p != '"')
    return NULL;
  for (p++; p < end; p++) {
    if (*p == '\\') {
      p++;
      continue;
    }
    if (*p == '"')
      return p + 1;
  }
  return NULL;
}

// Returns the position just past the value starting at `p`.
static const char *json_skip_value(const char *p, const char *end) {
  p = json_skip_ws(p, end);
  if (p >= end)
    return NULL;

  if (*p == '"')
    return json_skip_string(p, end);

  if (*p == '{' || *p == '[') {
    char open = *p;
    char close = (open == '{') ? '}' : ']';
    int depth = 0;
    while (p < end) {
      if (*p == '"') {
        p = json_skip_string(p, end);
        if (!p)
          return NULL;
        continue;
      }
      if (*p == open) {
        depth++;
      } else if (*p == close) {
        if (--depth == 0)
          return p + 1;
      }
      p++;
    }
    return NULL;
  }

  while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
         *p != '\t' && *p != '\n' && *p != '\r')
    p++;
  return p;
}

// `obj` points at the '{' of an object; returns a pointer to the value of
// `key` within that object, or NULL when the key is absent.
static const char *json_member(const char *obj, const char *end,
                               const char *key) {
  if (!obj || obj >= end || *obj != '{')
    return NULL;

  size_t keylen = strlen(key);
  const char *p = obj + 1;

  for (;;) {
    p = json_skip_ws(p, end);
    if (p >= end || *p != '"')
      return NULL;

    const char *name = p + 1;
    const char *after = json_skip_string(p, end);
    if (!after)
      return NULL;
    size_t namelen = (size_t)(after - 1 - name);

    p = json_skip_ws(after, end);
    if (p >= end || *p != ':')
      return NULL;
    p = json_skip_ws(p + 1, end);

    if (namelen == keylen && strncmp(name, key, keylen) == 0)
      return p;

    p = json_skip_value(p, end);
    if (!p)
      return NULL;
    p = json_skip_ws(p, end);
    if (p < end && *p == ',') {
      p++;
      continue;
    }
    return NULL;
  }
}

// Copies the JSON string at `p` into `out`, resolving simple escapes.
static bool json_string_copy(const char *p, const char *end, char *out,
                             size_t outlen) {
  if (!p || p >= end || *p != '"' || !out || outlen == 0)
    return false;

  size_t oi = 0;
  for (p++; p < end && *p != '"'; p++) {
    char ch = *p;
    if (ch == '\\') {
      if (++p >= end)
        return false;
      switch (*p) {
      case 'n':
        ch = '\n';
        break;
      case 't':
        ch = '\t';
        break;
      case 'r':
        ch = '\r';
        break;
      case 'b':
        ch = '\b';
        break;
      case 'f':
        ch = '\f';
        break;
      case 'u':
        return false; // repository names never need \u escapes
      default:
        ch = *p;
        break;
      }
    }
    if (oi + 1 >= outlen)
      return false;
    out[oi++] = ch;
  }

  if (p >= end)
    return false;
  out[oi] = '\0';
  return true;
}

// Extracts the top-level repository.full_name ("owner/name") from a webhook
// payload. Nested copies (workflow_run.repository, head_repository) are
// deliberately ignored so a fork's run cannot redirect the refresh.
bool webhook_payload_repo(const char *body, size_t len, char *out,
                          size_t outlen) {
  if (!body || len == 0)
    return false;

  const char *end = body + len;
  const char *root = json_skip_ws(body, end);
  const char *repo = json_member(root, end, "repository");
  if (!repo)
    return false;

  const char *full = json_member(repo, end, "full_name");
  if (!full)
    return false;

  return json_string_copy(full, end, out, outlen);
}

/* ------------------------------------------------------------------------ */
/* Minimal HTTP endpoint that turns a webhook delivery into one repo refresh. */
/* ------------------------------------------------------------------------ */

typedef struct {
  int code;
  const char *reason;
  const char *body;
} WebhookReply;

// Finds a header value in the request's header block (case-insensitive).
bool http_header_value(const char *req, const char *end, const char *name,
                       char *out, size_t outlen) {
  if (!req || !end || !out || outlen == 0)
    return false;

  size_t namelen = strlen(name);
  const char *line = memchr(req, '\n', (size_t)(end - req));
  if (!line)
    return false;

  for (line++; line < end;) {
    const char *eol = memchr(line, '\n', (size_t)(end - line));
    const char *stop = eol ? eol : end;
    const char *colon = memchr(line, ':', (size_t)(stop - line));

    if (colon && (size_t)(colon - line) == namelen &&
        strncasecmp(line, name, namelen) == 0) {
      const char *v = colon + 1;
      while (v < stop && (*v == ' ' || *v == '\t'))
        v++;
      const char *ve = stop;
      while (ve > v && (ve[-1] == '\r' || ve[-1] == ' ' || ve[-1] == '\t'))
        ve--;
      size_t n = (size_t)(ve - v);
      if (n >= outlen)
        n = outlen - 1;
      memcpy(out, v, n);
      out[n] = '\0';
      return true;
    }

    if (!eol)
      break;
    line = eol + 1;
  }
  return false;
}

// Events that can change what the monitor displays for a repository.
bool webhook_event_tracked(const char *event) {
  static const char *tracked[] = {"workflow_run", "workflow_job", "check_run",
                                  "check_suite", "status"};
  for (size_t i = 0; i < sizeof(tracked) / sizeof(tracked[0]); i++) {
    if (strcasecmp(event, tracked[i]) == 0)
      return true;
  }
  return false;
}

// Handles one complete delivery. `req`/`req_len` span the header block
// (request line included), `body`/`body_len` the raw payload used for both
// signature verification and repository extraction.
WebhookReply webhook_handle_request(const char *req, size_t req_len,
                                    const char *body, size_t body_len) {
  const char *req_end = req + req_len;
  char value[256];

  if (req_len < 5 || strncmp(req, "POST ", 5) != 0) {
    webhook_rejected++;
    return (WebhookReply){405, "Method Not Allowed", "POST only\n"};
  }

  char signature[160];
  bool has_signature = http_header_value(req, req_end, "x-hub-signature-256",
                                         signature, sizeof(signature));
  if (!webhook_signature_ok(has_signature ? signature : NULL, body, body_len)) {
    webhook_rejected++;
    return (WebhookReply){401, "Unauthorized", "bad signature\n"};
  }

  char event[64];
  if (!http_header_value(req, req_end, "x-github-event", event,
                         sizeof(event))) {
    webhook_rejected++;
    return (WebhookReply){400, "Bad Request", "missing X-GitHub-Event\n"};
  }

  if (strcasecmp(event, "ping") == 0)
    return (WebhookReply){200, "OK", "pong\n"};

  if (!webhook_event_tracked(event))
    return (WebhookReply){202, "Accepted", "event ignored\n"};

  if (http_header_value(req, req_end, "content-type", value, sizeof(value)) &&
      strncasecmp(value, "application/x-www-form-urlencoded", 33) == 0) {
    webhook_rejected++;
    return (WebhookReply){415, "Unsupported Media Type",
                          "configure the hook with content type "
                          "application/json\n"};
  }

  char repo[128];
  if (!webhook_payload_repo(body, body_len, repo, sizeof(repo))) {
    webhook_rejected++;
    return (WebhookReply){400, "Bad Request", "no repository in payload\n"};
  }

  int index = find_repo(repo);
  if (index < 0)
    return (WebhookReply){202, "Accepted", "repository not tracked\n"};

  request_refresh(index);
  webhook_deliveries++;
  snprintf(webhook_last_repo, sizeof(webhook_last_repo), "%s", repo);
  return (WebhookReply){202, "Accepted", "refresh queued\n"};
}

static void webhook_write_all(int fd, const char *data, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, data + off, len - off);
    if (n > 0) {
      off += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    break; // best effort: the reply is small and the socket closes anyway
  }
}

static void webhook_respond(int fd, WebhookReply reply) {
  char head[256];
  const char *body = reply.body ? reply.body : "";
  int n = snprintf(head, sizeof(head),
                   "HTTP/1.1 %d %s\r\n"
                   "Content-Type: text/plain\r\n"
                   "Content-Length: %zu\r\n"
                   "Connection: close\r\n\r\n",
                   reply.code, reply.reason, strlen(body));
  if (n > 0 && (size_t)n < sizeof(head)) {
    webhook_write_all(fd, head, (size_t)n);
    webhook_write_all(fd, body, strlen(body));
  }
}

static void webhook_client_close(WebhookClient *client) {
  if (client->fd != -1)
    close(client->fd);
  free(client->buf);
  client->fd = -1;
  client->buf = NULL;
  client->len = client->cap = 0;
  client->deadline_ms = 0;
}

static void webhook_client_reply(WebhookClient *client, WebhookReply reply) {
  webhook_respond(client->fd, reply);
  webhook_client_close(client);
}

// Returns true once the client has been dealt with (answered and closed).
static bool webhook_client_dispatch(WebhookClient *client) {
  const char *sep = memmem(client->buf, client->len, "\r\n\r\n", 4);
  if (!sep) {
    if (client->len > WEBHOOK_MAX_HEADERS) {
      webhook_client_reply(client, (WebhookReply){431,
                                                  "Request Header Fields "
                                                  "Too Large",
                                                  "headers too large\n"});
      return true;
    }
    return false; // keep reading
  }

  size_t header_len = (size_t)(sep - client->buf) + 2; // keep trailing CRLF
  const char *body = sep + 4;
  size_t available = client->len - (size_t)(body - client->buf);

  char value[64];
  if (!http_header_value(client->buf, client->buf + header_len,
                         "content-length", value, sizeof(value))) {
    webhook_client_reply(client, (WebhookReply){411, "Length Required",
                                                "Content-Length required\n"});
    return true;
  }

  char *tail = NULL;
  errno = 0;
  unsigned long long content_length = strtoull(value, &tail, 10);
  if (errno != 0 || !tail || tail == value || *tail != '\0') {
    webhook_client_reply(
        client, (WebhookReply){400, "Bad Request", "bad Content-Length\n"});
    return true;
  }
  if (content_length > WEBHOOK_MAX_BODY) {
    webhook_client_reply(client, (WebhookReply){413, "Payload Too Large",
                                                "payload too large\n"});
    return true;
  }
  if (available < content_length)
    return false; // body still arriving

  webhook_client_reply(client,
                       webhook_handle_request(client->buf, header_len, body,
                                              (size_t)content_length));
  return true;
}

static void webhook_client_read(WebhookClient *client) {
  for (;;) {
    if (client->len + 1 >= client->cap) {
      size_t cap = client->cap ? client->cap * 2 : 8192;
      if (cap > WEBHOOK_MAX_BODY + WEBHOOK_MAX_HEADERS) {
        webhook_client_reply(client, (WebhookReply){413, "Payload Too Large",
                                                    "payload too large\n"});
        return;
      }
      char *buf = realloc(client->buf, cap);
      if (!buf) {
        webhook_client_close(client);
        return;
      }
      client->buf = buf;
      client->cap = cap;
    }

    ssize_t n = read(client->fd, client->buf + client->len,
                     client->cap - client->len - 1);
    if (n > 0) {
      client->len += (size_t)n;
      client->buf[client->len] = '\0';
      if (webhook_client_dispatch(client))
        return;
      continue;
    }
    if (n == 0) {
      webhook_client_close(client); // client hung up mid-request
      return;
    }
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return; // wait for the next poll() wakeup
    webhook_client_close(client);
    return;
  }
}

static void webhook_accept(void) {
  for (;;) {
    int fd = accept(webhook_listen_fd, NULL, NULL);
    if (fd < 0)
      return;

    fcntl(fd, F_SETFL, O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    int slot = -1;
    for (int i = 0; i < MAX_WEBHOOK_CLIENTS; i++) {
      if (webhook_clients[i].fd == -1) {
        slot = i;
        break;
      }
    }

    if (slot < 0) {
      webhook_respond(fd, (WebhookReply){503, "Service Unavailable",
                                         "too many connections\n"});
      close(fd);
      continue;
    }

    webhook_clients[slot].fd = fd;
    webhook_clients[slot].len = 0;
    webhook_clients[slot].deadline_ms = now_ms() + WEBHOOK_CLIENT_TIMEOUT_MS;
  }
}

// Drops connections that opened but never finished sending a request.
static void webhook_expire_clients(long long now) {
  for (int i = 0; i < MAX_WEBHOOK_CLIENTS; i++) {
    if (webhook_clients[i].fd != -1 && now > webhook_clients[i].deadline_ms) {
      webhook_respond(webhook_clients[i].fd,
                      (WebhookReply){408, "Request Timeout", "timeout\n"});
      webhook_client_close(&webhook_clients[i]);
    }
  }
}

static bool spec_all_digits(const char *s, size_t len) {
  if (len == 0)
    return false;
  for (size_t i = 0; i < len; i++) {
    if (!isdigit((unsigned char)s[i]))
      return false;
  }
  return true;
}

// Splits "port", "host:port" or "[v6addr]:port". An empty host means "bind
// every interface"; a bare port binds loopback only.
bool parse_listen_spec(const char *spec, char *host, size_t hostlen, char *port,
                       size_t portlen) {
  if (!spec || !*spec || !host || !port || hostlen == 0 || portlen == 0)
    return false;

  const char *colon;
  const char *host_start = spec;
  size_t host_len;

  if (*spec == '[') {
    const char *close = strchr(spec, ']');
    if (!close || close[1] != ':')
      return false;
    host_start = spec + 1;
    host_len = (size_t)(close - host_start);
    colon = close + 1;
  } else {
    colon = strrchr(spec, ':');
    if (!colon) {
      if (!spec_all_digits(spec, strlen(spec)))
        return false;
      if (strlen(spec) >= portlen || strlen(WEBHOOK_DEFAULT_HOST) >= hostlen)
        return false;
      snprintf(host, hostlen, "%s", WEBHOOK_DEFAULT_HOST);
      snprintf(port, portlen, "%s", spec);
      return true;
    }
    host_len = (size_t)(colon - spec);
  }

  const char *port_start = colon + 1;
  if (!spec_all_digits(port_start, strlen(port_start)))
    return false;
  if (host_len >= hostlen || strlen(port_start) >= portlen)
    return false;

  memcpy(host, host_start, host_len);
  host[host_len] = '\0';
  snprintf(port, portlen, "%s", port_start);
  return true;
}

// Opens the hook listener. Returns false (with a message on stderr) on error.
bool webhook_start(const char *spec) {
  char host[NI_MAXHOST];
  char port[16];

  for (int i = 0; i < MAX_WEBHOOK_CLIENTS; i++)
    webhook_clients[i].fd = -1;

  if (!parse_listen_spec(spec, host, sizeof(host), port, sizeof(port))) {
    fprintf(stderr,
            "Invalid hook listen address '%s'. Expected port, host:port or "
            "[v6addr]:port.\n",
            spec);
    return false;
  }

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  struct addrinfo *res = NULL;
  int rc = getaddrinfo(host[0] ? host : NULL, port, &hints, &res);
  if (rc != 0) {
    fprintf(stderr, "Cannot resolve hook listen address '%s': %s\n", spec,
            gai_strerror(rc));
    return false;
  }

  int fd = -1;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0)
      continue;

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
        listen(fd, MAX_WEBHOOK_CLIENTS) == 0)
      break;

    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);

  if (fd < 0) {
    fprintf(stderr, "Cannot listen for hooks on '%s': %s\n", spec,
            strerror(errno));
    return false;
  }

  fcntl(fd, F_SETFL, O_NONBLOCK);
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  webhook_listen_fd = fd;
  webhook_enabled = true;
  snprintf(webhook_listen_desc, sizeof(webhook_listen_desc), "%.100s:%.10s",
           host[0] ? host : "*", port);
  return true;
}

static void webhook_set_secret(const char *secret, size_t len) {
  if (len > sizeof(webhook_secret))
    len = sizeof(webhook_secret);
  memcpy(webhook_secret, secret, len);
  webhook_secret_len = len;
}

// Reads the shared secret from a file so it never appears in `ps` output.
bool webhook_load_secret_file(const char *path) {
  FILE *fp = fopen(path, "r");
  if (!fp) {
    fprintf(stderr, "Cannot read hook secret from '%s': %s\n", path,
            strerror(errno));
    return false;
  }

  char line[WEBHOOK_SECRET_MAX];
  if (!fgets(line, sizeof(line), fp)) {
    fclose(fp);
    fprintf(stderr, "Hook secret file '%s' is empty.\n", path);
    return false;
  }
  fclose(fp);

  size_t len = strcspn(line, "\r\n");
  if (len == 0) {
    fprintf(stderr, "Hook secret file '%s' is empty.\n", path);
    return false;
  }

  webhook_set_secret(line, len);
  return true;
}

void webhook_load_secret_env(void) {
  const char *secret = getenv(WEBHOOK_SECRET_ENV);
  if (secret && *secret)
    webhook_set_secret(secret, strlen(secret));
  // keep the secret out of the environment inherited by `gh` children
  unsetenv(WEBHOOK_SECRET_ENV);
}

void webhook_shutdown(void) {
  for (int i = 0; i < MAX_WEBHOOK_CLIENTS; i++) {
    if (webhook_clients[i].fd != -1)
      webhook_client_close(&webhook_clients[i]);
  }
  if (webhook_listen_fd != -1) {
    close(webhook_listen_fd);
    webhook_listen_fd = -1;
  }
}

void cleanup(int pipes[][2], pid_t pids[]) {
  webhook_shutdown();
  for (int i = 0; i < NUM_REPOS; i++) {
    free(REPOS[i]);
    if (pipes[i][0] != -1)
      close(pipes[i][0]);
    if (pipes[i][1] != -1)
      close(pipes[i][1]);
    if (pids[i] > 0) {
      kill(pids[i], SIGTERM);
      waitpid(pids[i], NULL, 0);
    }
  }
}

void handle_sigint(int signo) {
  (void)signo;
  cleanup(pipes, fetch_pids);
  endwin();
  exit(0);
}

long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

int sanitize_positive_option(const char *label, int value, int default_value,
                             int warn) {
  if (value < 1) {
    if (warn) {
      fprintf(stderr,
              "Invalid %s (%d). Value must be at least 1. Using default %d.\n",
              label, value, default_value);
    }
    return default_value;
  }
  return value;
}

int cmp_alpha(const void *a, const void *b) {
  int i = *(const int *)a;
  int j = *(const int *)b;
  if (!REPOS[i] || !REPOS[j])
    return (!REPOS[i]) - (!REPOS[j]);
  return strcmp(REPOS[i], REPOS[j]);
}

int status_rank(const char *status) {
  for (size_t i = 0; i < STATUS_KNOWN; i++) {
    if (status && strstr(status, status_map[i].match))
      return (int)i;
  }
  return (int)STATUS_KNOWN;
}

int cmp_status(const void *a, const void *b) {
  int i = *(const int *)a;
  int j = *(const int *)b;
  int c = status_rank(STATUS[i]) - status_rank(STATUS[j]);
  if (c == 0)
    c = strcmp(STATUS[i], STATUS[j]);
  if (c == 0)
    return strcmp(REPOS[i], REPOS[j]); // tie-break
  return c;
}

void apply_sort(void) {
  memcpy(order, ORIGINAL_INDEX, NUM_REPOS * sizeof(int));
  if (sort_mode == SORT_ALPHA) {
    qsort(order, NUM_REPOS, sizeof(int), cmp_alpha);
  } else if (sort_mode == SORT_STATUS) {
    qsort(order, NUM_REPOS, sizeof(int), cmp_status);
  }
}

static const char USAGE[] =
    "Usage: %s [-p seconds>=0] [-c count>=1] [-w [addr:]port] [-s "
    "secret-file]\n"
    "          <github-username> [user2 [user3 [...]]]\n"
    "\n"
    "  -p  seconds between full refreshes (0 disables, hook mode only)\n"
    "  -c  maximum simultaneous fetches\n"
    "  -w  listen for GitHub webhooks and refresh only the repository each\n"
    "      delivery names (a bare port binds " WEBHOOK_DEFAULT_HOST ")\n"
    "  -s  file holding the webhook secret (or set " WEBHOOK_SECRET_ENV ")\n";

int main(int argc, char **argv) {
  int poll_interval_s = POLL_INTERVAL_S;
  int max_concurrent_fetches = MAX_CONCURRENT_FETCHES;
  const char *webhook_spec = NULL;
  const char *webhook_secret_file = NULL;
  bool poll_given = false;
  int opt;

  while ((opt = getopt(argc, argv, "hp:c:w:s:")) != -1) {
    switch (opt) {
    case 'p':
      poll_interval_s = atoi(optarg);
      poll_given = true;
      break;
    case 'c':
      max_concurrent_fetches = atoi(optarg);
      break;
    case 'w':
      webhook_spec = optarg;
      break;
    case 's':
      webhook_secret_file = optarg;
      break;
    case 'h':
    default:
      fprintf(stderr, USAGE, argv[0]);
      return 0;
    }
  }

  max_concurrent_fetches =
      sanitize_positive_option("max concurrent fetches", max_concurrent_fetches,
                               MAX_CONCURRENT_FETCHES, 1);

  if (optind >= argc) {
    fprintf(stderr, USAGE, argv[0]);
    return 0;
  }

  if (webhook_spec) {
    webhook_load_secret_env();
    if (webhook_secret_file && !webhook_load_secret_file(webhook_secret_file))
      return 1;
    if (!webhook_start(webhook_spec))
      return 1;
    if (webhook_secret_len == 0) {
      fprintf(stderr,
              "Warning: no hook secret configured; deliveries on %s are not "
              "verified.\n",
              webhook_listen_desc);
    }
    // hook deliveries drive refreshes, so the timer only reconciles state that
    // a missed or undelivered hook would otherwise leave stale
    if (!poll_given)
      poll_interval_s = WEBHOOK_RECONCILE_S;
    else if (poll_interval_s < 0)
      poll_interval_s = WEBHOOK_RECONCILE_S;
  } else {
    if (webhook_secret_file)
      fprintf(stderr, "Ignoring -s: hook mode requires -w.\n");
    poll_interval_s = sanitize_positive_option("poll interval", poll_interval_s,
                                               POLL_INTERVAL_S, 1);
  }

  for (int i = optind; i < argc; i++)
    load_repos(argv[i]);
  int num_users = argc - optind;

  if (NUM_REPOS == 0) {
    fprintf(stderr, "No repos found for specified users, exiting...\n");
    return 0;
  }

  for (int i = 0; i < NUM_REPOS; i++) {
    ORIGINAL_INDEX[i] = i;
    order[i] = i;
    strcpy(STATUS[i], "loading");
  }
  apply_sort();

  for (int i = 0; i < MAX_REPOS; i++) {
    pipes[i][0] = pipes[i][1] = -1;
    fetch_pids[i] = -1;
  }
  for (int i = 0; i < NUM_REPOS; i++)
    strcpy(STATUS[i], "loading");
  request_refresh_all();

  setlocale(LC_CTYPE, "C.UTF-8");
  initscr();

  use_default_colors();
  start_color();
  assume_default_colors(-1, -1);

  cbreak();
  noecho();
  curs_set(0);
  keypad(stdscr, TRUE);
  nodelay(stdscr, TRUE);

  // enable mouse support
  mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);

  start_color();
  if (can_change_color()) {
    init_color(COLOR_YELLOW, 1000, 1000, 0); // redefine to #FFFF00
  }
  //       idx text         background
  init_pair(1, COLOR_WHITE, COLOR_CYAN);   // success
  init_pair(2, COLOR_WHITE, COLOR_RED);    // failure/timeout
  init_pair(3, COLOR_WHITE, COLOR_BLACK);  // neutral/unknown
  init_pair(4, COLOR_BLACK, COLOR_YELLOW); // cancelled/stale
  init_pair(5, COLOR_BLUE, COLOR_GREEN);   // skipped
  init_pair(6, COLOR_RED, COLOR_YELLOW);   // action_required
  init_pair(7, COLOR_WHITE, COLOR_BLUE);   // in_progress

  signal(SIGINT, handle_sigint);
  signal(SIGTERM, handle_sigint);
  signal(SIGPIPE, SIG_IGN);

  int ch;
  time_t last_poll = time(NULL);
  int spinner_index = 0;
  long long last_spin_update = now_ms();
  unsigned nsc = wcslen(spinner_chars);

  // clickable regions
  int q_col_start = 0, q_col_end = 0;
  int sp_col_start = 0, sp_col_end = 0;
  int s_col_start = 0, s_col_end = 0;

  // main loop
  while (1) {
    erase();

    long long now = now_ms();
    if (now - last_spin_update >= SPIN_INTERVAL_MS) {
      spinner_index = (spinner_index + 1) % nsc;
      last_spin_update = now;
    }

    int secs_left = poll_interval_s - (int)(time(NULL) - last_poll);
    if (secs_left < 0 || poll_interval_s <= 0)
      secs_left = 0;

    int row = 2;
    int col = 0;
    int term_rows, term_cols;
    getmaxyx(stdscr, term_rows, term_cols);

    int cell_w = 32;
    int cols_fit = term_cols / cell_w;
    if (cols_fit < 1)
      cols_fit = 1;

    struct pollfd pollfds[MAX_POLL_FDS];
    int poll_index[MAX_POLL_FDS];
    nfds_t poll_count = 0;
    for (int i = 0; i < NUM_REPOS; i++) {
      if (pipes[i][0] != -1) {
        pollfds[poll_count].fd = pipes[i][0];
        pollfds[poll_count].events = POLLIN;
        pollfds[poll_count].revents = 0;
        poll_index[poll_count] = i;
        poll_count++;
      }
    }

    nfds_t repo_poll_count = poll_count;
    int listen_slot = -1;
    int client_slot[MAX_WEBHOOK_CLIENTS];
    for (int c = 0; c < MAX_WEBHOOK_CLIENTS; c++)
      client_slot[c] = -1;

    if (webhook_listen_fd != -1) {
      pollfds[poll_count].fd = webhook_listen_fd;
      pollfds[poll_count].events = POLLIN;
      pollfds[poll_count].revents = 0;
      listen_slot = (int)poll_count++;

      for (int c = 0; c < MAX_WEBHOOK_CLIENTS; c++) {
        if (webhook_clients[c].fd == -1)
          continue;
        pollfds[poll_count].fd = webhook_clients[c].fd;
        pollfds[poll_count].events = POLLIN;
        pollfds[poll_count].revents = 0;
        client_slot[c] = (int)poll_count++;
      }
    }

    const int poll_timeout_ms = 100;
    int poll_result = 0;
    if (poll_count > 0) {
      poll_result = poll(pollfds, poll_count, poll_timeout_ms);
      if (poll_result < 0) {
        if (errno == EINTR)
          continue;
        // Unexpected error; skip processing this cycle.
        continue;
      }
    } else {
      poll_result = poll(NULL, 0, poll_timeout_ms);
      if (poll_result < 0 && errno != EINTR)
        continue;
    }

    bool updated_status = false;
    for (nfds_t pi = 0; pi < repo_poll_count; ++pi) {
      if (!(pollfds[pi].revents & (POLLIN | POLLHUP | POLLERR)))
        continue;

      int i = poll_index[pi];
      char buf[128];
      int n = read(pipes[i][0], buf, sizeof(buf) - 1);
      if (n > 0) {
        buf[n] = '\0';
        buf[strcspn(buf, "\n")] = 0;

        if (strncmp(STATUS[i], buf, sizeof(STATUS[i])) != 0) {
          strncpy(STATUS[i], buf, sizeof(STATUS[i]) - 1);
          STATUS[i][sizeof(STATUS[i]) - 1] = '\0';
          status_received[i] = 1;
          updated_status = true;
        }
      } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        close(pipes[i][0]);
        if (fetch_pids[i] > 0)
          waitpid(fetch_pids[i], NULL, 0);
        pipes[i][0] = -1;
        fetch_pids[i] = -1;
        if (!status_received[i]) {
          strcpy(STATUS[i], "no_runs");
        }
      }
    }

    if (listen_slot >= 0 &&
        (pollfds[listen_slot].revents & (POLLIN | POLLERR | POLLHUP)))
      webhook_accept();

    for (int c = 0; c < MAX_WEBHOOK_CLIENTS; c++) {
      if (client_slot[c] >= 0 &&
          (pollfds[client_slot[c]].revents & (POLLIN | POLLHUP | POLLERR)))
        webhook_client_read(&webhook_clients[c]);
    }
    if (webhook_enabled)
      webhook_expire_clients(now);

    start_pending_fetches(max_concurrent_fetches);

    if (updated_status && sort_mode != SORT_DEFAULT)
      apply_sort();

    for (int oi = 0; oi < NUM_REPOS; oi++) {
      int i = order[oi];
      const wchar_t *icon = status_icon(STATUS[i]);
      int color = status_color(STATUS[i]);
      attron(COLOR_PAIR(color));
      mvprintw(row, col * cell_w, "%ls %s", icon, REPOS[i]);
      attroff(COLOR_PAIR(color));
      col++;
      if (col >= cols_fit) {
        col = 0;
        row++;
      }
    }

    // stats
    int counts[STATUS_COUNT] = {0};
    for (int i = 0; i < NUM_REPOS; i++) {
      int matched = 0;
      for (size_t j = 0; j < STATUS_KNOWN; j++) {
        if (strstr(STATUS[i], status_map[j].match)) {
          counts[j]++;
          matched = 1;
          break;
        }
      }
      if (!matched)
        counts[STATUS_KNOWN]++;
    }
    const char *sort_label = (sort_mode == SORT_DEFAULT) ? "Default"
                             : (sort_mode == SORT_ALPHA) ? "Alphabetical"
                                                         : "Status";

    mvprintw(term_rows - 2, 0, "📦%d 👥%d", NUM_REPOS, num_users);
    int stats_col = getcurx(stdscr);
    int stats_start[STATUS_COUNT];
    int stats_end[STATUS_COUNT];
    for (size_t j = 0; j < STATUS_COUNT; j++) {
      stats_start[j] = stats_col;
      mvprintw(term_rows - 2, stats_col, " %ls%d", status_map[j].icon,
               counts[j]);
      stats_end[j] = getcurx(stdscr);
      stats_col = stats_end[j];
    }

    char tooltip[128] = "";
    int repo_rows = (NUM_REPOS + cols_fit - 1) / cols_fit;
    int repo_row_start = 2;
    int repo_row_end = repo_row_start + repo_rows;
    if (hover_x >= 0 && hover_y >= repo_row_start && hover_y < repo_row_end &&
        hover_x < cols_fit * cell_w) {
      int rel_row = hover_y - repo_row_start;
      int rel_col = hover_x / cell_w;
      int index = rel_row * cols_fit + rel_col;
      if (rel_col < cols_fit && index < NUM_REPOS) {
        int repo_index = order[index];
        const StatusEntry *entry = status_details(STATUS[repo_index]);
        describe_status(STATUS[repo_index], entry->label, tooltip,
                        sizeof(tooltip));
      }
    } else if (hover_y == term_rows - 2 && hover_x >= 0) {
      for (size_t j = 0; j < STATUS_COUNT; j++) {
        if (hover_x >= stats_start[j] && hover_x < stats_end[j]) {
          snprintf(tooltip, sizeof(tooltip), "%s (%d)", status_map[j].label,
                   counts[j]);
          break;
        }
      }
    } else if (hover_y == term_rows - 1 && hover_x >= 0) {
      if (hover_x >= q_col_start && hover_x <= q_col_end) {
        snprintf(tooltip, sizeof(tooltip), "Quit application");
      } else if (hover_x >= sp_col_start && hover_x <= sp_col_end) {
        snprintf(tooltip, sizeof(tooltip), "Refresh repository statuses");
      } else if (hover_x >= s_col_start && hover_x <= s_col_end) {
        snprintf(tooltip, sizeof(tooltip), "Change sorting mode");
      }
    }

    move(0, 0);
    clrtoeol();
    if (tooltip[0] != '\0') {
      mvprintw(0, 0, "%s", tooltip);
    } else if (webhook_enabled) {
      mvprintw(0, 0, "🪝 hooks on %s%s — %lu delivered, %lu rejected%s%s",
               webhook_listen_desc, webhook_secret_len ? " (signed)" : "",
               webhook_deliveries, webhook_rejected,
               webhook_last_repo[0] ? ", last: " : "", webhook_last_repo);
    }

    // --- footer buttons ---
    move(term_rows - 1, 0);

    // [q]
    if (hover_y == term_rows - 1 && hover_x >= 0 && hover_x <= 2)
      attron(A_REVERSE | A_BOLD);
    else
      attron(A_REVERSE);
    printw("[q]");
    attroff(A_REVERSE | A_BOLD);
    printw(" Quit ");
    q_col_start = 0;
    q_col_end = 2;

    // [space]
    int sp_start = getcurx(stdscr);
    if (hover_y == term_rows - 1 && hover_x >= sp_start &&
        hover_x <= sp_start + 6)
      attron(A_REVERSE | A_BOLD);
    else
      attron(A_REVERSE);
    printw("[space]");
    attroff(A_REVERSE | A_BOLD);
    printw(" Refresh ");
    sp_col_start = sp_start;
    sp_col_end = sp_start + 6;

    // [s]
    int s_start = getcurx(stdscr);
    if (hover_y == term_rows - 1 && hover_x >= s_start &&
        hover_x <= s_start + 2)
      attron(A_REVERSE | A_BOLD);
    else
      attron(A_REVERSE);
    printw("[s]");
    attroff(A_REVERSE | A_BOLD);
    printw(" %-12s", sort_label);
    s_col_start = s_start;
    s_col_end = s_start + 2;

    char clock[32];
    if (poll_interval_s > 0)
      snprintf(clock, sizeof(clock), "%ds", secs_left);
    else
      snprintf(clock, sizeof(clock), "hook");
    int clock_col;
    if (webhook_enabled) {
      char hooks[24];
      snprintf(hooks, sizeof(hooks), "%lu", webhook_deliveries);
      clock_col = getmaxx(stdscr) - (int)(strlen(clock) + strlen(hooks) + 8);
      mvprintw(term_rows - 1, clock_col > 0 ? clock_col : 0, "%lc 🪝%s %s",
               spinner_chars[spinner_index], hooks, clock);
    } else {
      clock_col = getmaxx(stdscr) - (int)strlen(clock) - 3;
      mvprintw(term_rows - 1, clock_col > 0 ? clock_col : 0, "%lc %s",
               spinner_chars[spinner_index], clock);
    }

    refresh();

    if (poll_interval_s > 0 && time(NULL) - last_poll >= poll_interval_s) {
      request_refresh_all();
      last_poll = time(NULL);
    }

    ch = getch();
    if (ch == 'q' || ch == 'Q')
      break;
    if (ch == ' ' && time(NULL) - last_poll >= 1) {
      request_refresh_all();
      last_poll = time(NULL);
    }
    if (ch == 's' || ch == 'S') {
      if (sort_mode == SORT_STATUS) {
        sort_mode = SORT_DEFAULT;
      } else if (sort_mode == SORT_DEFAULT) {
        sort_mode = SORT_ALPHA;
      } else {
        sort_mode = SORT_STATUS;
      }
      apply_sort();
    }
    if (ch == KEY_MOUSE) {
      MEVENT ev;
      if (getmouse(&ev) == OK) {
        hover_x = ev.x;
        hover_y = ev.y;
        if (ev.bstate & BUTTON1_CLICKED) {
          int footer_row = term_rows - 1;
          if (ev.y == footer_row) {
            if (ev.x >= q_col_start && ev.x <= q_col_end) {
              break; // clicked [q]
            } else if (ev.x >= sp_col_start && ev.x <= sp_col_end) {
              if (time(NULL) - last_poll >= 1) {
                request_refresh_all();
                last_poll = time(NULL);
              }
            } else if (ev.x >= s_col_start && ev.x <= s_col_end) {
              if (sort_mode == SORT_STATUS)
                sort_mode = SORT_DEFAULT;
              else if (sort_mode == SORT_DEFAULT)
                sort_mode = SORT_ALPHA;
              else
                sort_mode = SORT_STATUS;
              apply_sort();
            }
          }
        }
      }
    }
  }

  cleanup(pipes, fetch_pids);
  endwin();
  return 0;
}
