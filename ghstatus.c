/*
 GitHub Actions Build Monitor
   usage: ghstatus [-p seconds>=1] [-c count>=1] user1 [user2 [user3 [...]]]
   build: gcc ghstatus.c -o ghstatus -lncursesw
*/

#define _GNU_SOURCE

#include <fcntl.h>
#include <locale.h>
#include <ncursesw/ncurses.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define MAX_REPOS 2048
#define POLL_INTERVAL_S 300       // seconds between full refresh
#define SPIN_INTERVAL_MS 125      // ms between spinner frame changes
#define MAX_CONCURRENT_FETCHES 32 // max number of simultaneous fetches

char *REPOS[MAX_REPOS];
int NUM_REPOS = 0;
char STATUS[MAX_REPOS][64];
int status_received[MAX_REPOS];
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
SortMode sort_mode = SORT_DEFAULT;

int ORIGINAL_INDEX[MAX_REPOS]; // for restoring original order
int order[MAX_REPOS];          // active display order

int pipes[MAX_REPOS][2];
pid_t fetch_pids[MAX_REPOS];

// button hover state
int hover_x = -1, hover_y = -1;

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

    execlp("gh", "gh", "repo", "list", user, "--visibility", "all",
           "--limit", "500",
           "--json", "nameWithOwner", "--jq", ".[].nameWithOwner",
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

int status_color(const char *status) {
  return status_details(status)->color;
}

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

void spawn_fetches(int pipes[][2], pid_t pids[], int max_concurrent_fetches) {
  // tear down any previous fetches
  for (int i = 0; i < NUM_REPOS; i++) {
    if (pipes[i][0] != -1) {
      close(pipes[i][0]);
      pipes[i][0] = -1;
    }
    if (pipes[i][1] != -1) {
      close(pipes[i][1]);
      pipes[i][1] = -1;
    }
    if (pids[i] > 0) {
      waitpid(pids[i], NULL, 0);
      pids[i] = -1;
    }
  }

  int running = 0; // currently active children
  for (int i = 0; i < NUM_REPOS; i++) {
    while (running >= max_concurrent_fetches) {
      int status;
      pid_t done = wait(&status);
      if (done <= 0)
        break;
      running--;
      for (int j = 0; j < NUM_REPOS; j++) {
        if (pids[j] == done) {
          pids[j] = -1;
          break;
        }
      }
    }

    if (pipe(pipes[i]) == -1) {
      pipes[i][0] = pipes[i][1] = -1;
      continue;
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
    } else if (pid > 0) {
      pids[i] = pid;
      close(pipes[i][1]);
      fcntl(pipes[i][0], F_SETFL, O_NONBLOCK);
      strcpy(STATUS[i], "loading");
      status_received[i] = 0;
      running++;
    } else {
      fprintf(stderr, "Failed to fork 'gh'. GitHub CLI is required.\n");
      close(pipes[i][0]);
      close(pipes[i][1]);
      pipes[i][0] = pipes[i][1] = -1;
    }
  }
}

void cleanup(int pipes[][2], pid_t pids[]) {
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
  return strcmp(REPOS[i], REPOS[j]);
}

int cmp_status(const void *a, const void *b) {
  int i = *(const int *)a;
  int j = *(const int *)b;
  int c = strcmp(STATUS[i], STATUS[j]);
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

int main(int argc, char **argv) {
  int poll_interval_s = POLL_INTERVAL_S;
  int max_concurrent_fetches = MAX_CONCURRENT_FETCHES;
  int opt;

  while ((opt = getopt(argc, argv, "hp:c:")) != -1) {
    switch (opt) {
    case 'p':
      poll_interval_s = atoi(optarg);
      break;
    case 'c':
      max_concurrent_fetches = atoi(optarg);
      break;
    case 'h':
    default:
      fprintf(stderr,
              "Usage: %s [-p seconds>=1] [-c count>=1] <github-username> [user2 "
              "[user3 [...]]]\n",
              argv[0]);
      return 0;
    }
  }

  poll_interval_s = sanitize_positive_option("poll interval", poll_interval_s,
                                             POLL_INTERVAL_S, 1);
  max_concurrent_fetches = sanitize_positive_option(
      "max concurrent fetches", max_concurrent_fetches, MAX_CONCURRENT_FETCHES,
      1);

  if (optind >= argc) {
    fprintf(stderr,
            "Usage: %s [-p seconds>=1] [-c count>=1] <github-username> [user2 [user3 "
            "[...]]]\n",
            argv[0]);
    return 0;
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
  }

  for (int i = 0; i < MAX_REPOS; i++) {
    pipes[i][0] = pipes[i][1] = -1;
    fetch_pids[i] = -1;
  }
  spawn_fetches(pipes, fetch_pids, max_concurrent_fetches);

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
    if (secs_left < 0)
      secs_left = 0;

    int row = 2;
    int col = 0;
    int term_rows, term_cols;
    getmaxyx(stdscr, term_rows, term_cols);

    int cell_w = 32;
    int cols_fit = term_cols / cell_w;
    if (cols_fit < 1)
      cols_fit = 1;

    fd_set readfds;
    FD_ZERO(&readfds);
    int maxfd = -1;
    for (int i = 0; i < NUM_REPOS; i++) {
      if (pipes[i][0] != -1) {
        FD_SET(pipes[i][0], &readfds);
        if (pipes[i][0] > maxfd)
          maxfd = pipes[i][0];
      }
    }
    struct timeval tv = {0, 100000}; // 100ms
    select(maxfd + 1, &readfds, NULL, NULL, &tv);

    for (int i = 0; i < NUM_REPOS; i++) {
      if (pipes[i][0] != -1 && FD_ISSET(pipes[i][0], &readfds)) {
        char buf[128];
        int n = read(pipes[i][0], buf, sizeof(buf) - 1);
        if (n > 0) {
          buf[n] = '\0';
          buf[strcspn(buf, "\n")] = 0;
          strncpy(STATUS[i], buf, sizeof(STATUS[i]) - 1);
          STATUS[i][sizeof(STATUS[i]) - 1] = '\0';
          status_received[i] = 1;
        } else if (n == 0) {
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
    }

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
    if (tooltip[0] != '\0')
      mvprintw(0, 0, "%s", tooltip);

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

    mvprintw(term_rows - 1, getmaxx(stdscr) - 10, "%lc %ds",
             spinner_chars[spinner_index], secs_left);

    refresh();

    if (time(NULL) - last_poll >= poll_interval_s) {
      spawn_fetches(pipes, fetch_pids, max_concurrent_fetches);
      last_poll = time(NULL);
    }

    ch = getch();
    if (ch == 'q' || ch == 'Q')
      break;
    if (ch == ' ' && time(NULL) - last_poll >= 1) {
      spawn_fetches(pipes, fetch_pids, max_concurrent_fetches);
      last_poll = time(NULL);
    }
    if (ch == 's' || ch == 'S') {
      if (sort_mode == SORT_DEFAULT) {
        sort_mode = SORT_ALPHA;
      } else if (sort_mode == SORT_ALPHA) {
        sort_mode = SORT_STATUS;
      } else {
        sort_mode = SORT_DEFAULT;
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
                spawn_fetches(pipes, fetch_pids, max_concurrent_fetches);
                last_poll = time(NULL);
              }
            } else if (ev.x >= s_col_start && ev.x <= s_col_end) {
              if (sort_mode == SORT_DEFAULT)
                sort_mode = SORT_ALPHA;
              else if (sort_mode == SORT_ALPHA)
                sort_mode = SORT_STATUS;
              else
                sort_mode = SORT_DEFAULT;
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
