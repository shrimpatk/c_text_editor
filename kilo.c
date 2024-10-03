/*** includes ***/

// Stop compiler from complaining about getline()
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/*** define ***/

#define KILO_VERSION "0.0.1"
#define CRTL_KEY(k) ((k) & 0x1f) // 1 = 0001, f = 1111 => 00011111 in binary 

enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/*** data ***/

typedef struct editor_row {
    int size;
    char *chars;
} editor_row;

struct editorConfig {
    int cursor_x, cursor_y;
    int screen_rows;
    int screen_cols;
    int numrows;
    editor_row *row;
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

int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) { // nread = number of byte
    if (nread == -1 && errno != EAGAIN) die("read");
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    // Check if it have [ and then check if it PAGE_UP or ARROW_KEY if not return ESC
    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      } else {
          switch (seq[1]) {
            case 'A': return ARROW_UP;
            case 'B': return ARROW_DOWN;
            case 'C': return ARROW_RIGHT;
            case 'D': return ARROW_LEFT;
            case 'H': return HOME_KEY;
            case 'F': return END_KEY;
          }
        }
      } else if (seq[0] == 'O') {
        switch (seq[1]) {
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
      return '\x1b';
  } else {
      return c;
  }
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

/*** Row Operations ***/

void editorAppendRow(char *s, size_t len) {
  E.row = realloc(E.row, sizeof(editor_row) * (E.numrows + 1));

  int at = E.numrows;
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.numrows++;
}

/*** File I/O ***/

void editorOpen(char *filename) {
  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");

  char *line = NULL;
  size_t line_cap = 0;
  ssize_t line_length;

  line_length = getline(&line, &line_cap, fp); // will return -1 if it the end of the file

  while ((line_length = getline(&line, &line_cap, fp)) != -1) {
    while(line_length > 0 && (line[line_length - 1] == '\n' || 
                              line[line_length - 1] == '\r'))
      line_length--;
    editorAppendRow(line, line_length);
  }
  free(line);
  fclose(fp);
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
    if (y >= E.numrows) {
      if (E.numrows == 0 && y == (E.screen_rows / 2) - 1) {
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
    } else {
        // if line size is greater than editor_cols cap that shit then append to buffer
        int len = E.row[y].size;
        if (len > E.screen_cols) len = E.screen_cols;
        abAppend(ab, E.row[y].chars, len);
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
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);

  // This buffer instruct terminal to move cursor supplied coordinated
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cursor_y + 1, E.cursor_x + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6); // show cursor

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/*** input ***/

void editorMoveCursor(int key) {
  switch (key) {
    case ARROW_UP:
      if (E.cursor_y != 0) {
        E.cursor_y--;
      }
      break;
    case ARROW_DOWN:
      if (E.cursor_y != E.screen_rows - 1) {
        E.cursor_y++;
      }
      break;
    case ARROW_LEFT:
      if (E.cursor_x != 0) {
        E.cursor_x--;
      }
      break;
    case ARROW_RIGHT:
      if (E.cursor_x != E.screen_cols - 1) {
        E.cursor_x++;
      }
      break;
  }
}

void editorProcessKeypress() {
  int c = editorReadKey();

  switch (c) {
    case CRTL_KEY('q'): // ASCII 17 | 0x11
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;

    case HOME_KEY:
      E.cursor_x = 0;
      break;

    case END_KEY:
      E.cursor_x = E.screen_cols - 1;
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        int times = E.screen_rows;
        while (times--)
          editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;
  }
}

/*** init ***/

void initEditor() {
  E.cursor_x = 0;
  E.cursor_y = 0;
  E.numrows = 0;

  if (getWindowSize(&E.screen_rows, &E.screen_cols) == -1) die("getWindowSize");
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}