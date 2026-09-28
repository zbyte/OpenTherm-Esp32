/**
 * @file web.h
 * @brief Minimal local web UI: status page with forms (GET /, POST /set).
 */

#ifndef WEB_H_
#define WEB_H_

#include <stddef.h>

size_t web_page(char *out, size_t cap);
void   web_form(const char *body);

#endif /* WEB_H_ */
