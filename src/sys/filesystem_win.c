#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <userenv.h>
#include "sys/filesystem.h"

bool fs_userdir(char *userdir, size_t size) {
  HANDLE accessToken = NULL;
  HANDLE processHandle = GetCurrentProcess();
  if (!OpenProcessToken(processHandle, TOKEN_QUERY, &accessToken)) {
    return false;
  }

  if (!GetUserProfileDirectory(accessToken, (LPSTR)userdir, (LPDWORD)&size)) {
    CloseHandle(accessToken);
    return false;
  }

  CloseHandle(accessToken);
  return true;
}

bool fs_exists(const char *path) {
  struct _stat buffer;
  return _stat(path, &buffer) == 0;
}

bool fs_isdir(const char *path) {
  struct _stat buffer;
  if (_stat(path, &buffer) != 0) {
    return false;
  }
  return (buffer.st_mode & S_IFDIR) == S_IFDIR;
}

bool fs_isfile(const char *path) {
  struct _stat buffer;
  if (_stat(path, &buffer) != 0) {
    return false;
  }
  return (buffer.st_mode & S_IFREG) == S_IFREG;
}

bool fs_mkdir(const char *path) {
  int res = _mkdir(path);
  return res == 0 || errno == EEXIST;
}
