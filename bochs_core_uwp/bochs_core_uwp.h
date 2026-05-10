#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int bochs_core_uwp_run(const char *config_path);
int bochs_core_uwp_run_with_restore(const char *config_path, const char *restore_path);
void bochs_core_uwp_pause(void);
void bochs_core_uwp_resume(void);
void bochs_core_uwp_request_shutdown(void);
int bochs_core_uwp_wait_until_paused(unsigned timeout_ms);
int bochs_core_uwp_poll(void);
int bochs_core_uwp_save_state(const char *checkpoint_path);
void bochs_core_uwp_clear_log(void);
void bochs_core_uwp_report_log(int level, const char *prefix, const char *message);
size_t bochs_core_uwp_copy_log(char *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif
