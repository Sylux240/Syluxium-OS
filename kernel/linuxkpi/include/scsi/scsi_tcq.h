/* LinuxKPI: tag lookup (include/scsi/scsi_tcq.h). */
#pragma once
#include <linux/blkdev.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>

#define SCSI_NO_TAG (-1)
/* The in-flight command with this (unique) tag, or NULL (glue/scsi.c). */
struct scsi_cmnd *scsi_host_find_tag(struct Scsi_Host *shost, int tag);
