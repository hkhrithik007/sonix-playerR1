#ifndef RECOVERYPREP_H
#define RECOVERYPREP_H
#include <stdbool.h>
typedef enum {
	RECOVERY_PREP_UNSUPPORTED,
	RECOVERY_PREP_READY,
	RECOVERY_PREP_FIXED,
	RECOVERY_PREP_BACKED_UP,
	RECOVERY_PREP_FAILED,
	RECOVERY_PREP_UNCERTAIN,
} recovery_prep_result_t;
// Maintenance ownership blocks normal reboot, update and suspend. An uncertain
// readback keeps this held until the running OS is recovered deliberately.
bool recovery_prep_active(void);
void recovery_prep_set_active(bool active);
// Blocking operations, worker thread only. No operation selects boot or reboots.
recovery_prep_result_t recovery_prep_check(void);
recovery_prep_result_t recovery_prep_backup(void);
// Called only after an explicit confirmation of the separate recovery write.
recovery_prep_result_t recovery_prep_apply(void);
#endif
