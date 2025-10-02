#include <assert.h>
#include <locale.h>
#include <wchar.h>
#define main ghstatus_main
#include "ghstatus.c"
#undef main

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
  return 0;
}
