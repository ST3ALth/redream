#ifndef OPTIONS_H
#define OPTIONS_H

#include <stdbool.h>
#include <string.h>
#include "core/constructor.h"
#include "core/list.h"

#define MAX_OPTION_LENGTH 1024

#define DECLARE_OPTION_BOOL(name) extern bool OPTION_##name;

#define DEFINE_OPTION_BOOL(name, value, desc)               \
  bool OPTION_##name;                                       \
  static struct option OPTION_T_##name = {                  \
      OPT_BOOL, #name, desc, &OPTION_##name, {NULL, NULL}}; \
  CONSTRUCTOR(OPTION_REGISTER_##name) {                     \
    *(bool *)(&OPTION_##name) = value;                      \
    options_register(&OPTION_T_##name);                     \
  }                                                         \
  DESTRUCTOR(OPTION_UNREGISTER_##name) {                    \
    options_unregister(&OPTION_T_##name);                   \
  }

#define DECLARE_OPTION_INT(name) extern int OPTION_##name;

#define DEFINE_OPTION_INT(name, value, desc)               \
  int OPTION_##name;                                       \
  static struct option OPTION_T_##name = {                 \
      OPT_INT, #name, desc, &OPTION_##name, {NULL, NULL}}; \
  CONSTRUCTOR(OPTION_REGISTER_##name) {                    \
    *(int *)(&OPTION_##name) = value;                      \
    options_register(&OPTION_T_##name);                    \
  }                                                        \
  DESTRUCTOR(OPTION_UNREGISTER_##name) {                   \
    options_unregister(&OPTION_T_##name);                  \
  }

#define DECLARE_OPTION_STRING(name) \
  extern char OPTION_##name[MAX_OPTION_LENGTH];

#define DEFINE_OPTION_STRING(name, value, desc)               \
  char OPTION_##name[MAX_OPTION_LENGTH];                      \
  static struct option OPTION_T_##name = {                    \
      OPT_STRING, #name, desc, &OPTION_##name, {NULL, NULL}}; \
  CONSTRUCTOR(OPTION_REGISTER_##name) {                       \
    strncpy(OPTION_##name, value, MAX_OPTION_LENGTH);         \
    options_register(&OPTION_T_##name);                       \
  }                                                           \
  DESTRUCTOR(OPTION_UNREGISTER_##name) {                      \
    options_unregister(&OPTION_T_##name);                     \
  }

enum option_type {
  OPT_BOOL,
  OPT_INT,
  OPT_STRING,
};

struct option {
  enum option_type type;
  const char *name;
  const char *desc;
  void *storage;
  struct list_node it;
};

void options_register(struct option *option);
void options_unregister(struct option *option);

void options_parse(int *argc, char ***argv);
bool options_read(const char *filename);
bool options_write(const char *filename);

void options_print_help();

#endif
