/* SPDX-License-Identifier: GPL-3.0-only */
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define GAME_LIMIT (64u * 1024u * 1024u)
#define VERIFY_DIAGNOSTIC_LIMIT 100u
#define PLY_TAG_DETAIL_LIMIT 8u

typedef struct {
    uint64_t line, value;
    int numeric;
    const char *raw;
    size_t raw_len;
} PlyTagOccurrence;

typedef struct { char *data; size_t len, cap; } Buffer;
typedef struct {
    uint64_t checked, correct, missing, mismatched, invalid, duplicate;
    uint64_t diagnostics;
    int suppression_reported;
} VerifyStats;
typedef struct {
    uint64_t count;
    size_t detail_count;
    PlyTagOccurrence details[PLY_TAG_DETAIL_LIMIT];
} PlyTagInfo;
typedef struct {
    int verify, force, have_limit, show_help, show_version;
    uint64_t limit;
    const char *input, *output;
} Options;

static void usage(FILE *out)
{
    fputs("Usage:\n"
          "  plycount [OPTIONS] INPUT OUTPUT\n"
          "  plycount --verify [OPTIONS] INPUT\n\n"
          "Add or replace the PlyCount tag with the number of main-line "
          "plies,\n"
          "or verify existing PlyCount tags. RAV moves are not counted. A "
          "FEN\n"
          "fullmove number is not added to the count. A single game is "
          "limited\n"
          "to 64 MiB. INPUT and OUTPUT must not refer to the same file.\n"
          "\n"
          "Options:\n"
          "  --verify       Verify PlyCount tags; no OUTPUT argument is "
          "used.\n"
          "  --limit N      Process at most N games (N must be positive)."
          "\n"
          "  --force        Replace an existing OUTPUT after successful "
          "processing.\n"
          "  --help, -h, -? Show this help and exit.\n"
          "  --version      Show version information and exit.\n\n"
          "Exit status: 0 means all selected tags are correct; 1 means "
          "one or\n"
          "more PlyCount discrepancies; 2 means an invocation or "
          "processing error.\n", out);
}

static int parse_u64(const char *s, uint64_t *v)
{
    uint64_t n = 0;
    const unsigned char *p = (const unsigned char *)s;
    if (!*p)
        return 0;
    while (*p) {
        unsigned d;
        if (*p < '0' || *p > '9')
            return 0;
        d = (unsigned)(*p++ - '0');
        if (n > (UINT64_MAX - d) / 10u)
            return 0;
        n = n * 10u + d;
    }
    if (n == 0)
        return 0;
    *v = n;
    return 1;
}

static int parse_decimal(const char *s, size_t len, uint64_t *v)
{
    uint64_t n = 0;
    size_t i;
    if (!len)
        return 0;
    for (i = 0; i < len; ++i) {
        unsigned d;
        if (s[i] < '0' || s[i] > '9')
            return 0;
        d = (unsigned)(s[i] - '0');
        if (n > (UINT64_MAX - d) / 10u)
            return 0;
        n = n * 10u + d;
    }
    *v = n;
    return 1;
}

static int parse_options(int argc, char **argv, Options *o)
{
    int i;
    memset(o, 0, sizeof(*o));
    for (i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h") || !strcmp(a, "-?")) {
            o->show_help = 1;
            continue;
        }
        if (!strcmp(a, "--version")) {
            o->show_version = 1;
            continue;
        }
        if (!strcmp(a, "--verify")) {
            o->verify = 1;
            continue;
        }
        if (!strcmp(a, "--force")) {
            o->force = 1;
            continue;
        }
        if (!strcmp(a, "--limit")) {
            if (++i >= argc || !parse_u64(argv[i], &o->limit)) {
                fprintf(stderr,
                        "plycount: --limit requires a positive integer\n");
                return 0;
            }
            o->have_limit = 1;
            continue;
        }
        if (a[0] == '-') {
            fprintf(stderr, "plycount: unknown option: %s\n", a);
            return 0;
        }
        if (!o->input)
            o->input = a;
        else if (!o->output)
            o->output = a;
        else {
            fputs("plycount: too many file arguments\n", stderr);
            return 0;
        }
    }
    if (o->show_help || o->show_version)
        return 1;
    if (o->verify) {
        if (!o->input || o->output) {
            fputs("plycount: verify mode requires INPUT and does not "
                  "accept OUTPUT\n", stderr);
            return 0;
        }
        if (o->force) {
            fputs("plycount: --force is only valid in rewrite mode\n",
                  stderr);
            return 0;
        }
    } else if (!o->input || !o->output) {
        fputs("plycount: rewrite mode requires INPUT and OUTPUT\n",
              stderr);
        return 0;
    }
    return 1;
}

static int reserve(Buffer *b, size_t extra)
{
    size_t need, cap;
    char *p;
    if (extra > GAME_LIMIT || b->len > GAME_LIMIT - extra) {
        errno = EFBIG;
        return 0;
    }
    need = b->len + extra;
    if (need <= b->cap)
        return 1;
    cap = b->cap ? b->cap : 4096;
    while (cap < need) {
        if (cap > GAME_LIMIT / 2) {
            cap = GAME_LIMIT;
            break;
        }
        cap *= 2;
    }
    p = (char *)realloc(b->data, cap);
    if (!p)
        return 0;
    b->data = p;
    b->cap = cap;
    return 1;
}

static int append(Buffer *b, const void *p, size_t n)
{
    if (!reserve(b, n))
        return 0;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 1;
}

static int tag_line(const char *s, size_t n,
                    size_t *name_start, size_t *name_len)
{
    size_t i = 0, start;
    while (i < n && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    if (i == n || s[i++] != '[')
        return 0;
    start = i;
    while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '_'))
        ++i;
    if (i == start || i == n || !(s[i] == ' ' || s[i] == '\t'))
        return 0;
    *name_start = start;
    *name_len = i - start;
    return 1;
}

static size_t header_end(const char *s, size_t n)
{
    size_t pos = (n >= 3 && (unsigned char)s[0] == 0xef &&
                  (unsigned char)s[1] == 0xbb &&
                  (unsigned char)s[2] == 0xbf) ? 3 : 0;
    while (pos < n) {
        size_t end = pos, ns, nl;
        while (end < n && s[end] != '\n')
            ++end;
        if (!tag_line(s + pos, end - pos, &ns, &nl))
            break;
        pos = end < n ? end + 1 : end;
    }
    return pos;
}

static int is_result(const char *s, size_t n)
{
    return (n == 3 && (!memcmp(s, "1-0", 3) ||
                       !memcmp(s, "0-1", 3) ||
                       !memcmp(s, "***", 3))) ||
           (n == 7 && !memcmp(s, "1/2-1/2", 7)) ||
           (n == 1 && s[0] == '*');
}

static int is_move_number(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n && isdigit((unsigned char)s[i]))
        ++i;
    if (!i || i == n)
        return 0;
    while (i < n && s[i] == '.')
        ++i;
    return i == n;
}

static int token_is_ignored(const char *s, size_t n)
{
    if (!n || is_move_number(s, n) || is_result(s, n))
        return 1;
    if ((n == 4 && !memcmp(s, "e.p.", 4)) ||
        (n == 3 && !memcmp(s, "...", 3)))
        return 1;
    if (n <= 3 && (strchr("!?", s[0]) != NULL)) {
        size_t i;
        for (i = 0; i < n; ++i) {
            if (s[i] != '!' && s[i] != '?')
                return 0;
        }
        return 1;
    }
    if (s[0] == '$') {
        size_t i = 1;
        if (i == n)
            return 0;
        while (i < n && isdigit((unsigned char)s[i]))
            ++i;
        return i == n;
    }
    return 0;
}

static int inspect_ply_tag(const char *line, size_t n, uint64_t *value,
                           const char **raw, size_t *raw_len)
{
    size_t i = 0, name_start, name_end, value_start, value_end;
    while (i < n && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    if (i == n || line[i++] != '[')
        return -1;
    name_start = i;
    while (i < n && (isalnum((unsigned char)line[i]) || line[i] == '_'))
        ++i;
    name_end = i;
    if (name_end - name_start != 8 ||
        memcmp(line + name_start, "PlyCount", 8))
        return -1;
    if (i >= n || !(line[i] == ' ' || line[i] == '\t')) {
        *raw = line + i; *raw_len = n - i;
        return 0;
    }
    while (i < n && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    if (i >= n || line[i++] != '"') {
        *raw = line + i; *raw_len = n - i;
        return 0;
    }
    value_start = i;
    while (i < n && line[i] != '"' && line[i] != '\n' && line[i] != '\r')
        ++i;
    value_end = i;
    *raw = line + value_start; *raw_len = value_end - value_start;
    if (i >= n || line[i] != '"')
        return 0;
    ++i;
    while (i < n && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
        ++i;
    if (i >= n || line[i] != ']')
        return 0;
    ++i;
    while (i < n && (line[i] == '\n' || line[i] == '\r' ||
                     line[i] == ' ' || line[i] == '\t'))
        ++i;
    if (i != n)
        return 0;
    return parse_decimal(line + value_start, value_end - value_start,
                         value);
}

static int parse_game(const char *s, size_t n, uint64_t start_line,
                      uint64_t *plies, int *has_result, PlyTagInfo *tag)
{
    size_t h = header_end(s, n), i = h;
    uint64_t count = 0;
    int rav = 0, brace = 0, line_comment = 0, found_result = 0;
    memset(tag, 0, sizeof(*tag));
    /* Header tags are inspected independently of movetext tokenization. */
    {
        size_t p = (h >= 3 && (unsigned char)s[0] == 0xef &&
                    (unsigned char)s[1] == 0xbb &&
                    (unsigned char)s[2] == 0xbf) ? 3 : 0;
        uint64_t line = start_line;
        while (p < h) {
            size_t e = p;
            while (e < h && s[e] != '\n')
                ++e;
            {
                uint64_t value = 0;
                const char *raw = NULL;
                size_t raw_len = 0;
                int status = inspect_ply_tag(s + p, e - p, &value,
                                             &raw, &raw_len);
                if (status >= 0) {
                    if (tag->count == UINT64_MAX) {
                        errno = EOVERFLOW;
                        return 0;
                    }
                    ++tag->count;
                    if (tag->detail_count < PLY_TAG_DETAIL_LIMIT) {
                        PlyTagOccurrence *occurrence =
                            &tag->details[tag->detail_count++];
                        occurrence->line = line;
                        occurrence->numeric = status == 1;
                        occurrence->value = value;
                        occurrence->raw = raw;
                        occurrence->raw_len = raw_len;
                    }
                }
            }
            p = e < h ? e + 1 : e;
            if (e < h && line < UINT64_MAX)
                ++line;
        }
    }
    while (i < n) {
        size_t start, len;
        unsigned char c = (unsigned char)s[i];
        if (line_comment) {
            if (c == '\n')
                line_comment = 0;
            ++i;
            continue;
        }
        if (brace) {
            if (c == '}')
                brace = 0;
            ++i;
            continue;
        }
        if (c == '{') {
            brace = 1;
            ++i;
            continue;
        }
        if (c == ';') {
            line_comment = 1;
            ++i;
            continue;
        }
        if (c == '(') {
            ++rav;
            ++i;
            continue;
        }
        if (c == ')') {
            if (rav)
                --rav;
            ++i;
            continue;
        }
        if (isspace(c)) {
            ++i;
            continue;
        }
        if (c == '$') {
            ++i;
            while (i < n && isdigit((unsigned char)s[i]))
                ++i;
            continue;
        }
        start = i;
        while (i < n && !isspace((unsigned char)s[i]) &&
               s[i] != '{' && s[i] != '}' && s[i] != '(' &&
               s[i] != ')' && s[i] != ';')
            ++i;
        len = i - start;
        if (rav)
            continue;
        if (is_result(s + start, len)) {
            found_result = 1;
            continue;
        }
        /* Accept compact forms such as 1.e4 and 1...e5. */
        while (len && isdigit((unsigned char)s[start])) {
            ++start;
            --len;
        }
        while (len && s[start] == '.') {
            ++start;
            --len;
        }
        if (token_is_ignored(s + start, len))
            continue;
        if (count == UINT64_MAX) {
            errno = EOVERFLOW;
            return 0;
        }
        ++count;
    }
    *plies = count; *has_result = found_result;
    return 1;
}

static int output_game(FILE *out, const char *s, size_t n,
                       uint64_t plies, int replace)
{
    size_t h = header_end(s, n), p = 0, first = 1;
    char tag[96];
    int z = snprintf(tag, sizeof(tag), "[PlyCount \"%" PRIu64 "\"]\n",
                     plies);
    if (z < 0 || (size_t)z >= sizeof(tag))
        return 0;
    while (p < h) {
        size_t e = p, ns, nl, line_end;
        while (e < h && s[e] != '\n')
            ++e;
        line_end = e < h ? e + 1 : e;
        if (tag_line(s + p, e - p, &ns, &nl) && nl == 8 &&
            !memcmp(s + p + ns, "PlyCount", 8)) {
            if (first) {
                if (fwrite(tag, 1, (size_t)z, out) != (size_t)z)
                    return 0;
                first = 0;
            }
            p = line_end;
            continue;
        }
        if (fwrite(s + p, 1, line_end - p, out) != line_end - p)
            return 0;
        p = line_end;
    }
    if (first) {
        if (h == 0) {
            if (fwrite(tag, 1, (size_t)z, out) != (size_t)z)
                return 0;
        } else if (fwrite(tag, 1, (size_t)z, out) != (size_t)z) {
            return 0;
        }
    }
    if (n > h && fwrite(s + h, 1, n - h, out) != n - h)
        return 0;
    (void)replace;
    return 1;
}

static int valid_tag_candidate(const char *line, size_t n)
{
    size_t i = 0, a, b, q;
    if (!tag_line(line, n, &a, &b))
        return 0;
    i = a + b;
    while (i < n && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    if (i >= n || line[i++] != '"')
        return 0;
    q = i;
    while (q < n) {
        if (line[q] == '\\' && q + 1 < n) {
            q += 2;
            continue;
        }
        if (line[q] == '"')
            break;
        ++q;
    }
    if (q >= n)
        return 0;
    ++q;
    while (q < n && (line[q] == ' ' || line[q] == '\t' || line[q] == '\r'))
        ++q;
    return q < n && line[q] == ']';
}

static int same_file(const char *in, const char *out)
{
    struct stat a, b;
    char *ri = realpath(in, NULL);
    char *ro = realpath(out, NULL);
    int same = 0;
    if (stat(in, &a) == 0 && stat(out, &b) == 0 &&
        a.st_dev == b.st_dev && a.st_ino == b.st_ino)
        same = 1;
    if (ri && ro && !strcmp(ri, ro))
        same = 1;
    free(ri);
    free(ro);
    return same;
}

static char *make_stage_template(const char *output)
{
    const char *slash = strrchr(output, '/');
    const char *base = slash ? slash + 1 : output;
    const char *suffix = ".plycount.XXXXXX";
    size_t prefix_len, suffix_len = strlen(suffix), total;
    const char *prefix;
    char *path;

    if (!*base) {
        errno = EINVAL;
        return NULL;
    }
    if (!slash) {
        prefix = "./";
        prefix_len = 2;
    } else if (slash == output) {
        prefix = "/";
        prefix_len = 1;
    } else {
        prefix = output;
        prefix_len = (size_t)(slash - output) + 1;
    }
    if (prefix_len > SIZE_MAX - suffix_len - 1) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    total = prefix_len + suffix_len + 1;
    path = (char *)malloc(total);
    if (!path)
        return NULL;
    memcpy(path, prefix, prefix_len);
    memcpy(path + prefix_len, suffix, suffix_len + 1);
    return path;
}

static void report_progress(uint64_t games, uint64_t bytes, time_t *last)
{
    time_t now = time(NULL);
    if (now - *last >= 5) {
        fprintf(stderr, "plycount: processed %" PRIu64 " games "
                "(%" PRIu64 " input bytes)\n", games, bytes);
        *last = now;
    }
}

static void print_raw_value(const char *s, size_t n)
{
    size_t i, limit = n < 80 ? n : 80;
    fputc('"', stderr);
    for (i = 0; i < limit; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            fputc('\\', stderr);
            fputc(c, stderr);
        } else if (c >= 32 && c < 127)
            fputc(c, stderr);
        else
            fputc('?', stderr);
    }
    if (limit < n)
        fputs("...", stderr);
    fputc('"', stderr);
}

static void report_discrepancy(VerifyStats *stats, uint64_t game_no,
                               uint64_t start_line, int category,
                               const PlyTagInfo *tag, uint64_t plies)
{
    if (stats->diagnostics < VERIFY_DIAGNOSTIC_LIMIT) {
        fprintf(stderr, "plycount: game %" PRIu64, game_no);
        if (category == 1)
            fprintf(stderr, ", starting at line %" PRIu64
                    ": missing PlyCount (calculated %" PRIu64 ")\n",
                    start_line, plies);
        else if (category == 2) {
            const PlyTagOccurrence *occurrence = &tag->details[0];
            fprintf(stderr, ", line %" PRIu64 ": PlyCount %" PRIu64
                    ", calculated %" PRIu64 "\n",
                    occurrence->line, occurrence->value, plies);
        }
        else if (category == 3) {
            const PlyTagOccurrence *occurrence = &tag->details[0];
            fprintf(stderr, ", line %" PRIu64 ": invalid PlyCount ",
                    occurrence->line);
            print_raw_value(occurrence->raw ? occurrence->raw : "",
                            occurrence->raw_len);
            fprintf(stderr, " (calculated %" PRIu64 ")\n", plies);
        } else {
            size_t i;
            fprintf(stderr, ": duplicate PlyCount tags (%" PRIu64
                    " total): ", tag->count);
            for (i = 0; i < tag->detail_count; ++i) {
                const PlyTagOccurrence *occurrence = &tag->details[i];
                if (i)
                    fputs(", ", stderr);
                fprintf(stderr, "line %" PRIu64 " ", occurrence->line);
                print_raw_value(occurrence->raw ? occurrence->raw : "",
                                occurrence->raw_len);
            }
            if (tag->count > tag->detail_count)
                fputs(", additional tags omitted", stderr);
            fprintf(stderr, " (calculated %" PRIu64 ")\n", plies);
        }
        ++stats->diagnostics;
    } else if (!stats->suppression_reported) {
        fprintf(stderr, "plycount: further discrepancy details suppressed "
                "after %u reports\n",
                (unsigned)VERIFY_DIAGNOSTIC_LIMIT);
        stats->suppression_reported = 1;
    }
}

static int process_record(const Options *o, FILE *out, Buffer *game,
                          uint64_t game_no, uint64_t start_line,
                          VerifyStats *stats)
{
    uint64_t plies = 0; int result = 0; PlyTagInfo tag;
    if (!parse_game(game->data, game->len, start_line,
                    &plies, &result, &tag))
        return 0;
    if (o->verify) {
        int category = 0;
        if (tag.count == 0)
            category = 1;
        else if (tag.count > 1)
            category = 4;
        else if (!tag.details[0].numeric)
            category = 3;
        else if (tag.details[0].value != plies)
            category = 2;
        if (category == 0)
            ++stats->correct;
        else {
            if (category == 1)
                ++stats->missing;
            else if (category == 2)
                ++stats->mismatched;
            else if (category == 3)
                ++stats->invalid;
            else
                ++stats->duplicate;
            report_discrepancy(stats, game_no, start_line, category,
                               &tag, plies);
        }
        ++stats->checked;
    } else if (!output_game(out, game->data, game->len, plies,
                            tag.count != 0))
        return 0;
    (void)result;
    return 1;
}

int main(int argc, char **argv)
{
    Options o; FILE *in = NULL, *out = NULL; Buffer game = {0,0,0};
    char *stage = NULL; int stage_owned = 0, rc = 2, c, line_candidate = 0;
    size_t line_start = 0;
    uint64_t line_start_line = 1, game_start_line = 1, current_line = 1;
    uint64_t games = 0, bytes = 0, next_progress = 64u * 1024u * 1024u;
    VerifyStats verify_stats;
    time_t last_progress = time(NULL);
    struct stat st; int fd = -1;
    memset(&verify_stats, 0, sizeof(verify_stats));
    if (!parse_options(argc, argv, &o)) {
        fputs("Try 'plycount --help' for usage.\n", stderr);
        return 2;
    }
    if (o.show_help) {
        usage(stdout);
        return 0;
    }
    if (o.show_version) {
        puts("plycount 1.0");
        return 0;
    }
    if (!o.verify && same_file(o.input, o.output)) {
        fputs("plycount: INPUT and OUTPUT refer to the same file\n",
              stderr);
        return 2;
    }
    in = fopen(o.input, "rb");
    if (!in) {
        fprintf(stderr, "plycount: cannot open INPUT: %s\n",
                strerror(errno));
        goto done;
    }
    if (fstat(fileno(in), &st) == 0 && !S_ISREG(st.st_mode)) {
        fputs("plycount: INPUT must be a regular file\n", stderr);
        goto done;
    }
    if (!o.verify) {
        struct stat ost;
        if (!o.force && stat(o.output, &ost) == 0) {
            fputs("plycount: OUTPUT already exists (use --force to "
                  "replace it)\n", stderr);
            goto done;
        }
        stage = make_stage_template(o.output);
        if (!stage) {
            fprintf(stderr,
                    "plycount: cannot construct staging path: %s\n",
                    strerror(errno));
            goto done;
        }
        fd = mkstemp(stage);
        if (fd < 0) {
            fprintf(stderr, "plycount: cannot create staging file: %s\n",
                    strerror(errno));
            goto done;
        }
        stage_owned = 1; out = fdopen(fd, "wb");
        if (!out) {
            int e = errno;
            close(fd);
            fd = -1;
            unlink(stage);
            stage_owned = 0;
            errno = e;
            fprintf(stderr, "plycount: cannot open staging stream: %s\n",
                    strerror(errno));
            goto done;
        }
        fd = -1;
    }
    while ((c = fgetc(in)) != EOF) {
        char ch = (char)c;
        if (game.len == 0 ||
            (game.len && game.data[game.len - 1] == '\n')) {
            line_start = game.len;
            line_start_line = current_line;
            line_candidate = (ch == '[');
        }
        if (!append(&game, &ch, 1)) {
            fprintf(stderr, "plycount: game exceeds 64 MiB or memory "
                    "allocation failed\n");
            goto done;
        }
        ++bytes;
        if (ch == '\n') {
            if (line_candidate && line_start > 0 &&
                valid_tag_candidate(game.data + line_start,
                                    game.len - line_start)) {
                uint64_t p; int res; PlyTagInfo tag;
                if (!parse_game(game.data, line_start, game_start_line,
                                &p, &res, &tag)) {
                    fputs("plycount: cannot reliably parse game "
                          "boundary\n", stderr);
                    goto done;
                }
                if (res) {
                    size_t tail = game.len - line_start;
                    Buffer next = {0,0,0};
                    if (!append(&next, game.data + line_start, tail)) {
                        free(next.data);
                        goto done;
                    }
                    game.len = line_start;
                    if (game.len &&
                        process_record(&o, out, &game, games + 1,
                                       game_start_line,
                                       &verify_stats) != 1) {
                        free(next.data);
                        goto done;
                    }
                    ++games;
                    if (o.have_limit && games >= o.limit) {
                        free(next.data);
                        game.len = 0;
                        break;
                    }
                    free(game.data);
                    game = next;
                    game_start_line = line_start_line;
                }
            }
            if (current_line < UINT64_MAX)
                ++current_line;
            line_candidate = 0;
        }
        if (bytes >= next_progress) {
            report_progress(games, bytes, &last_progress);
            if (UINT64_MAX - bytes < 64u * 1024u * 1024u)
                next_progress = UINT64_MAX;
            else
                next_progress = bytes + 64u * 1024u * 1024u;
        }
    }
    if (ferror(in)) {
        fprintf(stderr, "plycount: input read error: %s\n",
                strerror(errno));
        goto done;
    }
    if (game.len && !(o.have_limit && games >= o.limit)) {
        if (process_record(&o, out, &game, games + 1,
                           game_start_line, &verify_stats) != 1) {
            fputs("plycount: malformed game or processing failure\n",
                  stderr);
            goto done;
        }
        ++games;
    }
    if (!games) {
        fputs("plycount: INPUT contains no PGN games\n", stderr);
        goto done;
    }
    if (!o.verify) {
        {
            int flush_failed = fflush(out) != 0;
            int saved_errno = flush_failed ? errno : 0;
            int close_failed = fclose(out) != 0;
            if (!saved_errno && close_failed)
                saved_errno = errno;
            out = NULL;
            if (saved_errno) {
                errno = saved_errno;
                fprintf(stderr,
                        "plycount: staging write/close failed: %s\n",
                        strerror(errno));
                goto done;
            }
        }
        if (!o.force && lstat(o.output, &st) == 0) {
            fputs("plycount: OUTPUT appeared during processing\n", stderr);
            goto done;
        }
        if (rename(stage, o.output) != 0) {
            fprintf(stderr, "plycount: cannot publish OUTPUT: %s\n",
                    strerror(errno));
            goto done;
        }
        stage_owned = 0;
    }
    if (o.verify) {
        if (verify_stats.missing || verify_stats.mismatched ||
            verify_stats.invalid || verify_stats.duplicate) {
            fprintf(stderr, "plycount: checked %" PRIu64 " games: %" PRIu64
                    " correct, %" PRIu64 " missing, %" PRIu64
                    " mismatched, %" PRIu64 " invalid, %" PRIu64
                    " duplicate\n", verify_stats.checked,
                    verify_stats.correct, verify_stats.missing,
                    verify_stats.mismatched,
                    verify_stats.invalid, verify_stats.duplicate);
            rc = 1;
        } else {
            fprintf(stderr, "plycount: verified %" PRIu64 " games\n",
                    verify_stats.checked);
            rc = 0;
        }
    } else {
        fprintf(stderr, "plycount: processed %" PRIu64 " games\n", games);
        rc = 0;
    }
done:
    if (out)
        fclose(out);
    if (fd >= 0)
        close(fd);
    if (stage_owned)
        unlink(stage);
    free(stage);
    if (in)
        fclose(in);
    free(game.data);
    return rc;
}
