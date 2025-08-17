/*
 GitHub Actions Build Monitor
   usage: ghstatus user1 [user2 [user3 [...]]]
   build: gcc ghstatus.c -o ghstatus -lncursesw
*/

#define _GNU_SOURCE

#include <fcntl.h>
#include <locale.h>
#include <ncursesw/ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define MAX_REPOS 2048
#define POLL_INTERVAL_S 300  // seconds between full refresh
#define SPIN_INTERVAL_MS 125 // ms between spinner frame changes

char *REPOS[MAX_REPOS];
int NUM_REPOS = 0;
char STATUS[MAX_REPOS][64];
const wchar_t spinner_chars[] = L"🌑🌒🌓🌔🌕🌖🌗🌘";

typedef enum { SORT_DEFAULT, SORT_ALPHA, SORT_STATUS } SortMode;
SortMode sort_mode = SORT_DEFAULT;

int ORIGINAL_INDEX[MAX_REPOS]; // for restoring original order
int order[MAX_REPOS];          // active display order

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
    return;
  }

  if (pid == 0) { // child
    dup2(fds[1], STDOUT_FILENO);
    close(fds[0]);
    close(fds[1]);

    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }

    execlp("gh", "gh", "repo", "list", user, "--public", "--limit", "500",
           "--json", "nameWithOwner", "--jq", ".[].nameWithOwner",
           (char *)NULL);
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
    REPOS[NUM_REPOS++] = strdup(line);
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

const wchar_t *status_icon(const char *status) {
  if (strstr(status, "success"))
    return L"✅";
  if (strstr(status, "failure"))
    return L"❌";
  if (strstr(status, "cancelled"))
    return L"🛑";
  if (strstr(status, "skipped"))
    return L"⏭️";
  if (strstr(status, "timed_out"))
    return L"⌛";
  if (strstr(status, "action_required"))
    return L"⛔";
  if (strstr(status, "neutral"))
    return L"⭕";
  if (strstr(status, "stale"))
    return L"🥖";
  if (strstr(status, "in_progress"))
    return L"🔁";
  if (strstr(status, "queued"))
    return L"📋";
  if (strstr(status, "loading"))
    return L"🌀";
  return L"➖"; // fallback
}

int status_color(const char *status) {
  if (strstr(status, "success"))
    return 1;
  if (strstr(status, "failure") || strstr(status, "timed_out"))
    return 2;
  if (strstr(status, "cancelled") || strstr(status, "stale"))
    return 4;
  if (strstr(status, "skipped"))
    return 5;
  if (strstr(status, "action_required"))
    return 6;
  if (strstr(status, "in_progress"))
    return 7;
  return 3;
}

void spawn_fetches(int pipes[][2], pid_t pids[]) {
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

    if (pipe(pipes[i]) == -1) {
      pipes[i][0] = pipes[i][1] = -1;
      continue;
    }

    pid_t pid = fork();
    if (pid == 0) {
      dup2(pipes[i][1], STDOUT_FILENO);
      close(pipes[i][0]);
      close(pipes[i][1]);
      int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        close(devnull);
      }

      execlp("gh", "gh", "run", "list", "-L", "1", "-R", REPOS[i], "--json",
             "status,conclusion", "--jq",
             ".[0] | \"\\(.status) \\(.conclusion)\"", (char *)NULL);
      _exit(1);
    } else if (pid > 0) {
      pids[i] = pid;
      close(pipes[i][1]);
      fcntl(pipes[i][0], F_SETFL, O_NONBLOCK);
      strcpy(STATUS[i], "loading");
    } else {
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

long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
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
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <github-username> [user2 [user3 [...]]]\n",
            argv[0]);
    return 0;
  }

  for (int i = 1; i < argc; i++)
    load_repos(argv[i]);

  if (NUM_REPOS == 0) {
    fprintf(stderr, "No repos found for specified users, exiting...\n");
    return 0;
  }

  for (int i = 0; i < NUM_REPOS; i++) {
    ORIGINAL_INDEX[i] = i;
    order[i] = i;
  }

  int pipes[MAX_REPOS][2];
  pid_t fetch_pids[MAX_REPOS];
  for (int i = 0; i < MAX_REPOS; i++) {
    pipes[i][0] = pipes[i][1] = -1;
    fetch_pids[i] = -1;
  }
  spawn_fetches(pipes, fetch_pids);

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

    int secs_left = POLL_INTERVAL_S - (int)(time(NULL) - last_poll);
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
        } else if (n == 0) {
          close(pipes[i][0]);
          waitpid(fetch_pids[i], NULL, 0);
          pipes[i][0] = -1;
          fetch_pids[i] = -1;
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
    int count_success = 0, count_fail = 0, count_timeout = 0, count_cancel = 0,
        count_skipped = 0, count_progress = 0, count_action = 0,
        count_neutral = 0, count_stale = 0, count_queued = 0, count_loading = 0,
        count_other = 0;

    for (int i = 0; i < NUM_REPOS; i++) {
      if (strstr(STATUS[i], "success"))
        count_success++;
      else if (strstr(STATUS[i], "failure"))
        count_fail++;
      else if (strstr(STATUS[i], "timed_out"))
        count_timeout++;
      else if (strstr(STATUS[i], "cancelled"))
        count_cancel++;
      else if (strstr(STATUS[i], "skipped"))
        count_skipped++;
      else if (strstr(STATUS[i], "in_progress"))
        count_progress++;
      else if (strstr(STATUS[i], "action_required"))
        count_action++;
      else if (strstr(STATUS[i], "neutral"))
        count_neutral++;
      else if (strstr(STATUS[i], "stale"))
        count_stale++;
      else if (strstr(STATUS[i], "queued"))
        count_queued++;
      else if (strstr(STATUS[i], "loading"))
        count_loading++;
      else
        count_other++;
    }
    const char *sort_label = (sort_mode == SORT_DEFAULT) ? "Default"
                             : (sort_mode == SORT_ALPHA) ? "Alphabetical"
                                                         : "Status";

    mvprintw(
        term_rows - 2, 0,
        "📦%d 👥%d ✅%d ❌%d ⏳%d 🛑%d ⏭️%d 🔁%d ⛔%d ⭕%d 🥖%d 📋%d 🌀%d ➖%d",
        NUM_REPOS, argc - 1, count_success, count_fail, count_timeout,
        count_cancel, count_skipped, count_progress, count_action,
        count_neutral, count_stale, count_queued, count_loading, count_other);

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

    if (time(NULL) - last_poll >= POLL_INTERVAL_S) {
      spawn_fetches(pipes, fetch_pids);
      last_poll = time(NULL);
    }

    ch = getch();
    if (ch == 'q' || ch == 'Q')
      break;
    if (ch == ' ' && time(NULL) - last_poll >= 1) {
      spawn_fetches(pipes, fetch_pids);
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
                spawn_fetches(pipes, fetch_pids);
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
