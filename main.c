#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define WPM 350
#define MIN_WPM 50
#define MAX_WPM 2000
#define WPM_STEP 25
#define DEFAULT_COLS 80
#define DEFAULT_ROWS 24
#define INITIAL_BUFFER_SIZE 4096

#define ENTER_ALT_SCREEN "\033[?1049h"
#define EXIT_ALT_SCREEN  "\033[?1049l"
#define CLEAR_SCREEN     "\033[2J\033[H"
#define ERASE_BELOW      "\033[J"

#define RED_COLOR   "\033[31m"
#define RESET_COLOR "\033[0m"
#define HIDE_CURSOR "\033[?25l"
#define SHOW_CURSOR "\033[?25h"

#define KEY_ESC     '\033'
#define KEY_CSI     '['
#define ARROW_LEFT  'D'
#define ARROW_RIGHT 'C'

#define EXTRA_PAUSE_FACTOR 1.5

#define RAMP_WORDS 5
#define INITIAL_RAMP_START 0.5
#define RESUME_RAMP_START  0.8

#define WPM_INDICATOR_WORDS 5

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

static struct termios originalTermios;
static bool termiosSaved = false;
static int keyboardFd = -1;

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

void prepareTerminalInput(void) {
    if (!isatty(STDOUT_FILENO)) {
        return;
    }
    if (tcgetattr(STDOUT_FILENO, &originalTermios) != 0) {
        return;
    }
    termiosSaved = true;
    struct termios mode = originalTermios;
    mode.c_lflag &= ~(ECHO | ICANON);
    mode.c_cc[VMIN] = 0;
    mode.c_cc[VTIME] = 0;
    tcsetattr(STDOUT_FILENO, TCSANOW, &mode);
}

void openKeyboard(void) {
    if (!isatty(STDOUT_FILENO)) {
        return;
    }
    keyboardFd = open("/dev/tty", O_RDONLY);
}

char waitKeyOrTimeout(const double seconds) {
    if (keyboardFd < 0) {
        if (seconds > 0) sleepSeconds(seconds);
        return 0;
    }
    struct pollfd pfd = {.fd = keyboardFd, .events = POLLIN};
    const int timeoutMs = seconds < 0 ? -1 : (int)(seconds * 1000);
    if (poll(&pfd, 1, timeoutMs) <= 0) {
        return 0;
    }
    char byte = 0;
    (void)read(keyboardFd, &byte, 1);
    return byte;
}

typedef enum {
    NAV_TIMEOUT,
    NAV_QUIT,
    NAV_TOGGLE_PAUSE,
    NAV_BACK,
    NAV_FORWARD,
    NAV_FASTER,
    NAV_SLOWER,
    NAV_RESTART,
} NavEvent;

char readKey(const double seconds) {
    const char key = waitKeyOrTimeout(seconds);
    if (key != KEY_ESC) {
        return key;
    }
    const char introducer = waitKeyOrTimeout(0.05);
    if (introducer == KEY_CSI) {
        const char finalByte = waitKeyOrTimeout(0.05);
        if (finalByte == ARROW_LEFT)  return 'h';
        if (finalByte == ARROW_RIGHT) return 'l';
    }
    return 0;
}

NavEvent waitDuringWord(const double duration) {
    double remaining = duration;
    while (remaining > 0) {
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        const char key = readKey(remaining);
        clock_gettime(CLOCK_MONOTONIC, &end);
        const double elapsed = (end.tv_sec - start.tv_sec)
            + (end.tv_nsec - start.tv_nsec) / 1e9;
        remaining -= elapsed;

        if (key == 'q' || key == 'Q') return NAV_QUIT;
        if (key == 'r' || key == 'R') return NAV_RESTART;
        if (key == ' ')               return NAV_TOGGLE_PAUSE;
        if (key == '+' || key == '=') return NAV_FASTER;
        if (key == '-')               return NAV_SLOWER;
    }
    return NAV_TIMEOUT;
}

NavEvent waitDuringPause(void) {
    while (true) {
        const char key = readKey(-1);
        switch (key) {
            case 'q': case 'Q':       return NAV_QUIT;
            case 'r': case 'R':       return NAV_RESTART;
            case ' ':                 return NAV_TOGGLE_PAUSE;
            case 'h': case '[':       return NAV_BACK;
            case 'l': case ']':       return NAV_FORWARD;
            default: break;
        }
    }
}

void restoreTerminal(void) {
    const char sequence[] = SHOW_CURSOR EXIT_ALT_SCREEN;
    (void)write(STDOUT_FILENO, sequence, sizeof(sequence) - 1);
    if (termiosSaved) {
        tcsetattr(STDOUT_FILENO, TCSANOW, &originalTermios);
    }
}

void onSignal(const int signo) {
    restoreTerminal();
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
    size_t capacity = INITIAL_BUFFER_SIZE;
    size_t length = 0;
    char *buffer = malloc(capacity);
    if (buffer == NULL) {
        return NULL;
    }
    while (1) {
        if (length + 1 >= capacity) {
            capacity *= 2;
            char *resized = realloc(buffer, capacity);
            if (resized == NULL) {
                free(buffer);
                return NULL;
            }
            buffer = resized;
        }
        const size_t bytesRead = fread(buffer + length, 1, capacity - length - 1, stream);
        length += bytesRead;
        if (bytesRead == 0) {
            if (ferror(stream)) {
                free(buffer);
                return NULL;
            }
            break;
        }
    }
    buffer[length] = '\0';
    return buffer;
}

static struct {
    int rows;
    int cols;
} terminalSize = {.rows = DEFAULT_ROWS, .cols = DEFAULT_COLS};

void refreshTerminalSize(void) {
    if (!terminalResized) {
        return;
    }
    terminalResized = 0;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0) terminalSize.cols = ws.ws_col;
        if (ws.ws_row > 0) terminalSize.rows = ws.ws_row;
    }
}

int currentCols(void) {
    refreshTerminalSize();
    return terminalSize.cols;
}

int currentRows(void) {
    refreshTerminalSize();
    return terminalSize.rows;
}

void moveCursor(const int row, const int col) {
    printf("\033[%d;%dH", row, col);
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

int countWords(const char *text) {
    int count = 0;
    bool inWord = false;
    for (const char *walker = text; *walker; walker++) {
        if (isWordSeparator(*walker)) {
            inWord = false;
        } else if (!inWord) {
            count++;
            inWord = true;
        }
    }
    return count;
}

char *previousWordStart(char *text, char *current) {
    if (current <= text) return text;
    char *walker = current;
    while (walker > text && isWordSeparator(*(walker - 1))) {
        walker--;
    }
    while (walker > text && !isWordSeparator(*(walker - 1))) {
        walker--;
    }
    return walker;
}

int utf8SeqLen(const char *p) {
    const unsigned char leadByte = (unsigned char)*p;
    if ((leadByte & 0x80) == 0x00) return 1;
    if ((leadByte & 0xE0) == 0xC0) return 2;
    if ((leadByte & 0xF0) == 0xE0) return 3;
    if ((leadByte & 0xF8) == 0xF0) return 4;
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

void printSpaces(const int count) {
    for (int i = 0; i < count; i++) {
        putchar(' ');
    }
}

void printProgressBar(const long consumed, const long total, const int cols, const int secondsRemaining) {
    if (total <= 0 || cols <= 0) {
        return;
    }
    const int percent = (int)(100 * consumed / total);
    const int minutes = secondsRemaining / 60;
    const int seconds = secondsRemaining % 60;

    char tail[32];
    const int tailLen = snprintf(tail, sizeof(tail), "] %3d%% %d:%02d", percent, minutes, seconds);

    const int reserved = 1 + tailLen;
    if (cols < reserved + 1) {
        printf("%3d%% %d:%02d", percent, minutes, seconds);
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
    fputs(tail, stdout);
}

void printWpmLabel(const int wpm, const int cols, const int row) {
    char label[32];
    const int labelLen = snprintf(label, sizeof(label), "%d wpm", wpm);
    const int pad = cols / 2 > labelLen / 2 ? cols / 2 - labelLen / 2 : 0;
    moveCursor(row, 1);
    printSpaces(pad);
    fputs(label, stdout);
}

void printWordCentered(const char *buffer, const int wordLength, const int cols) {
    const WordMetrics metrics = measureWord(buffer, wordLength);
    int padding = cols / 2 - metrics.codePoints / 2;
    if (padding < 0) {
        padding = 0;
    }

    const int rest = wordLength - metrics.middleByteOffset - metrics.middleByteLength;
    printSpaces(padding);
    printf("%.*s" RED_COLOR "%.*s" RESET_COLOR "%.*s",
           metrics.middleByteOffset, buffer,
           metrics.middleByteLength, buffer + metrics.middleByteOffset,
           rest, buffer + metrics.middleByteOffset + metrics.middleByteLength);
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

int rampedWpm(const int target, const int rampIndex, const double rampStart) {
    if (rampIndex >= RAMP_WORDS) {
        return target;
    }
    const double progress = (double)rampIndex / RAMP_WORDS;
    const double factor = rampStart + (1.0 - rampStart) * progress;
    return (int)(target * factor);
}

double secondsForWpm(const int wpm) {
    return 60.0 / wpm;
}

void installSignalHandler(const int signo, void (*handler)(int), const int flags) {
    struct sigaction sa = {0};
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = flags;
    sigaction(signo, &sa, NULL);
}

void initTerminal(void) {
    atexit(restoreTerminal);
    installSignalHandler(SIGINT, onSignal, 0);
    installSignalHandler(SIGTERM, onSignal, 0);
    installSignalHandler(SIGWINCH, onWindowChange, SA_RESTART);
    prepareTerminalInput();
    openKeyboard();
    fputs(ENTER_ALT_SCREEN HIDE_CURSOR CLEAR_SCREEN, stdout);
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

    int wpm = options.wpm;
    int rampIndex = 0;
    double rampStart = INITIAL_RAMP_START;
    int wpmShowCounter = WPM_INDICATOR_WORDS;

    const char *end = text + strlen(text);
    while (end > text && isWordSeparator(*(end - 1))) {
        end--;
    }
    const long totalBytes = end - text;
    const int totalWords = countWords(text);
    int wordsRead = 0;

    char *cursor = text;
    bool paused = false;
    bool quit = false;

    while (*cursor && !quit) {
        while (isWordSeparator(*cursor)) {
            cursor++;
        }
        if (!*cursor) {
            break;
        }

        char *wordStart = cursor;
        const int wordLength = getWordLength(cursor);
        const bool needsExtraPause = isSentenceEnd(cursor[wordLength - 1])
            || nextIsParagraphBreak(cursor + wordLength);
        cursor += wordLength;

        const int cols = currentCols();
        const int rows = currentRows();
        const int wordRow = rows / 2 > 0 ? rows / 2 : 1;
        const int barRow = rows > 0 ? rows : 1;

        moveCursor(wordRow, 1);
        fputs(ERASE_BELOW, stdout);
        printWordCentered(wordStart, wordLength, cols);

        if (wpmShowCounter > 0 && wordRow + 2 < barRow) {
            printWpmLabel(wpm, cols, wordRow + 2);
        }

        const int wordsRemaining = totalWords > wordsRead ? totalWords - wordsRead : 0;
        const int secondsRemaining = (int)(wordsRemaining * 60.0 / wpm);

        moveCursor(barRow, 1);
        printProgressBar(cursor - text, totalBytes, cols, secondsRemaining);
        fflush(stdout);

        NavEvent event;
        if (paused) {
            event = waitDuringPause();
        } else {
            const int effectiveWpm = rampedWpm(wpm, rampIndex, rampStart);
            const double base = secondsForWpm(effectiveWpm);
            const double duration = needsExtraPause ? base * EXTRA_PAUSE_FACTOR : base;
            event = waitDuringWord(duration);
        }

        switch (event) {
            case NAV_QUIT:
                quit = true;
                break;
            case NAV_TOGGLE_PAUSE:
                paused = !paused;
                if (paused) {
                    cursor = wordStart;
                } else {
                    rampIndex = 0;
                    rampStart = RESUME_RAMP_START;
                }
                break;
            case NAV_BACK:
                cursor = previousWordStart(text, wordStart);
                if (wordsRead > 0) wordsRead--;
                break;
            case NAV_RESTART:
                cursor = text;
                rampIndex = 0;
                rampStart = INITIAL_RAMP_START;
                wordsRead = 0;
                wpmShowCounter = WPM_INDICATOR_WORDS;
                break;
            case NAV_FASTER:
                if (wpm + WPM_STEP <= MAX_WPM) wpm += WPM_STEP;
                cursor = wordStart;
                wpmShowCounter = WPM_INDICATOR_WORDS;
                break;
            case NAV_SLOWER:
                if (wpm - WPM_STEP >= MIN_WPM) wpm -= WPM_STEP;
                cursor = wordStart;
                wpmShowCounter = WPM_INDICATOR_WORDS;
                break;
            case NAV_FORWARD:
            case NAV_TIMEOUT:
                if (rampIndex < RAMP_WORDS) rampIndex++;
                if (wpmShowCounter > 0) wpmShowCounter--;
                wordsRead++;
                break;
        }
    }

    free(text);
    return 0;
}
