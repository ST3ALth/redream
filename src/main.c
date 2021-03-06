#include "core/log.h"
#include "core/option.h"
#include "emu/emulator.h"
#include "emu/tracer.h"
#include "sys/exception_handler.h"
#include "sys/filesystem.h"
#include "ui/window.h"

DEFINE_OPTION_BOOL(help, false, "Show help");

int main(int argc, char **argv) {
  const char *appdir = fs_appdir();

  if (!fs_mkdir(appdir)) {
    LOG_FATAL("Failed to create app directory %s", appdir);
  }

  // load base options from config
  char config[PATH_MAX] = {0};
  snprintf(config, sizeof(config), "%s" PATH_SEPARATOR "config", appdir);
  options_read(config);

  // override options from the command line
  options_parse(&argc, &argv);

  if (OPTION_help) {
    options_print_help();
    return EXIT_SUCCESS;
  }

  if (!exception_handler_install()) {
    LOG_WARNING("Failed to initialize exception handler");
    return EXIT_FAILURE;
  }

  struct window *window = win_create();
  if (!window) {
    LOG_WARNING("Failed to initialize window");
    return EXIT_FAILURE;
  }

  const char *load = argc > 1 ? argv[1] : NULL;
  if (load && strstr(load, ".trace")) {
    struct tracer *tracer = tracer_create(window);
    tracer_run(tracer, load);
    tracer_destroy(tracer);
  } else {
    struct emu *emu = emu_create(window);
    emu_run(emu, load);
    emu_destroy(emu);
  }

  win_destroy(window);

  exception_handler_uninstall();

  // persist options for next run
  options_write(config);

  return EXIT_SUCCESS;
}
