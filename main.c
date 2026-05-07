#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define WPM 350
#define DEFAULT_COLS 80
#define INITIAL_BUFFER_SIZE 4096

char* readInput(FILE *stream) {
  size_t cap = INITIAL_BUFFER_SIZE;
  size_t len = 0;
  char *buffer = malloc(cap);
  if (buffer == NULL) {
    return NULL;
  }
  while (1) {
    if (len + 1 >= cap) {
      cap *= 2;
      char *tmp = realloc(buffer, cap);
      if (tmp == NULL) {
        free(buffer);
        return NULL;
      }
      buffer = tmp;
    }
    size_t n = fread(buffer + len, 1, cap - len - 1, stream);
    len += n;
    if (n == 0) {
      break;
    }
  }
  buffer[len] = '\0';
  return buffer;
}

int terminalCols(void) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    return ws.ws_col;
  }
  return DEFAULT_COLS;
}

char* printNextWord(char *buffer, int *linesUsed) {
  char letter = ' ';
  int wordLength = 0;
  while ((letter = buffer[wordLength]) != '\0' && letter != '.' && letter != ' ' && letter != '\n') {
    wordLength++;
  }

  int middle = wordLength / 2;
  int cols = terminalCols();
  int center = cols / 2;
  int padding = center - middle;
  if (padding < 0) {
    padding = 0;
  }
  for (int i = 0; i < padding; i++) {
    printf(" ");
  }
  for (int i = 0; i < wordLength; i++) {
    if (i == middle) {
      printf("\033[31m%c\033[0m", buffer[i]);
    } else {
      printf("%c", buffer[i]);
    }
  }

  int totalChars = padding + wordLength;
  *linesUsed = totalChars > 0 ? (totalChars + cols - 1) / cols : 1;

  buffer = &buffer[wordLength];
  fflush(stdout);
  while (buffer[0] == ' ' || buffer[0] == '\n' || buffer[0] == '.') {
    buffer = &buffer[1];
  }
  return buffer;
}

int parseInt(char *num) {
  int result = 0;
  for (int i = 0; num[i] != '\0'; i++) {
    if (num[i] < '0' || num[i] > '9') {
      return -1;
    }
    result = result * 10 + (num[i] - '0');
  }
  return result;
}

int main(int argc, char *argv[]) {
  int wpm = WPM;
  char *path = NULL;

  static struct option longOptions[] = {
    {"wpm",  required_argument, 0, 'w'},
    {"file", required_argument, 0, 'f'},
    {0, 0, 0, 0}
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "w:f:", longOptions, NULL)) != -1) {
    switch (opt) {
      case 'w':
        wpm = parseInt(optarg);
        if (wpm <= 0) {
          fprintf(stderr, "invalid wpm: %s\n", optarg);
          return 1;
        }
        break;
      case 'f':
        path = optarg;
        break;
      default:
        fprintf(stderr, "usage: %s [-w|--wpm wpm] [-f|--file file]\n", argv[0]);
        return 1;
    }
  }

  FILE *stream = stdin;
  if (path != NULL) {
    stream = fopen(path, "r");
    if (stream == NULL) {
      fprintf(stderr, "could not open file: %s\n", path);
      return 1;
    }
  }

  char *buffer = readInput(stream);
  if (stream != stdin) {
    fclose(stream);
  }
  if (buffer == NULL) {
    fprintf(stderr, "out of memory\n");
    return 1;
  }

  int linesUsed = 1;
  while (buffer[0] != '\0') {
    for (int i = 0; i < linesUsed - 1; i++) {
      printf("\033[A");
    }
    printf("\r\033[J");
    usleep(60 * 1000000 / wpm);
    buffer = printNextWord(buffer, &linesUsed);
  }
  return 0;
}

