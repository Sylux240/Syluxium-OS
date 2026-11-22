/* LinuxKPI: SCSI debug printing (include/scsi/scsi_dbg.h): the command
 * bytes are printed, sense and result decoding is left out. */
#pragma once
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_common.h>

static inline void scsi_print_sense_hdr(const struct scsi_device *sdev, const char *name,
					const struct scsi_sense_hdr *sshdr) { }
static inline const char *scsi_sense_key_string(unsigned char key) { return NULL; }
static inline const char *scsi_hostbyte_string(int result) { return NULL; }
static inline const char *scsi_mlreturn_string(int result) { return NULL; }
