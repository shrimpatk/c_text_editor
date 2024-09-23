/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** define ***/

#define KILO_VERSION "0.0.1"
#define CRTL_KEY(k) ((k) & 0x1f) // 1 = 0001, f = 1111 => 00011111 in binary 

/*** data ***/

struct editorConfig {
    int screen_rows;
    int screen_cols;
    struct termios orig_termios;
};

struct editorConfig E;


/*** terminal ***/

void die(const char *s) {
    // "\x1b" = escape sequence follow by '[' and command
    write(STDOUT_FILENO, "\x1b[2J", 4); // clear the screen
    write(STDOUT_FILENO, "\x1b[H", 3);  // move cursor up at the top

    perror(s);
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;

    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag &= ~(CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

    // minimum time before read() can return
    raw.c_cc[VMIN] = 0;
    // maximum amount ot time to wait for read() 1/10 of a second / 100ms
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

char editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) { // nread = number of byte
        if (nread == -1 && errno != EAGAIN) die("read");
    }
    return c;
}

// Fallback when ioctl() didn't work as expected
int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    // n command => query device status report
    // 6 argument => ask for cursor position 
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    // String will always end with \0
    buf[i] = '\0';

    // check if it contain escape sequence
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    // read from &buf[2] and then parse it to rows and cols
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        editorReadKey();
        // if ioctl() failed to get winsize use getCursorPosition() as a fallback
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** Append Buffer ***/

struct abuf {
    char *b; // pointer point to buffer
    int len;
};

#define ABUF_INIT {NULL, 0} // Empty buffer act as a constructor for abuf type

void abAppend(struct abuf *ab, const char *s, int len) {
    // ab->b == (*ab).b access b from ab pointer and the same with (*ab).len
    // realloc() accept two args pointer and new size
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;

    // memcpy() copy block of memory to another location
    // accept 3 args 
    // 1. where to copy to - &new[ab->len]
    // 2. source pointer - s
    // 3. number of bytes to copy - len
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

/*** output ***/

void editorDrawRows(struct abuf *ab) {
    int y;
    for (y = 0; y < E.screen_rows; y++) {
        if (y == (E.screen_rows / 2) - 1) {
            char welcome[80];

            int welcome_len = snprintf(welcome, sizeof(welcome),
            "What is this sorcery -- version %s", KILO_VERSION);

            if (welcome_len > E.screen_cols) welcome_len = E.screen_cols;
            
            // padding value for one side
            int padding = (E.screen_cols - welcome_len) / 2; 

            // if append any char or whitespace padding--
            // loop till there no padding left
            // then append welcome message to buffer
            if (padding) {
                abAppend(ab, "~", 1);
                padding--;
            }

            while (padding--) abAppend(ab, " ", 1);
            abAppend(ab, welcome, welcome_len);
        } else {
            abAppend(ab, "~", 1);
        }

        // K is delete in line and 0 args is erase right of cursor
        abAppend(ab, "\x1b[K", 3);
        if (y < E.screen_rows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

// Append to buffer before write it to terminal
void editorRefreshScreen() {
    struct abuf ab = ABUF_INIT;

    // l command => Reset mode
    // h command => Set mode
    // ?25 arguments hide/showing cursor
    abAppend(&ab, "\x1b[?25l", 6); // hide cursor
    // abAppend(&ab, "\x1b[2J", 4); 
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    abAppend(&ab, "\x1b[H", 3);
    abAppend(&ab, "\x1b[?25h", 6); // show cursor

    // Write buffer to terminal and using string and len from buf then clear ab buffer memory
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** input ***/

void editorProcessKeypress() {
    char c = editorReadKey();

    switch (c) {
        case CRTL_KEY('q'): // ASCII 17 | 0x11
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);

            exit(0);
            break;
    }
}

/*** init ***/

void initEditor() {
    if (getWindowSize(&E.screen_rows, &E.screen_cols) == -1) die("getWindowSize");
}

int main() {
    enableRawMode();
    initEditor();

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}