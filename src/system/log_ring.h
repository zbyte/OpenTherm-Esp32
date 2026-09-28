/**
 * @file log_ring.h
 * @brief Keeps the most recent log text in RAM for GET /log (remote debugging
 *        when the USB console is not attached).
 */

#ifndef LOG_RING_H_
#define LOG_RING_H_

#include <stddef.h>

void   log_ring_init(void);
void   log_ring_output(const char *str); /* tal_log output callback */
size_t log_ring_read(char *buf, size_t cap);

#endif /* LOG_RING_H_ */
