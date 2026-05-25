#ifndef JNILOG_ANSI_H
#define JNILOG_ANSI_H

/* Shared ANSI palette for C-layer logcat output. Keeps main.c, rangeset.c,
 * and any future C source visually consistent (and the Go-side palette in
 * logger.go uses the same escape codes for the colors it shares with us). */

#define C_RESET    "\x1b[0m"
#define C_DIM      "\x1b[2m"
#define C_GRAY     "\x1b[90m"
#define C_CYAN     "\x1b[36m"
#define C_MAGENTA  "\x1b[35m"
#define C_GREEN    "\x1b[32m"
#define C_YELLOW   "\x1b[33m"
#define C_ORANGE   "\x1b[38;5;214m"
#define C_LAVENDER "\x1b[38;2;180;190;254m"

#endif /* JNILOG_ANSI_H */
