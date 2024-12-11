// Stop compiler from complaining about getline()
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** define ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8

#define CRTL_KEY(k) ((k) & 0x1f) // 1 = 0001, f = 1111 => 00011111 in binary 

enum editorKey {
  BACKSPACE = 127,
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
  int rsize;
  char *chars;
  char *render;
} editor_row;

struct editorConfig {
  int cursor_x, cursor_y;
  int render_x;
  int row_offset;
  int col_offset;
  int screen_rows;
  int screen_cols;
  int numrows;
  editor_row *row;
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
  struct termios orig_termios;
};

struct editorConfig E;

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);

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

// Convert chars index to render index
// Loop though left of cursor_x to find many space to find correct cursor placement
int editorRowCursorXToRenderX(editor_row *row, int cursor_x) {
  int render_x = 0;
  int j;
  for (j = 0; j < cursor_x; j++) {
    if (row->chars[j] == '\t')
      render_x += (KILO_TAB_STOP - 1) - (render_x % KILO_TAB_STOP);
    render_x++;
  }
  return render_x;
}

// First loop is find all the tabs in a row
// allocate memory to fit mulitple white space instead of tab bytes
// then loop to copy from row->chars to row->render to manipulate tabs size
void editorUpdateRow(editor_row *row) {
  int tabs = 0;
  int j;
  // Check how many tab there are for accuraty allocate memory with tabs size
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') tabs++;
  }

  free(row->render);
  row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

  int idx = 0;
  for (j = 0; j < row->size; j++) {
    // Loop to find the tabs size then replace tab with whitespace
    // with KILO_TAB_STOP to control how many space tabs have
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}

void editorAppendRow(char *s, size_t len) {
  E.row = realloc(E.row, sizeof(editor_row) * (E.numrows + 1));

  int at = E.numrows;
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editorUpdateRow(&E.row[at]);

  E.numrows++;
}

void editorRowInsertChar(editor_row *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editorUpdateRow(row);
}

/*** Editor Operations ***/

void editorInsertChar(int c) {
  if (E.cursor_y == E.numrows) {
    editorAppendRow("", 0);
  }
  editorRowInsertChar(&E.row[E.cursor_y], E.cursor_x, c);
  E.cursor_x++;
}

/*** File I/O ***/

char *editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;
  for (j = 0; j < E.numrows; j++)
    totlen += E.row[j].size + 1;
  *buflen = totlen;

  char *buf = malloc(totlen);
  char *p = buf;
  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }

  return buf;
}

void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);

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

void editorSave() {
  if (E.filename == NULL) return;

  int len;
  char *buf = editorRowsToString(&len);

  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) != -1) {
        close(fd);
        free(buf);
        editorSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }

  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
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

void editorScroll() {
  E.render_x = 0;
  if (E.cursor_y < E.numrows) {
    E.render_x = editorRowCursorXToRenderX(&E.row[E.cursor_y], E.cursor_x);
  }

  // Check if cursor move above the visible area
  if (E.cursor_y < E.row_offset) {
    E.row_offset = E.cursor_y;
  }
  // Then check if cursor move below the visible area
  if (E.cursor_y >= E.row_offset + E.screen_rows) {
    E.row_offset = E.cursor_y - E.screen_rows + 1;
  }
  // Change to render_x because the tabs size is whitespace instead of \t
  if (E.render_x < E.col_offset) {
    E.col_offset = E.render_x;
  }
  if (E.render_x >= E.col_offset + E.screen_cols) {
    E.col_offset = E.render_x - E.screen_cols + 1;
  }
}

void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screen_rows; y++) {
    int file_row = y + E.row_offset;
    if (file_row >= E.numrows) {
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
      int len = E.row[file_row].rsize - E.col_offset;
      if (len < 0) len = 0;
      if (len > E.screen_cols) len = E.screen_cols;
      abAppend(ab, &E.row[file_row].render[E.col_offset], len);
    }

    // K is delete in line and 0 args is erase right of cursor
    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  // m command is Select Graphic Rendition
  // 1 bold, 4 underscore, 5 blink, 7 inverted colors
  abAppend(ab, "\x1b[7m", 4);

  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines",
                     E.filename ? E.filename : "[No name]", E.numrows);
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
                      E.cursor_y + 1, E.numrows);
  if (len > E.screen_cols) len = E.screen_cols;
  abAppend(ab, status, len);
  while (len < E.screen_cols) {
    if (E.screen_cols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screen_cols) msglen = E.screen_cols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);
}

// Append to buffer before write it to terminal
void editorRefreshScreen() {
  editorScroll();

  struct abuf ab = ABUF_INIT;

  // l command => Reset mode
  // h command => Set mode
  // ?25 arguments hide/showing cursor
  abAppend(&ab, "\x1b[?25l", 6); // hide cursor
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  // This buffer instruct terminal to move cursor supplied coordinated
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cursor_y - E.row_offset) + 1, 
                                            (E.render_x - E.col_offset) + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6); // show cursor

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

/*** input ***/

void editorMoveCursor(int key) {
  // check the cursor if it on the actual line if it is row will point to editor_row[E.cursor_y]
  editor_row *row = (E.cursor_y >= E.numrows) ? NULL : &E.row[E.cursor_y];

  switch (key) {
    case ARROW_UP:
      if (E.cursor_y != 0) {
        E.cursor_y--;
      }
      break;
    case ARROW_DOWN:
      if (E.cursor_y < E.numrows) {
        E.cursor_y++;
      }
      break;
    case ARROW_LEFT:
      if (E.cursor_x != 0) {
        E.cursor_x--;
      } else if (E.cursor_y > 0) {
        E.cursor_y--;
        E.cursor_x = E.row[E.cursor_y].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.cursor_x < row->size) {
        E.cursor_x++;
      } else if (row && E.cursor_x == row->size) {
        E.cursor_y++;
        E.cursor_x = 0;
      }
      break;
  }

  row = (E.cursor_y >= E.numrows) ? NULL : &E.row[E.cursor_y];
  int rowlen = row ? row->size : 0;
  if (E.cursor_x > rowlen) {
    E.cursor_x = rowlen;
  }
}

void editorProcessKeypress() {
  int c = editorReadKey();

  switch (c) {
    case '\r':
      /* TODO */
      break;

    case CRTL_KEY('q'): // ASCII 17 | 0x11
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;

    case CRTL_KEY('s'):
      editorSave();
      break;

    case HOME_KEY:
      E.cursor_x = 0;
      break;

    case END_KEY:
      if (E.cursor_y < E.numrows)
        E.cursor_x = E.row[E.cursor_y].size;
      break;

    case BACKSPACE:
    case CRTL_KEY('h'):
    case DEL_KEY:
      /* TODO */
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        if (c == PAGE_UP) {
          E.cursor_y = E.row_offset;
        } else if (c == PAGE_DOWN) {
          E.cursor_y = E.row_offset + E.screen_rows - 1;
          if (E.cursor_y > E.numrows) E.cursor_y = E.numrows;
        }

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

    case CRTL_KEY('l'):
    case '\x1b':
      break;

    default:
      editorInsertChar(c);
      break;
  }
}

/*** init ***/

void initEditor() {
  E.cursor_x = 0;
  E.cursor_y = 0;
  E.render_x = 0;
  E.numrows = 0;
  E.row_offset = 0;
  E.row = NULL;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;

  if (getWindowSize(&E.screen_rows, &E.screen_cols) == -1) die("getWindowSize");
  E.screen_rows -= 2;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  editorSetStatusMessage("HELP: Ctrl-S = save | Crtl-Q = quit");

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
