/* LinuxKPI: SCSI error handling helpers (include/scsi/scsi_eh.h). The
 * midlayer's recovery is simple (abort, then host reset, see glue/scsi.c);
 * drivers only use the sense helpers from here. */
#pragma once
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_common.h>

static inline bool scsi_command_normalize_sense(const struct scsi_cmnd *cmd, struct scsi_sense_hdr *sshdr)
{
	return scsi_normalize_sense(cmd->sense_buffer, SCSI_SENSE_BUFFERSIZE, sshdr);
}
static inline bool scsi_sense_is_deferred(const struct scsi_sense_hdr *sshdr)
{
	return (sshdr->response_code & 0x0e) == 0x02;
}
static inline void scsi_report_bus_reset(struct Scsi_Host *shost, int channel) { }
static inline void scsi_report_device_reset(struct Scsi_Host *shost, int channel, int target) { }
static inline int scsi_block_when_processing_errors(struct scsi_device *sdev) { return 1; }
