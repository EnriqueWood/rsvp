#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    const char *end = buffer;
    while (*end && !isWordSeparator(*end)) {
        end++;
    }
    return (int)(end - buffer);
}

int utf8SeqLen(const char *p) {
    const unsigned char b = (unsigned char)*p;
    if ((b & 0x80) == 0x00) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 1;
}

WordMetrics measureWord(const char *buffer, const int byteLength) {
    int codePoints = 0;
    int byteIndex = 0;
    while (byteIndex < byteLength) {
        byteIndex += utf8SeqLen(buffer + byteIndex);
        codePoints++;
    }

    const int target = codePoints / 2;
    int middleOffset = 0;
    for (int codePoint = 0; codePoint < target; codePoint++) {
        middleOffset += utf8SeqLen(buffer + middleOffset);
    }
    const int middleLength = codePoints > 0
        ? utf8SeqLen(buffer + middleOffset)
        : 0;

    return (WordMetrics){
        .codePoints = codePoints,
        .middleByteOffset = middleOffset,
        .middleByteLength = middleLength,
    };
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

void printProgressBar(const long consumed, const long total, const int cols) {
    if (total <= 0 || cols <= 0) {
        return;
    }
    const int percent = (int)(100 * consumed / total);
    const int reserved = 7;
    if (cols < reserved + 1) {
        printf("%3d%%", percent);
        return;
    }
    const int barWidth = cols - reserved;
    const int filled = (int)((long)barWidth * consumed / total);

    putchar('[');
    for (int i = 0; i < filled; i++) {
        putchar('=');
    }
    for (int i = filled; i < barWidth; i++) {
        putchar(' ');
    }
    printf("] %3d%%", percent);
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
    const time_t wholeSecs = (time_t)seconds;
    const struct timespec ts = {
        .tv_sec = wholeSecs,
        .tv_nsec = (long)((seconds - wholeSecs) * 1e9),
    };
    nanosleep(&ts, NULL);
}

void installSignalHandler(const int signo, void (*handler)(int), const int flags) {
    struct sigaction sa = {0};
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = flags;
    sigaction(signo, &sa, NULL);
}

void initTerminal(void) {
    atexit(restoreCursor);
    installSignalHandler(SIGINT, onSignal, 0);
    installSignalHandler(SIGTERM, onSignal, 0);
    installSignalHandler(SIGWINCH, onWindowChange, SA_RESTART);
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

bool nextIsParagraphBreak(const char *cursor) {
    int newlines = 0;
    while (isWordSeparator(*cursor)) {
        if (*cursor == '\n') {
            newlines++;
        }
        cursor++;
    }
    return newlines >= 2;
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
    const long totalBytes = (long)strlen(text);

    char *cursor = text;
    int linesUsed = 1;
    while (*cursor) {
        while (isWordSeparator(*cursor)) {
            cursor++;
        }
        if (!*cursor) {
            break;
        }

        const int wordLength = getWordLength(cursor);
        const bool needsExtraPause = isSentenceEnd(cursor[wordLength - 1])
            || nextIsParagraphBreak(cursor + wordLength);
        const int cols = currentCols();

        cleanDisplay(linesUsed);
        const int totalChars = printWordCentered(cursor, wordLength, cols);
        const int wordLines = linesNeeded(totalChars, cols);

        cursor += wordLength;
        putchar('\n');
        printProgressBar(cursor - text, totalBytes, cols);
        linesUsed = wordLines + 1;
        fflush(stdout);

        sleepSeconds(needsExtraPause ? wordDuration * EXTRA_PAUSE_FACTOR : wordDuration);
    }

    free(text);
    return 0;
}
