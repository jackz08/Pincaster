
#include "common.h"
#include "http_server.h"
#include "db_log.h"

int init_db_log(void)
{
    DBLog * const db_log = &app_context.db_log;
    
    *db_log = (DBLog) {
        .db_log_file_name = NULL,
        .db_log_fd = -1,
        .log_buffer = NULL,
        .journal_buffer_size = DEFAULT_JOURNAL_BUFFER_SIZE,
        .fsync_period = DEFAULT_FSYNC_PERIOD
    };
    return 0;
}

int open_db_log(void)
{
    DBLog * const db_log = &app_context.db_log;
    const char *db_log_file_name = db_log->db_log_file_name;

    if (db_log_file_name == NULL) {
        db_log->db_log_fd = -1;
        return 1;
    }
    int flags = O_RDWR | O_CREAT;
#ifdef O_EXLOCK
    flags |= O_EXLOCK;
#endif
#ifdef O_NOATIME
    flags |= O_NOATIME;
#endif
#ifdef O_LARGEFILE
    flags |= O_LARGEFILE;
#endif
    db_log->db_log_fd = open(db_log_file_name, flags, (mode_t) 0600);
    if (db_log->db_log_fd == -1) {
        free_db_log();
        return -1;
    }
    if ((db_log->log_buffer = evbuffer_new()) == NULL) {
        free_db_log();
        return -1;
    }
    return 0;
}

void free_db_log(void)
{
    DBLog * const db_log = &app_context.db_log;

    close_db_log();
    free(db_log->db_log_file_name);
    db_log->db_log_file_name = NULL;
    if (db_log->log_buffer != NULL) {
        evbuffer_free(db_log->log_buffer);
        db_log->log_buffer = NULL;
    }
}

int close_db_log(void)
{
    DBLog * const db_log = &app_context.db_log;
    
    if (db_log->db_log_fd == -1) {
        return 0;
    }
    flush_db_log(1);
    fsync(db_log->db_log_fd);
    close(db_log->db_log_fd);
    db_log->db_log_fd = -1;
    
    return 0;
}

int add_to_db_log(const int verb,
                  const char *uri, struct evbuffer * const input_buffer)
{
    DBLog * const db_log = &app_context.db_log;
    if (db_log->db_log_fd == -1) {
        return 0;
    }
    const size_t uri_len = strlen(uri);
    size_t body_len = evbuffer_get_length(input_buffer);
    if (uri_len > DB_LOG_MAX_URI_LEN || body_len > DB_LOG_MAX_BODY_LEN) {
        return -1;
    }
    struct evbuffer * const log_buffer = db_log->log_buffer;
    const char *body = NULL;
    
    if (body_len > (size_t) 0U) {
        body = (const char *) evbuffer_pullup(input_buffer, -1);        
        if (body != NULL && *(body + body_len - (size_t) 1U) == 0) {
            body_len--;
        }
    }
    evbuffer_add(log_buffer, DB_LOG_RECORD_COOKIE_HEAD,
                 sizeof DB_LOG_RECORD_COOKIE_HEAD - (size_t) 1U);
    evbuffer_add_printf(log_buffer, "%x %zx:%s %zx:", verb, uri_len,
                        uri, body_len);
    if (body != NULL) {
        evbuffer_add(log_buffer, body, body_len);
    }
    evbuffer_add(log_buffer, DB_LOG_RECORD_COOKIE_TAIL,
                 sizeof DB_LOG_RECORD_COOKIE_TAIL - (size_t) 1U);
    if (app_context.db_log.fsync_period == 0) {
        flush_db_log(1);
    } else if (evbuffer_get_length(log_buffer) > db_log->journal_buffer_size) {
        flush_db_log(0);
    }
    return 0;
}

int flush_db_log(const _Bool sync)
{
    DBLog * const db_log = &app_context.db_log;
    if (db_log->db_log_fd == -1) {
        return 0;
    }
    struct evbuffer * const log_buffer = db_log->log_buffer;    
    size_t to_write;
    to_write = evbuffer_get_length(log_buffer);
    if (to_write <= (size_t) 0U) {
        return 0;
    }
    if (sync == 0) {
        to_write = (size_t) 0U;
    }
    do {
        ssize_t written = evbuffer_write(log_buffer, db_log->db_log_fd);
        if (written < (ssize_t) to_write) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep((useconds_t) 1000000 / 10);
                continue;
            }
            return -1;
        }
    } while(0);
    if (sync != 0) {        
#ifdef HAVE_FDATASYNC
        fdatasync(db_log->db_log_fd);
#else
        fsync(db_log->db_log_fd);
#endif
    }
    return 0;
}

// This is dog slow, all these calls to read() absolutely need to go away.
int replay_log_record(HttpHandlerContext * const context)
{
    DBLog * const db_log = &app_context.db_log;
    
    if (db_log->db_log_fd == -1) {
        return 0;
    }
    char buf_cookie_head[sizeof DB_LOG_RECORD_COOKIE_HEAD - (size_t) 1U];
    char buf_cookie_tail[sizeof DB_LOG_RECORD_COOKIE_TAIL - (size_t) 1U];
    char buf_number[50];
    char *pnt;
    char *endptr;
    off_t current_offset = lseek(db_log->db_log_fd, (off_t) 0, SEEK_CUR);
    
    ssize_t readnb = safe_read(db_log->db_log_fd, buf_cookie_head,
                               sizeof buf_cookie_head);
    if (readnb == (ssize_t) 0) {
        return 1;
    }
    if (readnb != (ssize_t) sizeof buf_cookie_head ||
        memcmp(buf_cookie_head, buf_cookie_head, readnb) != 0) {
        if (lseek(db_log->db_log_fd, current_offset, SEEK_SET) == (off_t) -1 ||
            ftruncate(db_log->db_log_fd, current_offset) != 0) {
        }
        return -1;
    }
    pnt = buf_number;
    for (;;) {
        if (safe_read(db_log->db_log_fd, pnt, (size_t) 1U) != (ssize_t) 1U) {
            return -1;
        }
        if (*pnt == ' ') {
            break;
        }
        if (++pnt == &buf_number[sizeof buf_number]) {
            return -1;
        }
    }
    int verb = (int) strtol(buf_number, &endptr, 16);
    if (endptr == NULL || endptr == buf_number) {
        return -1;
    }
    
    pnt = buf_number;
    for (;;) {
        if (safe_read(db_log->db_log_fd, pnt, (size_t) 1U) != (ssize_t) 1U) {
            return -1;
        }
        if (*pnt == ':') {
            break;
        }
        if (++pnt == &buf_number[sizeof buf_number]) {
            return -1;
        }
    }
    size_t uri_len = (size_t) strtoul(buf_number, &endptr, 16);
    if (endptr == NULL || endptr == buf_number || uri_len <= (size_t) 0U) {
        return -1;
    }
    char *uri;
    if ((uri = malloc(uri_len + (size_t) 1U)) == NULL) {
        _exit(1);
    }    
    if (safe_read(db_log->db_log_fd, uri, uri_len) != (ssize_t) uri_len) {
        return -1;
    }
    *(uri + uri_len) = 0;
    
    pnt = buf_number;
    if (safe_read(db_log->db_log_fd, pnt, (size_t) 1U) != (ssize_t) 1U ||
        *pnt != ' ') {
        free(uri);
        return -1;
    }    
    for (;;) {
        if (safe_read(db_log->db_log_fd, pnt, (size_t) 1U) != (ssize_t) 1U) {
            free(uri);
            return -1;
        }
        if (*pnt == ':') {
            break;
        }
        if (++pnt == &buf_number[sizeof buf_number]) {
            free(uri);
            return -1;
        }
    }
    size_t body_len = (size_t) strtoul(buf_number, &endptr, 16);
    if (endptr == NULL || endptr == buf_number) {
        free(uri);
        return -1;
    }
    
    char *body = NULL;
    if (body_len > (size_t) 0U) {
        body = malloc(body_len + (size_t) 1U);
        if (body == NULL) {
            free(uri);
            _exit(1);
        }
    }
    if (body != NULL) {
        assert(body_len > (size_t) 0U);
        if (safe_read(db_log->db_log_fd, body, body_len) !=
            (ssize_t) body_len) {
            free(uri);
            free(body);
            return -1;
        }
        *(body + body_len) = 0;
    }
    readnb = safe_read(db_log->db_log_fd, buf_cookie_tail,
                       sizeof buf_cookie_tail);
    if (readnb != (ssize_t) sizeof buf_cookie_tail ||
        memcmp(buf_cookie_tail, buf_cookie_tail, readnb) != 0) {
        free(body);
        free(uri);
        return -1;
    }
    fake_request(context, verb, uri, body, body_len);
    free(body);
    free(uri);
    
    return 0;
}

int replay_log(HttpHandlerContext * const context)
{
    int res;
    uintmax_t counter = (uintmax_t) 0U;

    puts("Replaying journal...");
    while ((res = replay_log_record(context)) == 0) {
        counter++;
    }
    printf("%" PRIuMAX " transactions replayed.\n", counter);
    if (res < 0) {
        puts("Possibly corrupted journal.");
        return -1;
    }
    return 0;
}
