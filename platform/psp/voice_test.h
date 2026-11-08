#ifndef ST_PSP_VOICE_TEST_H
#define ST_PSP_VOICE_TEST_H
void st_voice_test_open(void);
/* Reap only stopped workers; never join capture/output syscalls from the UI. */
void st_voice_test_poll(void);
#endif
