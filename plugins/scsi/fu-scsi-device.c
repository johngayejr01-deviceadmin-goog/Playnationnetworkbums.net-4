/*
 * Copyright (C) 2022 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <scsi/sg.h>

#include "fu-scsi-device.h"

struct _FuScsiDevice {
	FuUdevDevice parent_instance;
	guint64 ffu_timeout;
};

G_DEFINE_TYPE(FuScsiDevice, fu_scsi_device, FU_TYPE_UDEV_DEVICE)

#define BUFFER_VENDOR_MODE 0x01
#define BUFFER_DUFS_MODE   0x02
#define BUFFER_FFU_MODE	   0x0E
#define BUFFER_EHS_MODE	   0x1C

#define SENSE_BUFF_LEN	 18
#define WRITE_BUF_CMDLEN 10
#define READ_BUF_CMDLEN	 10
#define WRITE_BUFFER_CMD 0x3B
#define READ_BUFFER_CMD	 0x3C

static void
fu_scsi_device_to_string(FuDevice *device, guint idt, GString *str)
{
	FuScsiDevice *self = FU_SCSI_DEVICE(device);
	FU_DEVICE_CLASS(fu_scsi_device_parent_class)->to_string(device, idt, str);
	fu_common_string_append_kx(str, idt, "FfuTimeout", self->ffu_timeout);
}

static gboolean
fu_scsi_device_probe(FuDevice *device, GError **error)
{
	FuScsiDevice *self = FU_SCSI_DEVICE(device);
	/*
	 * XXX: pci is a parent node in the specific case, in general, controller
	 * can be on any bus, so we somehow need to get from device node, to
	 * controller node regardless of which bus it is on
	 */
	g_autoptr(FuUdevDevice) device_parent = fu_udev_device_get_parent_with_subsystem(FU_UDEV_DEVICE(device), "pci");
	const gchar *device_file = fu_udev_device_get_device_file(FU_UDEV_DEVICE(device));
	const gchar *name;
	const gchar *vendor;
	const gchar *version;
	guint64 ufs_features = 0;
	g_autofree gchar *vendor_id = NULL;
	g_autofree gchar *vendor_safe = NULL;
	g_autoptr(GPtrArray) block_devs = NULL;

	/* ignore */
	if (g_strcmp0(fu_udev_device_get_devtype(FU_UDEV_DEVICE(device)), "scsi_target") == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "targets are not supported");
		return FALSE;
	}

	/* vendor */
	vendor = fu_udev_device_get_sysfs_attr(FU_UDEV_DEVICE(device), "vendor", NULL);
	if (vendor != NULL)
		vendor_safe = fu_common_strstrip(vendor);
	if (vendor_safe == NULL || g_strcmp0(vendor_safe, "ATA") == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "no assigned vendor");
		return FALSE;
	}
	fu_device_set_vendor(device, vendor);
	vendor_id = g_strdup_printf("SCSI:%s", vendor_safe);
	fu_device_add_vendor_id(device, vendor_id);

	/* name */
	name = fu_udev_device_get_sysfs_attr(FU_UDEV_DEVICE(device), "model", NULL);
	fu_device_set_name(device, name);

	/* version */
	version = fu_udev_device_get_sysfs_attr(FU_UDEV_DEVICE(device), "rev", NULL);
	fu_device_set_version(device, version);

	if (device_parent != NULL) {
		/* Check if this is a UFS device */
		if (fu_udev_device_get_sysfs_attr_uint64(device_parent,
							 "device_descriptor/ufs_features",
							 &ufs_features,
							 NULL)) {
			fu_device_set_summary(device, "UFS device");
			/* Least significant bit specifies FFU capability. */
			if (ufs_features & 0x1) {
				fu_device_add_flag(device, FWUPD_DEVICE_FLAG_UPDATABLE);
				fu_device_add_protocol(device, "org.jedec.ufs");
			}
			if (!fu_udev_device_get_sysfs_attr_uint64(device_parent,
								  "device_descriptor/ffu_timeout",
								  &self->ffu_timeout,
								  error)) {
				g_prefix_error(error, "no ffu timeout specified: ");
				return FALSE;
			}
		}
	}

	/* add GUIDs */
	fu_device_add_instance_strsafe(device, "VEN", vendor);
	fu_device_add_instance_strsafe(device, "DEV", name);
	fu_device_add_instance_strsafe(device, "REV", version);
	if (!fu_device_build_instance_id_quirk(device, error, "SCSI", "VEN", NULL))
		return FALSE;
	if (!fu_device_build_instance_id(device, error, "SCSI", "VEN", "DEV", NULL))
		return FALSE;
	if (!fu_device_build_instance_id(device, error, "SCSI", "VEN", "DEV", "REV", NULL))
		return FALSE;
	/* check all block devices, although there should only be one */
	block_devs = fu_udev_device_get_children_with_subsystem(FU_UDEV_DEVICE(device), "block");
	for (guint i = 0; i < block_devs->len; i++) {
		FuUdevDevice *block_dev = g_ptr_array_index(block_devs, i);
		guint64 value = 0;
		if (!fu_udev_device_get_sysfs_attr_uint64(block_dev, "removable", &value, NULL))
			continue;
		if (value == 0x0) {
			fu_device_add_flag(device, FWUPD_DEVICE_FLAG_INTERNAL);
			break;
		}
	}

	/* set the physical ID */
	return fu_udev_device_set_physical_id(FU_UDEV_DEVICE(device), "scsi", error);
}

static FuFirmware *
fu_scsi_device_prepare_firmware(FuDevice *device,
				GBytes *fw,
				FwupdInstallFlags flags,
				GError **error)
{
	g_autoptr(FuFirmware) firmware = fu_firmware_new();
	fu_firmware_set_alignment(firmware, FU_FIRMWARE_ALIGNMENT_4K);
	if (!fu_firmware_parse(firmware, fw, flags, error))
		return NULL;
	return g_steal_pointer(&firmware);
}

static const gchar *
fu_scsi_device_sense_key_to_string(guint8 key)
{
	if (key == 0x00)
		return "no-sense";
	if (key == 0x01)
		return "recovered-error";
	if (key == 0x02)
		return "not-ready";
	if (key == 0x03)
		return "medium-error";
	if (key == 0x04)
		return "hardware-error";
	if (key == 0x05)
		return "illegal-request";
	if (key == 0x06)
		return "unit-attention";
	if (key == 0x07)
		return "data-protect";
	if (key == 0x08)
		return "blank-check";
	if (key == 0x09)
		return "vendor-specific";
	if (key == 0x0A)
		return "copy-aborted";
	if (key == 0x0B)
		return "aborted-command";
	if (key == 0x0C)
		return "equal";
	if (key == 0x0D)
		return "volume-overflow";
	if (key == 0x0E)
		return "miscompare";
	return NULL;
}

static gboolean
fu_scsi_device_send_scsi_cmd_v3(FuScsiDevice *self,
				const guint8 *cdb,
				guint8 cdbsz,
				const guint8 *buf,
				guint32 bufsz,
				gint dir,
				GError **error)
{
	guint8 sense_buffer[SENSE_BUFF_LEN] = {0};
	struct sg_io_hdr io_hdr = {.interface_id = 'S'};

	io_hdr.cmd_len = cdbsz;
	io_hdr.mx_sb_len = sizeof(sense_buffer);
	io_hdr.dxfer_direction = dir;
	io_hdr.dxfer_len = bufsz;
	io_hdr.dxferp = (guint8 *)buf;
	/* pointer to command buf */
	io_hdr.cmdp = (guint8 *)cdb;
	io_hdr.sbp = sense_buffer;
	io_hdr.timeout = 60000; /* ms */

	g_debug("cmd=0x%x len=0x%x", cdb[0], (guint)bufsz);
	if (!fu_udev_device_ioctl_full(FU_UDEV_DEVICE(self),
				       SG_IO,
				       (guint8 *)&io_hdr,
				       NULL,
				       5000,
				       error))
		return FALSE;

	if (io_hdr.status) {
		g_set_error(error,
			    G_IO_ERROR,
			    G_IO_ERROR_FAILED,
			    "Command fail with status %x, senseKey %s, asc 0x%02x, ascq 0x%02x",
			    io_hdr.status,
			    fu_scsi_device_sense_key_to_string(sense_buffer[2]),
			    sense_buffer[12],
			    sense_buffer[13]);
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_scsi_device_write_firmware(FuDevice *device,
			      FuFirmware *firmware,
			      FuProgress *progress,
			      FwupdInstallFlags flags,
			      GError **error)
{
	FuScsiDevice *self = FU_SCSI_DEVICE(device);
	guint32 chunksz = 0x1000;
	g_autoptr(GBytes) fw = NULL;
	g_autoptr(GPtrArray) chunks = NULL;

	/* get default image */
	fw = fu_firmware_get_bytes(firmware, error);
	if (fw == NULL)
		return FALSE;

	/* prepare chunks */
	chunks = fu_chunk_array_new_from_bytes(fw, 0x00, 0x00, chunksz);
	fu_progress_set_status(progress, FWUPD_STATUS_DEVICE_WRITE);
	fu_progress_set_steps(progress, chunks->len);

	/* write each block */
	for (guint i = 0; i < chunks->len; i++) {
		FuChunk *chk = g_ptr_array_index(chunks, i);
		guint8 cdb[WRITE_BUF_CMDLEN] = {WRITE_BUFFER_CMD,
						BUFFER_FFU_MODE,
						0x0 /* buf_id */};

		fu_common_write_uint24(cdb + 3, fu_chunk_get_address(chk), G_LITTLE_ENDIAN);
		fu_common_write_uint24(cdb + 6, fu_chunk_get_data_sz(chk), G_LITTLE_ENDIAN);
		if (!fu_scsi_device_send_scsi_cmd_v3(self,
						     cdb,
						     sizeof(cdb),
						     fu_chunk_get_data(chk),
						     fu_chunk_get_data_sz(chk),
						     SG_DXFER_TO_DEV,
						     error)) {
			g_prefix_error(error,
				       "SG_IO WRITE BUFFER data error for v3 chunk 0x%x: ",
				       fu_chunk_get_idx(chk));

			return FALSE;
		}

		/* wait for the timeout */
		fu_progress_sleep(fu_progress_get_child(progress), self->ffu_timeout * 1000);

		/* chunk done */
		fu_progress_step_done(progress);
	}

	/* success! */
	fu_device_add_flag(device, FWUPD_DEVICE_FLAG_NEEDS_REBOOT);
	return TRUE;
}

static void
fu_scsi_device_set_progress(FuDevice *self, FuProgress *progress)
{
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_add_flag(progress, FU_PROGRESS_FLAG_GUESSED);
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 0); /* detach */
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 98);	/* write */
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 0); /* attach */
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 2);	/* reload */
}

static void
fu_scsi_device_init(FuScsiDevice *self)
{
	fu_device_add_icon(FU_DEVICE(self), "drive-harddisk");
	fu_device_set_version_format(FU_DEVICE(self), FWUPD_VERSION_FORMAT_PLAIN);
	fu_device_set_summary(FU_DEVICE(self), "SCSI device");
	fu_udev_device_set_flags(FU_UDEV_DEVICE(self),
				 FU_UDEV_DEVICE_FLAG_OPEN_READ | FU_UDEV_DEVICE_FLAG_OPEN_SYNC |
				     FU_UDEV_DEVICE_FLAG_IOCTL_RETRY);
}

static void
fu_scsi_device_finalize(GObject *object)
{
	G_OBJECT_CLASS(fu_scsi_device_parent_class)->finalize(object);
}

static void
fu_scsi_device_class_init(FuScsiDeviceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	FuDeviceClass *klass_device = FU_DEVICE_CLASS(klass);
	object_class->finalize = fu_scsi_device_finalize;
	klass_device->to_string = fu_scsi_device_to_string;
	klass_device->probe = fu_scsi_device_probe;
	klass_device->prepare_firmware = fu_scsi_device_prepare_firmware;
	klass_device->write_firmware = fu_scsi_device_write_firmware;
	klass_device->set_progress = fu_scsi_device_set_progress;
}
