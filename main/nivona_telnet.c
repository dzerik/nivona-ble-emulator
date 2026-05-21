#include "nivona_telnet.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "esp_log.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "nivona_telnet";

#define TELNET_PORT 23

static int s_client_fd = -1;
static SemaphoreHandle_t s_mutex;
static vprintf_like_t s_orig_vprintf = NULL;

// ---- Log redirect: mirror stdout to any connected telnet client -------

static int telnet_vprintf(const char *fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int r = s_orig_vprintf ? s_orig_vprintf(fmt, args) : vprintf(fmt, args);

    // Format outside the mutex — vsnprintf can be slow on long lines
    // and we want to keep the critical section as small as possible.
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);
    if (n <= 0) return r;
    // vsnprintf returns the count it WOULD have written, not what
    // fit. Clamp so we never send the trailing NUL down the stream
    // (clients can choke on embedded NULs in the log).
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;

    // send() must happen WITH the mutex held. Otherwise a concurrent
    // listen_task could shutdown+close the old fd we snapshotted, lwIP
    // recycles the fd number, and our send() targets whatever new
    // socket got that number. Holding the mutex serialises send()
    // against the close() in client_task cleanup. send() uses
    // MSG_DONTWAIT so the critical section is bounded.
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int fd = s_client_fd;
    if (fd >= 0) {
        (void)send(fd, buf, n, MSG_DONTWAIT);
    }
    xSemaphoreGive(s_mutex);
    return r;
}

// ---- Command execution using existing esp_console registry -----------

static void exec_line(int fd, char *line) {
    // Strip CR/LF
    size_t n = strlen(line);
    while (n && (line[n-1] == '\r' || line[n-1] == '\n' || line[n-1] == ' ')) {
        line[--n] = 0;
    }
    if (!n) { send(fd, "nivona> ", 8, 0); return; }

    // Redirect stdout to a memory buffer so command's printf() output
    // is captured and sent back to the telnet client.
    //
    // stdout is a process-global FILE*. The swap + restore is protected
    // by s_mutex (which also guards telnet_vprintf) so a concurrent
    // log line from another task can't observe a torn stdout pointer
    // or trample the capture buffer. Audit-V3 finding I4.
    char capture[1024] = {0};
    FILE *mem = fmemopen(capture, sizeof(capture) - 1, "w");
    FILE *saved_stdout = stdout;
    if (mem) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        stdout = mem;
        xSemaphoreGive(s_mutex);
    }

    int ret = 0;
    esp_err_t err = esp_console_run(line, &ret);

    if (mem) {
        fflush(mem);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        stdout = saved_stdout;
        xSemaphoreGive(s_mutex);
        fclose(mem);
    }

    // Send captured command output
    size_t clen = strlen(capture);
    if (clen) send(fd, capture, clen, 0);

    // Append status line for errors
    char buf[96];
    const char *extra = NULL;
    if (err == ESP_ERR_NOT_FOUND) extra = "command not found\n";
    else if (err == ESP_ERR_INVALID_ARG) extra = "invalid args\n";
    else if (err == ESP_OK && ret != 0) {
        snprintf(buf, sizeof(buf), "(rc=%d)\n", ret);
        extra = buf;
    }
    if (extra) send(fd, extra, strlen(extra), 0);
    send(fd, "nivona> ", 8, 0);
}

// ---- Telnet server task -----------------------------------------------

static void client_task(void *arg) {
    int fd = (int)(intptr_t)arg;
    const char *banner = "Nivona emulator telnet. Type 'help'.\nnivona> ";
    send(fd, banner, strlen(banner), 0);

    char buf[128];
    size_t pos = 0;
    while (1) {
        char c;
        int r = recv(fd, &c, 1, 0);
        if (r <= 0) break;
        // Skip telnet IAC sequences (0xFF followed by 2 bytes)
        if ((uint8_t)c == 0xFF) {
            recv(fd, &c, 1, 0); recv(fd, &c, 1, 0);
            continue;
        }
        if (c == '\n' || c == '\r') {
            buf[pos] = 0;
            exec_line(fd, buf);
            pos = 0;
        } else if (pos + 1 < sizeof(buf)) {
            buf[pos++] = c;
        }
    }

    // close() inside the mutex too: between clearing s_client_fd and
    // the close(), lwIP could otherwise hand `fd` to a new socket
    // opened elsewhere, and a concurrent telnet_vprintf snapshot from
    // an earlier moment could end up sending data to that wrong fd.
    // Pairing the close() with the slot clear under one lock means
    // any vprintf that observes s_client_fd == fd is guaranteed the
    // fd is still ours until the lock is released.
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_client_fd == fd) s_client_fd = -1;
    close(fd);
    xSemaphoreGive(s_mutex);
    vTaskDelete(NULL);
}

static void listen_task(void *arg) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        ESP_LOGE(TAG, "socket() failed errno=%d — telnet unavailable", errno);
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {
        .sin_family = AF_INET, .sin_port = htons(TELNET_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind :23 failed errno=%d", errno);
        close(lfd); vTaskDelete(NULL); return;
    }
    if (listen(lfd, 1) < 0) {
        ESP_LOGE(TAG, "listen failed errno=%d", errno);
        close(lfd); vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "telnet listening on :%d", TELNET_PORT);

    while (1) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        // Evict the previous client safely: capture the old fd under
        // the mutex, then shutdown(SHUT_RDWR) it so the old client_task
        // breaks out of its recv() with EOF and cleans up — close()
        // alone races with an in-flight recv() because lwIP may recycle
        // the fd before the old task observes the error. The old task
        // is responsible for the final close() of the old fd.
        // Audit-V3 finding C8.
        int old_fd = -1;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_client_fd >= 0) {
            old_fd = s_client_fd;
        }
        s_client_fd = cfd;
        xSemaphoreGive(s_mutex);
        if (old_fd >= 0) {
            shutdown(old_fd, SHUT_RDWR);
        }
        xTaskCreate(client_task, "telnet_cli", 4096,
                    (void *)(intptr_t)cfd, 5, NULL);
    }
}

int nivona_telnet_start(void) {
    s_mutex = xSemaphoreCreateMutex();
    s_orig_vprintf = esp_log_set_vprintf(telnet_vprintf);
    xTaskCreate(listen_task, "telnet_lst", 4096, NULL, 5, NULL);
    return 0;
}
