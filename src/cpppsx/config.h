#ifndef MAC_CONFIG_H
#define MAC_CONFIG_H
#define STDC_HEADERS 1
#define HAVE_STRERROR 1
#define HAVE_VPRINTF 1
#define HAVE_UNISTD_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_TIME_H 1
#define HAVE_FCNTL_H 1
#define HAVE_SYS_FILE_H 1
#define HAVE_SYS_PARAMS_H 1
#define HAVE_SYS_RESOURCE_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDDEF_H 1
#define HAVE_BCOPY 1
#define HAVE_BZERO 1
#define HAVE_BCMP 1
#define HAVE_INDEX 1
#define HAVE_RINDEX 1
#define HAVE_PUTENV 1
#define HAVE_VFORK_H 0
#define HAVE_ATEXIT 1
#define POSIX 1
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include "config/mips/psx.h"
#define GCC_INCLUDE_DIR "/gcc-lib/PSX/2.8.1/include"
#define GPLUSPLUS_INCLUDE_DIR "/gcc-lib/PSX/2.8.1/g++-include"
#define OLD_GPLUSPLUS_INCLUDE_DIR "/gcc-lib/PSX/2.8.1/g++-include"
#define STANDARD_INCLUDE_DIR "/PSX/include"
#define FATAL_EXIT_CODE 33
#define SUCCESS_EXIT_CODE 0
#endif
