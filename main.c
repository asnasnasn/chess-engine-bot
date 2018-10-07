#include <X11/Xlib.h>
#include <X11/X.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <sys/types.h>
#include <signal.h>
#include <xdo.h>

#define xcall(f, ...) ({int r=f(__VA_ARGS__);if(r<0){perror(#f);exit(1);}r;})

void pr88(uint64_t);
void *xmalloc(size_t);
bool color_is_board(uint64_t);
char *get_move();
void sync_boards();
void kill_fish();
void kill_validator();
void prepare();
void read_from_fd(int, char *, int);
void read_line_from_fd(int, char *, int);
void write_to_fd(int, const char *);
void go();
void init();
void clean();

const uint64_t white = 0xf0d9b5, black = 0xb58863;
const uint64_t marked_white = 0xf7ec74, marked_black = 0xdac34b;
int flip = 0;
int turn = 0;
char *game[0x100];
int len = 0;
int fish_pid;
int fish_in;
int fish_out;
int validator_pid;
int validator_in;
int validator_out;
Display *display;
xdo_t *xdo;

void
pr88(uint64_t x)
{
    for (uint32_t i = 0; i < 8; i++) {
        for (uint32_t j = 0; j < 8; j++) {
            if (x & (1ull << ((7-i)*8+j))) {
                putchar('1');
            } else {
                putchar('0');
            }
        }
        putchar('\n');
    }
}

void *
xmalloc(size_t len)
{
    void *p = malloc(len);
    if (!p) {
        fprintf(stderr, "error: could not allocate memory\n");
        exit(1);
    }
    return p;
}

bool
color_is_board(uint64_t c)
{
    return c == white || c == black || c == marked_white || c == marked_black;
}

char *
opplm()
{
    Window root = DefaultRootWindow(display);
    XWindowAttributes wa;
    XGetWindowAttributes(display, root, &wa);
    int w = wa.width, h = wa.height;
    XImage *image = XGetImage(display, root, 0, 0, w, h, AllPlanes, ZPixmap);

    uint64_t occ = 0;
    uint32_t sq = 0x47;
    uint64_t prev_sq_c = -1;
    uint32_t src = -1, dst = -1;
    int prev_sq_y = -1;
    for (int y = 0; y < h; y++) {
        int bs = -1, be = -1;
        for (int x = 0; x < w; x++) {
            uint64_t pixel = XGetPixel(image, x, y);
            if (color_is_board(pixel)) {
                if (bs == -1) {
                    bs = x;
                }
                be = x+1;
            }
        }
        for (int x = 0; x < w; x++) {
            uint64_t pixel = XGetPixel(image, x, y);
            uint64_t sq_c = pixel;
            if (sq_c == marked_white) {
                sq_c = white;
            } else if (sq_c == marked_black) {
                sq_c = black;
            }
            if (sq_c != white && sq_c != black) {
                if (bs <= x && x < be) {
                    occ |= 1ull << sq;
                }
                continue;
            }
            if (prev_sq_y != -1 && sq_c != prev_sq_c) {
                sq++;
                // printf("sq : %u\n", sq);
            }
            if (y > prev_sq_y) {
                if (sq_c != prev_sq_c && prev_sq_y != -1) {
                    sq -= 0x8;
                    // printf("sq : %u\n", sq);
                } else {
                    sq -= 0xf;
                    // printf("sq : %u\n", sq);
                }
            }
            if (pixel == marked_white || pixel == marked_black) {
                // printf("## %d %d\n", y, x);
                if (src == -1) {
                    // printf("## %d %d\n", y, x);
                    src = sq;
                } else {
                    if (dst == -1) {
                        // printf("## %d %d\n", y, x);
                    }
                    dst = sq;
                }
            }
            prev_sq_c = sq_c;
            prev_sq_y = y;
        }
    }
    XDestroyImage(image);
    XFlush(display);
    if (src == -1 || dst == -1) {
        return 0;
    }
    // printf("## %u %u\n", src, dst);
    if (occ & (1ull << src)) {
        uint32_t t = src;
        src = dst;
        dst = t;
    }
    // pr88(occ);
    // printf("## r %u %u\n", src, dst);
    // printf("their move : %u %u\n", src, dst);
    // assert((occ & (1ull << src)) == 0);
    if (flip) {
        src = 0x3f - src;
        dst = 0x3f - dst;
    }
    char *str = xmalloc(5);
    str[0] = 'a' + (src & 7);
    str[1] = '1' + (src >> 3);
    str[2] = 'a' + (dst & 7);
    str[3] = '1' + (dst >> 3);
    str[4] = 0;
    return str;
}

char *
get_move()
{
    if (turn == flip) {
        static char *ponder_move = 0;
        bool ponderhit = false;
        if (ponder_move) {
            if (len > 0 && !strcmp(ponder_move, game[len-1])) {
                ponderhit = true;
                printf("      ponderhit!\n");
                write_to_fd(fish_in, "ponderhit\n");
            } else {
                write_to_fd(fish_in, "stop\n");
                char buf[0x100];
                while (1) {
                    read_line_from_fd(fish_out, buf, 0x100);
                    fprintf(stderr, "fish : %s", buf);
                    if (!strncmp(buf, "bestmove", 8)) {
                        break;
                    }
                }
            }
        }
        if (!ponderhit) {
            write_to_fd(fish_in, "position startpos moves");
            for (int i = 0; i < len; i++) {
                write_to_fd(fish_in, " ");
                write_to_fd(fish_in, game[i]);
            }
            write_to_fd(fish_in, "\n");
            write_to_fd(fish_in, "go movetime 500\n");
        }
        char buf[0x100];
        while (1) {
            read_line_from_fd(fish_out, buf, 0x100);
            if (!strncmp(buf, "bestmove", 8)) {
                break;
            }
        }
        printf("      fish : %s", buf);
        int buflen = strlen(buf);
        if (buflen < 14) {
            return 0;
        }
        char *p = buf+9;
        char *str = xmalloc(5);
        strncpy(str, p, 4);
        str[4] = 0;
        if (0 && buflen >= 26) {
            p += 4;
            if (*p != ' ') {
                p++;
            }
            p += 8;
            free(ponder_move);
            ponder_move = xmalloc(5);
            strncpy(ponder_move, p, 4);
            ponder_move[4] = 0;
            write_to_fd(fish_in, "position startpos moves");
            for (int i = 0; i < len; i++) {
                write_to_fd(fish_in, " ");
                write_to_fd(fish_in, game[i]);
            }
            write_to_fd(fish_in, " ");
            write_to_fd(fish_in, str);
            write_to_fd(fish_in, " ");
            write_to_fd(fish_in, ponder_move);
            write_to_fd(fish_in, "\n");
            write_to_fd(fish_in, "go ponder movetime 100\n");
            printf("      ponder_move : %s\n", ponder_move);
        } else {
            free(ponder_move);
            ponder_move = 0;
        }
        return str;
    } else {
        while (true) {
            char *m = opplm();
            if (!m) {
                continue;
            }
            write_to_fd(validator_in, "validate");
            for (int i = 0; i < len; i++) {
                write_to_fd(validator_in, " ");
                write_to_fd(validator_in, game[i]);
            }
            write_to_fd(validator_in, " ");
            write_to_fd(validator_in, m);
            write_to_fd(validator_in, "\n");
            char buf[0x100];
            read_line_from_fd(validator_out, buf, 0x100);
            if (!strcmp(buf, "yes\n")) {
                return m;
            }
            free(m);
            /* usleep(200*1000); */
            /* char *xlm = opplm(); */
            /* // printf("### %s %s\n", lm, xlm); */
            /* if (lm && xlm && (len == 0 || strcmp(lm, game[len-1])) && !strcmp(lm, xlm)) { */
            /*     return lm; */
            /* } */
            /* free(lm); free(xlm); */
        }
    }
}

void
sync_boards()
{
start:
    if (len == 0) {
        return;
    }
    if (turn == flip) {
        char *s = game[len-1];
        uint32_t src = (s[1]-'1')*8 + s[0]-'a';
        uint32_t dst = (s[3]-'1')*8 + s[2]-'a';
        if (flip) {
            src = 0x3f - src;
            dst = 0x3f - dst;
        }

        Window root = DefaultRootWindow(display);
        XWindowAttributes wa;
        XGetWindowAttributes(display, root, &wa);
        int w = wa.width, h = wa.height;
        XImage *image = XGetImage(display, root, 0, 0, w, h, AllPlanes, ZPixmap);

        uint32_t sq = 0x47;
        uint64_t prev_sq_c = -1;
        int prev_sq_y = -1;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint64_t pixel = XGetPixel(image, x, y);
                uint64_t sq_c = pixel;
                if (sq_c == marked_white) {
                    sq_c = white;
                } else if (sq_c == marked_black) {
                    sq_c = black;
                }
                if (sq_c != white && sq_c != black) {
                    continue;
                }
                if (sq_c != prev_sq_c && prev_sq_y != -1) {
                    sq++;
                }
                if (y > prev_sq_y) {
                    if (sq_c != prev_sq_c && prev_sq_y != -1) {
                        sq -= 0x8;
                    } else {
                        sq -= 0xf;
                    }
                }
                if (sq == src) {
                    xdo_move_mouse(xdo, x+10, y+10, 0);
                    xdo_click_window(xdo, CURRENTWINDOW, 1);
                    y = h;
                    break;
                }
                prev_sq_c = sq_c;
                prev_sq_y = y;
            }
        }
        usleep(50*1000);
        sq = 0x47;
        prev_sq_c = -1;
        prev_sq_y = -1;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint64_t pixel = XGetPixel(image, x, y);
                uint64_t sq_c = pixel;
                if (sq_c == marked_white) {
                    sq_c = white;
                } else if (sq_c == marked_black) {
                    sq_c = black;
                }
                if (sq_c != white && sq_c != black) {
                    continue;
                }
                if (sq_c != prev_sq_c && prev_sq_y != -1) {
                    sq++;
                }
                if (y > prev_sq_y) {
                    if (sq_c != prev_sq_c && prev_sq_y != -1) {
                        sq -= 0x8;
                    } else {
                        sq -= 0xF;
                    }
                }
                if (sq == dst) {
                    xdo_move_mouse(xdo, x+10, y+10, 0);
                    xdo_click_window(xdo, CURRENTWINDOW, 1);
                    y = h;
                    break;
                }
                prev_sq_c = sq_c;
                prev_sq_y = y;
            }
        }
        XDestroyImage(image);
        XFlush(display);
        return;
        char *lm = opplm();
        assert(len > 0);
        // printf("lm: %s\n", lm);
        if (!lm || strcmp(lm, game[len-1])) {
            free(lm);
            usleep(100*1000);
            goto start;
        }
        free(lm);
    }
}

void
kill_fish()
{
    xcall(kill, fish_pid, 9);
}

void
kill_validator()
{
    xcall(kill, validator_pid, 9);
}

void
prepare()
{
    len = 0;
    turn = 0;
    int pipe0[2];
    int pipe1[2];
    xcall(pipe, pipe0);
    xcall(pipe, pipe1);
    fish_pid = xcall(fork);
    if (fish_pid == 0) {
        xcall(dup2,  pipe0[1], 1);
        xcall(close, pipe0[1]);
        xcall(close, pipe0[0]);
        xcall(dup2,  pipe1[0], 0);
        xcall(close, pipe1[1]);
        xcall(close, pipe1[0]);
        char *argv[] = {"/home/asn/src/asn/asn", "-u", 0};
        // char *argv[] = {"/usr/games/stockfish", 0};
        xcall(execv, argv[0], argv);
    } else {
        atexit(kill_fish);
        xcall(close, pipe0[1]);
        xcall(close, pipe1[0]);
        fish_in = pipe1[1];
        fish_out = pipe0[0];
        write_to_fd(fish_in, "uci\n");
        write_to_fd(fish_in, "ucinewgame\n");
        write_to_fd(fish_in, "isready\n");
        while (true) {
            char buf[0x100];
            read_line_from_fd(fish_out, buf, 0x100);
            if (!strcmp(buf, "readyok\n")) {
                break;
            }
        }
    }
    {
        int pipe0[2];
        int pipe1[2];
        xcall(pipe, pipe0);
        xcall(pipe, pipe1);
        validator_pid = xcall(fork);
        if (validator_pid == 0) {
            xcall(dup2,  pipe0[1], 1);
            xcall(close, pipe0[1]);
            xcall(close, pipe0[0]);
            xcall(dup2,  pipe1[0], 0);
            xcall(close, pipe1[1]);
            xcall(close, pipe1[0]);
            char *argv[] = {"/home/asn/src/asn/asn", "-u", 0};
            xcall(execv, argv[0], argv);
        } else {
            atexit(kill_validator);
            xcall(close, pipe0[1]);
            xcall(close, pipe1[0]);
            validator_in = pipe1[1];
            validator_out = pipe0[0];
        }
    }
}

/* void */
/* read_from_fd(int fd, char *dest, int n) */
/* { */
/*     for (; --n > 0; dest++) { */
/*         int r = read(fd, dest, 1); */
/*         if (r < 0) { */
/*             perror("read"); */
/*             exit(1); */
/*         } */
/*         if (r == 0) { */
/*             fprintf(stderr, "error: unexpected eof\n"); */
/*             exit(1); */
/*         } */
/*     } */
/*     *dest = 0; */
/* } */

void
read_line_from_fd(int fd, char *dest, int n)
{
    while (--n > 0) {
        char chr[1];
        int r = xcall(read, fd, chr, 1);
        if (r == 0) {
            break;
        }
        *dest = chr[0];
        if (*dest++ == '\n') {
            break;
        }
    }
    *dest = 0;
}

void
write_to_fd(int fd, const char *s)
{
    xcall(write, fd, s, strlen(s));
}

void
go()
{
    prepare();
    char *m;
    while ((m = get_move())) {
        printf("%d: %s\n", turn, m);
        game[len++] = m;
        sync_boards();
        turn ^= 1;
    }
}

void
init()
{
    xdo = xdo_new(0);
    display = XOpenDisplay(0);
}

void
clean()
{
    for (int i = 0; i < len; i++) {
        free(game[i]);
    }
    XCloseDisplay(display);
    xdo_free(xdo);
}

int
main(int argc, char **argv)
{
    if (argc > 1) {
        flip = 1;
    }
    init();
    go();
    clean();
}
