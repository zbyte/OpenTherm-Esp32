/**
 * @file log_ring.h
 * @brief Keeps the most recent log text in RAM for GET /log and the remote
 *        console (debugging when the USB console is not attached).
 */

#ifndef LOG_RING_H_
#define LOG_RING_H_

#include <stddef.h>
#include <stdint.h>

void   log_ring_init(void);
void   log_ring_output(const char *str); /* tal_log output callback */
size_t log_ring_read(char *buf, size_t cap);

/* Position in the log stream; log_ring_since() copies what was written after
 * it (the tail, if more than cap), e.g. the output of one console command. */
uint32_t log_ring_mark(void);
size_t   log_ring_since(uint32_t mark, char *buf, size_t cap);

#endif /* LOG_RING_H_ */
