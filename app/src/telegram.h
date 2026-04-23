#ifndef TELEGRAM_H
#define TELEGRAM_H

struct telegram_cfg {
    const char *token;
    const char *chat_id;
};

int  telegram_enabled(const struct telegram_cfg *cfg);
void telegram_send(const struct telegram_cfg *cfg, const char *text);

#endif
