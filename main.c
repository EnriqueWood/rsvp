#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define WPM 350
#define DEFAULT_COLS 80
#define INITIAL_BUFFER_SIZE 4096
#define RED_COLOR "\033[31m"
#define RESET_COLOR "\033[0m"
#define HIDE_CURSOR "\033[?25l"
#define SHOW_CURSOR "\033[?25h"

#define CURSOR_UP "\033[A"
#define ERASE_LINE "\r\033[J"

#define EXTRA_PAUSE_FACTOR 2.5

typedef struct {
    int wpm;
    char *path;
} CliOptions;

typedef struct {
    int codePoints;
    int middleByteOffset;
    int middleByteLength;
} WordMetrics;

static volatile sig_atomic_t terminalResized = 1;

void restoreCursor(void) {
    const char show[] = SHOW_CURSOR;
    (void)write(STDOUT_FILENO, show, sizeof(show) - 1);
}

void onSignal(const int signo) {
    restoreCursor();
    _exit(128 + signo);
}

void onWindowChange(const int signo) {
    (void)signo;
    terminalResized = 1;
}

void printUsage(FILE *out, const char *prog) {
    fprintf(out, "usage: %s [-w|--wpm wpm] [-f|--file file] [-h|--help]\n", prog);
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
        const size_t n = fread(buffer + len, 1, cap - len - 1, stream);
        len += n;
        if (n == 0) {
            if (ferror(stream)) {
                free(buffer);
                return NULL;
            }
            break;
        }
    }
    buffer[len] = '\0';
    return buffer;
}

int currentCols(void) {
    static int cached = DEFAULT_COLS;
    if (terminalResized) {
        terminalResized = 0;
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
            cached = ws.ws_col;
        }
    }
    return cached;
}

bool isWordSeparator(const char c) {
    return c == ' ' || c == '\t' || c == '\n';
}

int getWordLength(const char *buffer) {
    int wordLength = 0;
    char letter;
    while ((letter = buffer[wordLength]) != '\0' && !isWordSeparator(letter)) {
        wordLength++;
    }
    return wordLength;
}

int utf8SeqLen(const unsigned char b) {
    if ((b & 0x80) == 0x00) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 1;
}

WordMetrics measureWord(const char *buffer, const int byteLength) {
    int codePoints = 0;
    int i = 0;
    while (i < byteLength) {
        i += utf8SeqLen((unsigned char)buffer[i]);
        codePoints++;
    }

    const int target = codePoints / 2;
    int byteOffset = 0;
    int byteLen = 0;
    int j = 0;
    int cp = 0;
    while (j < byteLength) {
        int seqLen = utf8SeqLen((unsigned char)buffer[j]);
        if (j + seqLen > byteLength) {
            seqLen = byteLength - j;
        }
        if (cp == target) {
            byteOffset = j;
            byteLen = seqLen;
            break;
        }
        j += seqLen;
        cp++;
    }
    return (WordMetrics){codePoints, byteOffset, byteLen};
}

int linesNeeded(const int totalChars, const int cols) {
    if (totalChars <= 0 || cols <= 0) {
        return 1;
    }
    return (totalChars + cols - 1) / cols;
}

void printSpaces(const int count) {
    for (int i = 0; i < count; i++) {
        putchar(' ');
    }
}

int printWordCentered(const char *buffer, const int wordLength, const int cols) {
    const WordMetrics m = measureWord(buffer, wordLength);
    int padding = cols / 2 - m.codePoints / 2;
    if (padding < 0) {
        padding = 0;
    }

    const int rest = wordLength - m.middleByteOffset - m.middleByteLength;
    printSpaces(padding);
    printf("%.*s" RED_COLOR "%.*s" RESET_COLOR "%.*s",
           m.middleByteOffset, buffer,
           m.middleByteLength, buffer + m.middleByteOffset,
           rest, buffer + m.middleByteOffset + m.middleByteLength);
    return padding + m.codePoints;
}

int parseInt(const char *num) {
    if (num[0] == '\0') {
        return -1;
    }
    int result = 0;
    for (int i = 0; num[i] != '\0'; i++) {
        if (num[i] < '0' || num[i] > '9') {
            return -1;
        }
        const int digit = num[i] - '0';
        if (result > (INT_MAX - digit) / 10) {
            return -1;
        }
        result = result * 10 + digit;
    }
    return result;
}

double secondsForWPM(const int wpm) {
    return 60.0 / wpm;
}

void sleepSeconds(const double seconds) {
    if (seconds <= 0) {
        return;
    }
    const long ns = (long)(seconds * 1e9);
    const struct timespec ts = {
        .tv_sec = ns / 1000000000L,
        .tv_nsec = ns % 1000000000L,
    };
    nanosleep(&ts, NULL);
}

void initTerminal(void) {
    atexit(restoreCursor);

    struct sigaction exitAction = {0};
    exitAction.sa_handler = onSignal;
    sigemptyset(&exitAction.sa_mask);
    sigaction(SIGINT, &exitAction, NULL);
    sigaction(SIGTERM, &exitAction, NULL);

    struct sigaction winchAction = {0};
    winchAction.sa_handler = onWindowChange;
    sigemptyset(&winchAction.sa_mask);
    winchAction.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &winchAction, NULL);

    fputs(HIDE_CURSOR, stdout);
}

void cleanDisplay(const int linesUsed) {
    for (int i = 0; i < linesUsed - 1; i++) {
        fputs(CURSOR_UP, stdout);
    }
    fputs(ERASE_LINE, stdout);
}

bool isSentenceEnd(const char c) {
    return c == '.' || c == '!' || c == '?';
}

CliOptions parseOptions(const int argc, char **argv) {
    CliOptions options = {
        .path = NULL,
        .wpm = WPM,
    };

    static struct option longOptions[] = {
        {"wpm", required_argument, NULL, 'w'},
        {"file", required_argument, NULL, 'f'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "w:f:h", longOptions, NULL)) != -1) {
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
            case 'h':
                printUsage(stdout, argv[0]);
                exit(0);
            default:
                printUsage(stderr, argv[0]);
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

    char *text = readInput(stream);
    if (stream != stdin) {
        fclose(stream);
    }
    if (text == NULL) {
        fprintf(stderr, "failed to read input\n");
        return 1;
    }

    initTerminal();

    const double wordDuration = secondsForWPM(options.wpm);

    char *cursor = text;
    int linesUsed = 1;
    while (cursor[0] != '\0') {
        while (isWordSeparator(cursor[0])) {
            cursor = &cursor[1];
        }
        if (cursor[0] == '\0') {
            break;
        }

        const int wordLength = getWordLength(cursor);
        const bool needsExtraPause = isSentenceEnd(cursor[wordLength - 1]);
        const int cols = currentCols();

        cleanDisplay(linesUsed);
        const int totalChars = printWordCentered(cursor, wordLength, cols);
        linesUsed = linesNeeded(totalChars, cols);
        fflush(stdout);

        cursor = &cursor[wordLength];
        sleepSeconds(needsExtraPause ? wordDuration * EXTRA_PAUSE_FACTOR : wordDuration);
    }

    free(text);
    return 0;
}
