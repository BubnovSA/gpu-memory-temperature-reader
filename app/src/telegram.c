#include "telegram.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

int telegram_enabled(const struct telegram_cfg *cfg)
{
    return cfg && cfg->token && *cfg->token && cfg->chat_id && *cfg->chat_id;
}

void telegram_send(const struct telegram_cfg *cfg, const char *text)
{
    if (!telegram_enabled(cfg) || !text) return;

    char url[512];
    snprintf(url, sizeof url,
             "https://api.telegram.org/bot%s/sendMessage", cfg->token);

    char chat_field[128];
    snprintf(chat_field, sizeof chat_field, "chat_id=%s", cfg->chat_id);

    size_t tlen = strlen(text);
    size_t flen = tlen + 6;
    char *text_field = malloc(flen);
    if (!text_field) return;
    snprintf(text_field, flen, "text=%s", text);

    pid_t p = fork();
    if (p < 0) {
        free(text_field);
        return;
    }
    if (p == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, 1);
            dup2(devnull, 2);
            close(devnull);
        }
        execlp("curl", "curl", "-sS", "-X", "POST",
               "--max-time", "5",
               "--data-urlencode", chat_field,
               "--data-urlencode", text_field,
               url, (char *)NULL);
        _exit(127);
    }
    free(text_field);
    /* Auto-reaped via SIGCHLD=SIG_IGN set in main(). */
}
