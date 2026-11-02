#ifndef HANDLERS_H
#define HANDLERS_H

#include "http.h"
#include "static.h"

/* Dispatch a parsed request: built-in routes first, then the file server. */
void route(struct http_ctx *ctx, const struct static_config *files);

#endif
