#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>

#define WPM 350
#define DEFAULT_COLS 80
#define INITIAL_BUFFER_SIZE 4096
#define RED_COLOR "\033[31m"
#define BLACK_COLOR "\033[0m"
#define HIDE_CURSOR "\033[?25l"
#define SHOW_CURSOR "\033[?25h"

#define CURSOR_UP "\033[A"
#define ERASE_LINE "\r\033[J"

#define EXTRA_PAUSE_MULTIPLIER 2.5

typedef struct {
    int wpm;
    char *path;
} CliOptions;

void restoreCursor(void) {
    printf(SHOW_CURSOR);
    fflush(stdout);
}

void onSignal(const int signal) {
    restoreCursor();
    _exit(128 + signal);
}

char *readInput(FILE *stream) {
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

int getWordLength(const char *buffer) {
    int wordLength = 0;
    char letter = ' ';
    while ((letter = buffer[wordLength]) != '\0' && letter != '.' && letter != ' ' && letter != '\n') {
        wordLength++;
    }
    return wordLength;
}

int printPadding(const int center, const int middle) {
    int padding = center - middle;
    if (padding < 0) {
        padding = 0;
    }
    for (int i = 0; i < padding; i++) {
        printf(" ");
    }
    return padding;
}

char *printNextWord(char *buffer, int *linesUsed) {
    const int wordLength = getWordLength(buffer);

    const int cols = terminalCols();
    const int center = cols / 2;
    const int middle = wordLength / 2;
    const int padding = printPadding(center, middle);
    for (int i = 0; i < wordLength; i++) {
        printf(i == middle ? RED_COLOR "%c" BLACK_COLOR : "%c", buffer[i]);
    }
    const int totalChars = padding + wordLength;
    *linesUsed = totalChars > 0 ? (totalChars + cols - 1) / cols : 1;

    buffer = &buffer[wordLength];
    fflush(stdout);
    return buffer;
}

int parseInt(const char *num) {
    int result = 0;
    for (int i = 0; num[i] != '\0'; i++) {
        if (num[i] < '0' || num[i] > '9') {
            return -1;
        }
        result = result * 10 + (num[i] - '0');
    }
    return result;
}

void limitToWPM(const int wpm) {
    usleep(60 * 1000000 / wpm);
}

void initTerminal(void) {
    atexit(restoreCursor);
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    printf(HIDE_CURSOR);
}

void cleanDisplay(const int linesUsed) {
    for (int i = 0; i < linesUsed - 1; i++) {
        printf(CURSOR_UP);
    }
    printf(ERASE_LINE);
}

int isExtraPauseCharacter(const char lastChar) {
    const char followingChar = *(&lastChar + 1);
    return (lastChar == '.' || lastChar == '!' || lastChar == '?') &&
           (followingChar == ' ' || followingChar == '\n' || followingChar == '\0');
}

CliOptions parseOptions(const int argc, char **argv) {
    CliOptions options = {
        .path = NULL,
        .wpm = WPM,
    };

    static struct option longOptions[] = {
        {"wpm", required_argument, 0, 'w'},
        {"file", required_argument, 0, 'f'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "w:f:", longOptions, NULL)) != -1) {
        switch (opt) {
            case 'w':
                options.wpm = parseInt(optarg);
                if (options.wpm <= 0) {
                    fprintf(stderr, "invalid wpm: %s\n", optarg);
                    exit(1);
                }
                break;
            case 'f':
                options.path = optarg;
                break;
            default:
                fprintf(stderr, "usage: %s [-w|--wpm wpm] [-f|--file file]\n", argv[0]);
                exit(1);
        }
    }

    return options;
}

int main(const int argc, char *argv[]) {
    const CliOptions options = parseOptions(argc, argv);

    FILE *stream = stdin;
    if (options.path != NULL) {
        stream = fopen(options.path, "r");
        if (stream == NULL) {
            fprintf(stderr, "could not open file: %s\n", options.path);
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

    initTerminal();

    int linesUsed = 1;
    while (buffer[0] != '\0') {
        cleanDisplay(linesUsed);
        buffer = printNextWord(buffer, &linesUsed);
        while (buffer[0] == ' ' || buffer[0] == '\n' || isExtraPauseCharacter(buffer[0])) {
            buffer = &buffer[1];
        }
        bool needsExtraPause = false;
        if (isExtraPauseCharacter(*(buffer - 2))) {
            needsExtraPause = true;
        }
        limitToWPM(needsExtraPause ? options.wpm / EXTRA_PAUSE_MULTIPLIER : options.wpm);
    }
    return 0;
}
