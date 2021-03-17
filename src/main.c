/**
 ** This file is part of the kilo project.
 ** Copyright 2021 Khaïs Colin <khais.colin@gmail.com>.
 **
 ** This program is free software: you can redistribute it and/or modify
 ** it under the terms of the GNU General Public License as published by
 ** the Free Software Foundation, either version 3 of the License, or
 ** (at your option) any later version.
 **
 ** This program is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 ** GNU General Public License for more details.
 **
 ** You should have received a copy of the GNU General Public License
 ** along with this program.  If not, see <http://www.gnu.org/licenses/>.
 **/

/*** includes ***/

#define _DEFAULT_SOURCE // needed for getline to work
#define _BSD_SOURCE     // needed for getline to work
#define _GNU_SOURCE     // needed for getline to work

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termio.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define VERSION "0.0.1"
#define TAB_STOP 4
#define QUIT_TIMES 3

#define CTRL_KEY(k) ((k)&0x1f)

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    HOME,
    END,
    DEL,
    PAGE_UP,
    PAGE_DOWN,
};

/*** data ***/

typedef struct erow {
    int size;
    int rsize;
    char* chars;
    char* render;
} erow;

struct EditorConfig {
    int cx, cy;
    int rx;
    int rowoffset;
    int columnoffset;
    int screenrows;
    int screencolumns;
    int numrows;
    erow* row;
    int dirty;
    char* filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios original_termios;
};

struct EditorConfig E;

/*** prototypes ***/

void editor_set_status_message(const char* fmt, ...);
void editor_refresh_screen();
char* editor_prompt(char* prompt, void (*callback)(char*, int));

/*** terminal ***/

void die(const char* s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disable_raw_mode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.original_termios) == -1) {
        die("tcsetattr");
    }
}

void enable_raw_mode() {
    if (tcgetattr(STDIN_FILENO, &E.original_termios) == -1)
        die("tcgetattr");
    atexit(disable_raw_mode);

    struct termios raw = E.original_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

int editor_read_key() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN)
            die("read");
    }

    if (c == '\033') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return c;
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return c;

        if (seq[0] == '[') { // This is supossed to handle pagup and pagedown, but for some reason this does not work on my terminal emulator
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                    return c;
                if (seq[2] == '~') {
                    switch (seq[1]) {
                    case '1':
                        return HOME;
                    case '3':
                        return DEL;
                    case '4':
                        return END;
                    case '5':
                        return PAGE_UP;
                    case '6':
                        return PAGE_DOWN;
                    case '7':
                        return HOME;
                    case '8':
                        return END;
                    }
                }
            } else {
                switch (seq[1]) {
                case 'A':
                    return ARROW_UP;
                case 'B':
                    return ARROW_DOWN;
                case 'C':
                    return ARROW_RIGHT;
                case 'D':
                    return ARROW_LEFT;
                case 'H':
                    return HOME;
                case 'F':
                    return END;
                case '5':
                    return PAGE_UP;
                case '6':
                    return PAGE_DOWN;
                case '7':
                    return HOME;
                case '8':
                    return END;
                }
            }
        } else if (seq[0] == '0') {
            switch (seq[1]) {
            case '5':
                return PAGE_UP;
            case '6':
                return PAGE_DOWN;
            case 'H':
                return HOME;
            case 'F':
                return END;
            }
        }

        return c;
    } else if (c == '~') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return c;
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return c;
        if (seq[0] == '[') { // This is supossed to handle pagup and pagedown, but for some reason this does not work on my terminal emulator
            if (read(STDIN_FILENO, &seq[2], 1) != 1)
                return c;
            if (seq[2] == '~') {
                switch (seq[1]) {
                case '5':
                    return PAGE_UP;
                case '6':
                    return PAGE_DOWN;
                }
            }
        }
    }
    return c;
}

int get_cursor_position(int* rows, int* cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }

    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1;

    return 0;
}

int get_window_size(int* rows, int* cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        return get_cursor_position(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/

int editor_row_cx_to_rx(erow* row, int cx) {
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t')
            rx += (TAB_STOP - 1) - (rx % TAB_STOP);
        rx++;
    }
    return rx;
}

int editor_row_rx_to_cx(erow* row, int rx) {
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t') {
            cur_rx += (TAB_STOP - 1) - (cur_rx % TAB_STOP);
        }
        cur_rx++;

        if (cur_rx > rx) {
            return cx;
        }
    }
    return cx;
}

void editor_update_row(erow* row) {
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++)
        if (row->chars[j] == '\t')
            tabs++;

    free(row->render);
    row->render = malloc(row->size + tabs * (TAB_STOP - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TAB_STOP != 0) {
                row->render[idx++] = ' ';
            }
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editor_insert_row(int at, char* s, size_t len) {
    if (at < 0 || at > E.numrows) {
        return;
    }

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editor_update_row(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editor_append_row(char* s, size_t len) {
    editor_insert_row(E.numrows, s, len);
}

void editor_free_row(erow* row) {
    free(row->render);
    free(row->chars);
}

void editor_del_row(int at) {
    if (at < 0 || at >= E.numrows) {
        return;
    }
    editor_free_row(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

void editor_row_insert_char(erow* row, int at, int c) {
    if (at < 0 || at > row->size)
        at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editor_update_row(row);
    E.dirty++;
}

void editor_row_append_string(erow* row, char* s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editor_update_row(row);
    E.dirty++;
}

void editor_row_delete_char(erow* row, int at) {
    if (at < 0 || at >= row->size)
        return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editor_update_row(row);
    E.dirty++;
}

/** editor operations **/

void editor_insert_char(int c) {
    if (E.cy == E.numrows) {
        editor_append_row("", 0);
    }
    editor_row_insert_char(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editor_insert_new_line() {
    if (E.cx == 0) {
        editor_insert_row(E.cy, "", 0);
    } else {
        erow* row = &E.row[E.cy];
        editor_insert_row(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editor_update_row(row);
    }
    E.cy++;
    E.cx = 0;
}

void editor_del_char() {
    if (E.cy == E.numrows) {
        return;
    }
    if (E.cx == 0 && E.cy == 0) {
        return;
    }

    erow* row = &E.row[E.cy];
    if (E.cx > 0) {
        editor_row_delete_char(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy - 1].size;
        editor_row_append_string(&E.row[E.cy - 1], row->chars, row->size);
        editor_del_row(E.cy);
        E.cy--;
    }
}

/*** file i/o ***/

char* editor_rows_to_string(int* buflen) {
    int totlen = 0;
    int j;
    for (j = 0; j < E.numrows; j++) {
        totlen += E.row[j].size + 1;
    }
    *buflen = totlen;

    char* buf = malloc(totlen);
    char* p = buf;
    for (j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n'; // we only handle LF-style files for now
        p++;
    }

    return buf;
}

void editor_open(char* filename) {
    free(E.filename);
    E.filename = strdup(filename);

    FILE* fp = fopen(filename, "r");
    if (!fp)
        die("fopen");

    char* line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;

        editor_append_row(line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editor_save() {
    if (E.filename == NULL) {
        E.filename = editor_prompt("Save as: %s", NULL);
        if (E.filename == NULL) {
            editor_set_status_message("Saving canceled");
            return;
        }
    }

    int len;
    char* buf = editor_rows_to_string(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                editor_set_status_message("\"%s\" %dL, %db written", E.filename, E.numrows, len);
                return;
            }
        }
        close(fd);
    }

    free(buf);
    editor_set_status_message("Write error: %s", strerror(errno));
}

/*** find ***/

void editor_find_callback(char* query, int key) {
    static int last_match = -1;
    static int direction = 1;

    if (key == '\r' || key == '\x1b') {
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }

    if (query == NULL) {
        return;
    }

    if (last_match == 1) {
        direction = 1;
    }
    int current = last_match;
    int i;
    for (i = 0; i < E.numrows; i++) {
        current += direction;
        if (current == -1) {
            current = E.numrows - 1;
        } else if (current == E.numrows) {
            current = 0;
        }
        erow* row = &E.row[current];
        char* match = strstr(row->render, query);
        if (match) {
            last_match = current;
            E.cy = current;
            E.cx = editor_row_rx_to_cx(row, match - row->render);
            //            E.rowoffset = E.numrows; // TODO
            break;
        }
    }
}

void editor_find() {
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_columnoffset = E.columnoffset;
    int saved_rowoffset = E.rowoffset;

    char* query = editor_prompt("Search: %s", editor_find_callback);

    if (query) {
        free(query);
    } else {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.columnoffset = saved_columnoffset;
        E.rowoffset = saved_rowoffset;
    }
}

/*** append buffer ***/

struct AppendBuffer {
    char* buffer;
    int length;
};

#define APPEND_BUFFER_INIT \
    { NULL, 0 }

void ab_append(struct AppendBuffer* append_buffer, const char* s, int len) {
    char* new = realloc(append_buffer->buffer, append_buffer->length + len);

    if (new == NULL)
        return;
    memcpy(&new[append_buffer->length], s, len);
    append_buffer->buffer = new;
    append_buffer->length += len;
}

void ab_free(struct AppendBuffer* append_buffer) {
    free(append_buffer->buffer);
}

/*** output ***/

void editor_scroll() {
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editor_row_cx_to_rx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.rowoffset) {
        E.rowoffset = E.cy;
    }
    if (E.cy >= E.rowoffset + E.screenrows) {
        E.rowoffset = E.cy - E.screenrows + 1;
    }
    if (E.rx < E.columnoffset) {
        E.columnoffset = E.rx;
    }
    if (E.rx >= E.columnoffset + E.screencolumns) {
        E.columnoffset = E.rx - E.screencolumns + 1;
    }
}

void editor_draw_rows(struct AppendBuffer* ab) {
    int y;
    for (y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoffset;
        if (filerow >= E.numrows) {
            /*** welcome ***/
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcome_len = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", VERSION);
                if (welcome_len > E.screencolumns)
                    welcome_len = E.screencolumns;
                int padding = (E.screencolumns - welcome_len) / 2;
                if (padding) {
                    ab_append(ab, "~", 1);
                    padding--;
                }
                while (padding--)
                    ab_append(ab, " ", 1);
                ab_append(ab, welcome, welcome_len);

            } else if (E.numrows == 0 && y == 2 + E.screenrows / 3) {
                char licence[80];
                int licence_len = snprintf(licence, sizeof(licence), "Copright 2021 Khaïs COLIN -- GNU GPL 3.0");
                if (licence_len > E.screencolumns)
                    licence_len = E.screencolumns;
                int padding = (E.screencolumns - licence_len) / 2;
                if (padding) {
                    ab_append(ab, "~", 1);
                    padding--;
                }
                while (padding--)
                    ab_append(ab, " ", 1);
                ab_append(ab, licence, licence_len);
                /*** end welcome ***/

            } else {
                ab_append(ab, "~", 1);
            }

        } else {
            int len = E.row[filerow].rsize - E.columnoffset;
            if (len < 0)
                len = 0;
            if (len > E.screencolumns)
                len = E.screencolumns;
            char* c = &E.row[filerow].render[E.columnoffset];
            int j;
            for (j = 0; j < len; j++) {
                if (isdigit(c[j])) {
                    ab_append(ab, "\x1b[31m", 5);
                    ab_append(ab, &c[j], 1);
                    ab_append(ab, "\x1b[39m", 5);
                } else {
                    ab_append(ab, &c[j], 1);
                }
            }
        }

        ab_append(ab, "\x1b[K", 3);
        ab_append(ab, "\r\n", 2);
    }
}

void editor_draw_status_bar(struct AppendBuffer* ab) {
    ab_append(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s %s", E.filename ? E.filename : "[No Name]", E.dirty ? "[+]" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d %d", E.cy + 1, E.numrows, E.cx);
    if (len > E.screencolumns)
        len = E.screencolumns;
    ab_append(ab, status, len);
    while (len < E.screencolumns) {
        if (E.screencolumns - len == rlen) {
            ab_append(ab, rstatus, rlen);
            break;
        } else {
            ab_append(ab, " ", 1);
            len++;
        }
    }
    ab_append(ab, "\x1b[m", 3);
    ab_append(ab, "\r\n", 2);
}

void editor_draw_message_bar(struct AppendBuffer* ab) {
    ab_append(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencolumns)
        msglen = E.screencolumns;
    if (msglen && time(NULL) - E.statusmsg_time < 5) {
        ab_append(ab, E.statusmsg, msglen);
    }
}

void editor_refresh_screen() {
    editor_scroll();

    struct AppendBuffer ab = APPEND_BUFFER_INIT;

    ab_append(&ab, "\x1b[?25l", 6);
    ab_append(&ab, "\x1b[H", 3);

    editor_draw_rows(&ab);
    editor_draw_status_bar(&ab);
    editor_draw_message_bar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoffset) + 1, (E.rx - E.columnoffset) + 1);
    ab_append(&ab, buf, strlen(buf));

    ab_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.buffer, ab.length);
    ab_free(&ab);
}

void editor_set_status_message(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** input ***/

char* editor_prompt(char* prompt, void (*callback)(char*, int)) {
    size_t bufsize = 128;
    char* buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';
    while (1) {
        editor_set_status_message(prompt, buf);
        editor_refresh_screen();

        int c = editor_read_key();
        if (c == DEL || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) {
                buf[--buflen] = '\0';
            }
        } else if (c == '\x1b') {
            editor_set_status_message("Canceled");
            if (callback) {
                callback(buf, c);
            }
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editor_set_status_message("");
                if (callback) {
                    callback(buf, c);
                }
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
        if (callback) {
            callback(buf, c);
        }
    }
}

void editor_move_cursor(int key) {
    erow* row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
    case ARROW_LEFT:
        if (E.cx != 0) {
            E.cx--;
        }
        break;
    case ARROW_RIGHT:
        if (row && E.cx < row->size) {
            E.cx++;
        }
        break;
    case ARROW_DOWN:
        if (E.cy != E.numrows - 1) {
            E.cy++;
        }
        break;
    case ARROW_UP:
        if (E.cy != 0) {
            E.cy--;
        }
        break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

void editor_process_keypress() {
    static int quit_times = QUIT_TIMES;

    int c = editor_read_key();

    switch (c) {
    case '\r':
        editor_insert_new_line();
        break;
    case CTRL_KEY('q'):
        if (E.dirty && quit_times > 0) {
            editor_set_status_message("No write since last change, quit again %d more times to ignore", quit_times);
            quit_times--;
            return;
        }
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;
    case CTRL_KEY('s'):
        editor_save();
        break;
    case HOME:
        E.cx = 0;
        break;
    case END:
        if (E.cy < E.numrows)
            E.cx = E.row[E.cy].size;
        break;
    case CTRL_KEY('f'):
        editor_find();
        break;
    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL:
        if (c == DEL) {
            editor_move_cursor(ARROW_RIGHT);
        }
        editor_del_char();
        break;
    case PAGE_UP:
    case PAGE_DOWN: {
        if (c == PAGE_UP) {
            E.cy = E.rowoffset;
        } else if (c == PAGE_DOWN) {
            E.cy = E.rowoffset - 1;
            if (E.cy > E.numrows)
                E.cy = E.numrows;
        }

        int times = E.screenrows;
        while (times--) {
            editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
        break;
    }
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
    case ARROW_UP:
        editor_move_cursor(c);
        break;
    case CTRL_KEY('l'):
    case '\x1b':
        break;
    default:
        editor_insert_char(c);
        break;
    }

    quit_times = QUIT_TIMES;
}

/*** init ***/

void init_editor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoffset = 0;
    E.columnoffset = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if (get_window_size(&E.screenrows, &E.screencolumns) == -1)
        die("get_window_size");
    E.screenrows -= 2;
}

int main(int argc, char* argv[]) {
    enable_raw_mode();
    init_editor();
    if (argc >= 2) {
        editor_open(argv[1]);
    }

    editor_set_status_message("-- INSERT --");

    while (1) {
        editor_refresh_screen();
        editor_process_keypress();
    }

    return 0;
}
