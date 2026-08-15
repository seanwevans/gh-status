#include <arpa/inet.h>
#include <assert.h>
#include <locale.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#define main ghstatus_main
#include "ghstatus.c"
#undef main

static const char PAYLOAD[] =
    "{\"action\":\"completed\","
    "\"workflow_run\":{\"id\":42,\"status\":\"completed\","
    "\"conclusion\":\"failure\","
    "\"head_repository\":{\"full_name\":\"forker/other\"},"
    "\"repository\":{\"full_name\":\"forker/other\"}},"
    "\"repository\":{\"id\":7,\"name\":\"gh-status\","
    "\"full_name\":\"octocat/gh-status\"},"
    "\"sender\":{\"login\":\"octocat\"}}";

static char *build_request(const char *event, const char *signature,
                           const char *body, size_t *header_len) {
  static char request[4096];
  int n = snprintf(request, sizeof(request),
                   "POST /hook HTTP/1.1\r\n"
                   "Host: localhost\r\n"
                   "Content-Type: application/json\r\n"
                   "X-GitHub-Event: %s\r\n"
                   "%s%s%s"
                   "Content-Length: %zu\r\n"
                   "\r\n",
                   event, signature ? "X-Hub-Signature-256: " : "",
                   signature ? signature : "", signature ? "\r\n" : "",
                   strlen(body));
  *header_len = (size_t)n - 2; // header block, minus the blank-line CRLF
  return request;
}

static void hmac_hex(const char *secret, const char *body, char *out) {
  uint8_t mac[32];
  hmac_sha256(secret, strlen(secret), body, strlen(body), mac);
  hex_encode(mac, sizeof(mac), out);
}

static void test_sha256_and_hmac(void) {
  uint8_t digest[32];
  char hex[65];

  sha256("abc", 3, digest);
  hex_encode(digest, sizeof(digest), hex);
  assert(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff6"
                     "1f20015ad") == 0);

  sha256("", 0, digest);
  hex_encode(digest, sizeof(digest), hex);
  assert(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991"
                     "b7852b855") == 0);

  // RFC 4231 test case 2
  hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, digest);
  hex_encode(digest, sizeof(digest), hex);
  assert(strcmp(hex, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b"
                     "964ec3843") == 0);

  assert(hex_equal_const_time("ABCdef", "abcDEF"));
  assert(!hex_equal_const_time("abc", "abcd"));
  assert(!hex_equal_const_time("abcd", "abce"));
}

static void test_payload_parsing(void) {
  char repo[128];

  // the top-level repository wins over the nested fork copies
  assert(webhook_payload_repo(PAYLOAD, strlen(PAYLOAD), repo, sizeof(repo)));
  assert(strcmp(repo, "octocat/gh-status") == 0);

  const char *nested =
      "{\"repository\":{\"owner\":{\"full_name\":\"nope/nope\"},"
      "\"full_name\":\"octocat/spaced name\"}}";
  assert(webhook_payload_repo(nested, strlen(nested), repo, sizeof(repo)));
  assert(strcmp(repo, "octocat/spaced name") == 0);

  const char *no_repo = "{\"action\":\"completed\"}";
  assert(!webhook_payload_repo(no_repo, strlen(no_repo), repo, sizeof(repo)));

  const char *truncated = "{\"repository\":{\"full_name\":\"octo";
  assert(
      !webhook_payload_repo(truncated, strlen(truncated), repo, sizeof(repo)));

  assert(!webhook_payload_repo(PAYLOAD, strlen(PAYLOAD), repo, 4));
}

static void test_listen_spec(void) {
  char host[NI_MAXHOST];
  char port[16];

  assert(parse_listen_spec("9000", host, sizeof(host), port, sizeof(port)));
  assert(strcmp(host, WEBHOOK_DEFAULT_HOST) == 0 && strcmp(port, "9000") == 0);

  assert(parse_listen_spec("0.0.0.0:8080", host, sizeof(host), port,
                           sizeof(port)));
  assert(strcmp(host, "0.0.0.0") == 0 && strcmp(port, "8080") == 0);

  assert(
      parse_listen_spec("[::1]:8080", host, sizeof(host), port, sizeof(port)));
  assert(strcmp(host, "::1") == 0 && strcmp(port, "8080") == 0);

  assert(parse_listen_spec(":8080", host, sizeof(host), port, sizeof(port)));
  assert(host[0] == '\0' && strcmp(port, "8080") == 0);

  assert(
      !parse_listen_spec("localhost", host, sizeof(host), port, sizeof(port)));
  assert(!parse_listen_spec("localhost:http", host, sizeof(host), port,
                            sizeof(port)));
  assert(
      !parse_listen_spec("[::1]8080", host, sizeof(host), port, sizeof(port)));
  assert(!parse_listen_spec("", host, sizeof(host), port, sizeof(port)));
}

static void test_header_lookup(void) {
  size_t header_len;
  const char *req =
      build_request("workflow_run", "sha256=deadbeef", "{}", &header_len);
  char value[128];

  assert(http_header_value(req, req + header_len, "x-github-event", value,
                           sizeof(value)));
  assert(strcmp(value, "workflow_run") == 0);

  assert(http_header_value(req, req + header_len, "X-HUB-SIGNATURE-256", value,
                           sizeof(value)));
  assert(strcmp(value, "sha256=deadbeef") == 0);

  // the request line is not a header, and unknown names are absent
  assert(
      !http_header_value(req, req + header_len, "POST", value, sizeof(value)));
  assert(!http_header_value(req, req + header_len, "x-missing", value,
                            sizeof(value)));
}

static void test_hook_dispatch(void) {
  size_t header_len;
  char signature[80];
  char *req;

  REPOS[0] = strdup("octocat/gh-status");
  REPOS[1] = strdup("octocat/other");
  NUM_REPOS = 2;
  memset(refresh_pending, 0, sizeof(refresh_pending));

  assert(find_repo("OctoCat/GH-Status") == 0);
  assert(find_repo("octocat/missing") == -1);

  // unsigned mode: a tracked repository is queued, others are not
  webhook_secret_len = 0;
  req = build_request("workflow_run", NULL, PAYLOAD, &header_len);
  assert(
      webhook_handle_request(req, header_len, PAYLOAD, strlen(PAYLOAD)).code ==
      202);
  assert(refresh_pending[0] == 1);
  assert(refresh_pending[1] == 0);
  assert(webhook_deliveries == 1);
  assert(strcmp(webhook_last_repo, "octocat/gh-status") == 0);

  // events that cannot change a build status are acknowledged but ignored
  refresh_pending[0] = 0;
  req = build_request("issues", NULL, PAYLOAD, &header_len);
  assert(
      webhook_handle_request(req, header_len, PAYLOAD, strlen(PAYLOAD)).code ==
      202);
  assert(refresh_pending[0] == 0);

  req = build_request("ping", NULL, PAYLOAD, &header_len);
  assert(
      webhook_handle_request(req, header_len, PAYLOAD, strlen(PAYLOAD)).code ==
      200);
  assert(refresh_pending[0] == 0);

  // a delivery for an untracked repository must not queue anything
  const char *other = "{\"repository\":{\"full_name\":\"someone/else\"}}";
  req = build_request("check_run", NULL, other, &header_len);
  assert(webhook_handle_request(req, header_len, other, strlen(other)).code ==
         202);
  assert(refresh_pending[0] == 0 && refresh_pending[1] == 0);

  // signed mode: only a correct HMAC over the raw body is accepted
  memcpy(webhook_secret, "s3cret", 6);
  webhook_secret_len = 6;

  req = build_request("workflow_run", "sha256=00", PAYLOAD, &header_len);
  assert(
      webhook_handle_request(req, header_len, PAYLOAD, strlen(PAYLOAD)).code ==
      401);
  assert(refresh_pending[0] == 0);

  req = build_request("workflow_run", NULL, PAYLOAD, &header_len);
  assert(
      webhook_handle_request(req, header_len, PAYLOAD, strlen(PAYLOAD)).code ==
      401);
  assert(refresh_pending[0] == 0);

  char digest[65];
  hmac_hex("s3cret", PAYLOAD, digest);
  snprintf(signature, sizeof(signature), "sha256=%s", digest);
  req = build_request("workflow_run", signature, PAYLOAD, &header_len);
  assert(
      webhook_handle_request(req, header_len, PAYLOAD, strlen(PAYLOAD)).code ==
      202);
  assert(refresh_pending[0] == 1);

  // a tampered body no longer matches the signature it arrived with
  char tampered[sizeof(PAYLOAD)];
  snprintf(tampered, sizeof(tampered), "%s", PAYLOAD);
  tampered[2] = 'A';
  refresh_pending[0] = 0;
  req = build_request("workflow_run", signature, tampered, &header_len);
  assert(webhook_handle_request(req, header_len, tampered, strlen(tampered))
             .code == 401);
  assert(refresh_pending[0] == 0);

  webhook_secret_len = 0;
  free(REPOS[0]);
  free(REPOS[1]);
  NUM_REPOS = 0;
}

static int hook_listen_port(void) {
  struct sockaddr_storage addr;
  socklen_t len = sizeof(addr);
  assert(getsockname(webhook_listen_fd, (struct sockaddr *)&addr, &len) == 0);
  if (addr.ss_family == AF_INET)
    return ntohs(((struct sockaddr_in *)&addr)->sin_port);
  return ntohs(((struct sockaddr_in6 *)&addr)->sin6_port);
}

// Runs the same accept/read steps the main loop performs, without ncurses.
static void hook_pump(int rounds) {
  for (int i = 0; i < rounds; i++) {
    webhook_accept();
    for (int c = 0; c < MAX_WEBHOOK_CLIENTS; c++) {
      if (webhook_clients[c].fd != -1)
        webhook_client_read(&webhook_clients[c]);
    }
    usleep(2000);
  }
}

static int hook_connect(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  return fd;
}

static void hook_read_response(int fd, char *out, size_t outlen) {
  size_t len = 0;
  for (;;) {
    ssize_t n = read(fd, out + len, outlen - len - 1);
    if (n <= 0)
      break;
    len += (size_t)n;
    if (len + 1 >= outlen)
      break;
  }
  out[len] = '\0';
  close(fd);
}

// Drives a delivery over a real socket, exercising accept, header/body
// framing and the reply path exactly as the main loop would.
static void test_hook_server_roundtrip(void) {
  char response[1024];
  char request[4096];
  size_t header_len;

  REPOS[0] = strdup("octocat/gh-status");
  NUM_REPOS = 1;
  memset(refresh_pending, 0, sizeof(refresh_pending));
  webhook_secret_len = 0;

  assert(webhook_start("127.0.0.1:0"));
  int port = hook_listen_port();

  // a complete delivery arriving in one write
  int fd = hook_connect(port);
  const char *head = build_request("workflow_run", NULL, PAYLOAD, &header_len);
  snprintf(request, sizeof(request), "%s%s", head, PAYLOAD);
  assert(write(fd, request, strlen(request)) == (ssize_t)strlen(request));
  hook_pump(50);
  hook_read_response(fd, response, sizeof(response));
  assert(strstr(response, "202 Accepted") != NULL);
  assert(refresh_pending[0] == 1);

  // a delivery split across writes must wait for the body before dispatching
  refresh_pending[0] = 0;
  fd = hook_connect(port);
  head = build_request("workflow_run", NULL, PAYLOAD, &header_len);
  snprintf(request, sizeof(request), "%s", head);
  assert(write(fd, request, strlen(request)) == (ssize_t)strlen(request));
  hook_pump(10);
  assert(refresh_pending[0] == 0); // body has not arrived yet
  assert(write(fd, PAYLOAD, strlen(PAYLOAD)) == (ssize_t)strlen(PAYLOAD));
  hook_pump(50);
  hook_read_response(fd, response, sizeof(response));
  assert(strstr(response, "202 Accepted") != NULL);
  assert(refresh_pending[0] == 1);

  // GET is refused outright
  refresh_pending[0] = 0;
  fd = hook_connect(port);
  const char *get = "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";
  assert(write(fd, get, strlen(get)) == (ssize_t)strlen(get));
  hook_pump(50);
  hook_read_response(fd, response, sizeof(response));
  assert(strstr(response, "405 Method Not Allowed") != NULL);
  assert(refresh_pending[0] == 0);

  webhook_shutdown();
  assert(webhook_listen_fd == -1);
  free(REPOS[0]);
  NUM_REPOS = 0;
  webhook_enabled = false;
}

int main(void) {
  setlocale(LC_ALL, "");
  assert(wcscmp(status_icon("success"), L"✅") == 0);
  assert(wcscmp(status_icon("failure"), L"❌") == 0);
  assert(wcscmp(status_icon("no_runs"), L"🚫") == 0);
  assert(wcscmp(status_icon("unknown"), L"➖") == 0);

  assert(status_color("success") == 1);
  assert(status_color("failure") == 2);
  assert(status_color("no_runs") == 3);
  assert(status_color("unknown") == 3);

  assert(sort_mode == SORT_STATUS);
  assert(status_rank("success") < status_rank("failure"));
  assert(status_rank("failure") < status_rank("queued"));
  assert(status_rank("unknown") == (int)STATUS_KNOWN);

  assert(sanitize_positive_option("test", 5, 10, 0) == 5);
  assert(sanitize_positive_option("test", 0, 10, 0) == 10);

  test_sha256_and_hmac();
  test_payload_parsing();
  test_listen_spec();
  test_header_lookup();
  test_hook_dispatch();
  test_hook_server_roundtrip();
  return 0;
}
