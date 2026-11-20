// Buffered log writer shared by the Switch layer (taken from Melee-NX, hence the melee_nx_ names).
// Writes from any thread are copied into a RAM buffer; one low-priority thread writes them to the SD card,
// so a slow card write never stalls the game, audio or GPU threads.
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
// Start after stdout/stderr have been redirected. Before init, writes are synchronous.
void melee_nx_log_init(void);
// Switch: points fd 1/2 at the buffered writer (which then writes logFd); starts it. Returns 1 on success.
int melee_nx_log_capture_stdio(int logFd);
// Milliseconds since the first call (the timestamps in LOGIO lines and other telemetry).
double melee_nx_log_now_ms(void);
// Queues bytes for the writer; drops the record if the buffer is full rather than waiting.
void melee_nx_log_write(const char* bytes, size_t size);
// Queues bytes and waits until they are on the card; for errors that must survive a crash.
void melee_nx_log_write_critical(const char* bytes, size_t size);
// Drains accepted records and commits them. Used for errors, panic and fast exit.
void melee_nx_log_flush(void);
// Joins the writer; used by host regression tests and orderly teardown.
void melee_nx_log_shutdown(void);
#ifdef __cplusplus
}
#endif
