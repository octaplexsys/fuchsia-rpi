// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/driver.h>
#include <usb/usb-request.h>
#include <usb/usb.h>
#include <zircon/assert.h>
#include <zircon/hw/usb.h>
#include <zircon/hw/usb/ums.h>

#include <endian.h>
#include <stdio.h>
#include <string.h>

#include "usb-mass-storage.h"

// comment the next line if you don't want debug messages
#define DEBUG 0
#ifdef DEBUG
#define DEBUG_PRINT(x) printf x
#else
#define DEBUG_PRINT(x) \
    do {               \
    } while (0)
#endif

static csw_status_t ums_verify_csw(ums_t* ums, usb_request_t* csw_request, uint32_t* out_residue);

static inline void txn_complete(ums_txn_t* txn, zx_status_t status) {
    zxlogf(TRACE, "UMS DONE %d (%p)\n", status, &txn->op);
    txn->completion_cb(txn->cookie, status, &txn->op);
}

static zx_status_t ums_reset(ums_t* ums) {
    // UMS Reset Recovery. See section 5.3.4 of
    // "Universal Serial Bus Mass Storage Class Bulk-Only Transport"
    DEBUG_PRINT(("UMS: performing reset recovery\n"));
    // Step 1: Send  Bulk-Only Mass Storage Reset
    zx_status_t status = usb_control_out(&ums->usb, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                                         USB_REQ_RESET, 0, ums->interface_number, ZX_TIME_INFINITE,
                                         NULL, 0);
    if (status != ZX_OK) {
        DEBUG_PRINT(("UMS: USB_REQ_RESET failed %d\n", status));
        return status;
    }
    // Step 2: Clear Feature HALT to the Bulk-In endpoint
    status = usb_clear_feature(&ums->usb, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_ENDPOINT,
                               USB_ENDPOINT_HALT, ums->bulk_in_addr, ZX_TIME_INFINITE);
    if (status != ZX_OK) {
        DEBUG_PRINT(("UMS: clear endpoint halt failed %d\n", status));
        return status;
    }
    // Step 3: Clear Feature HALT to the Bulk-Out endpoint
    status = usb_clear_feature(&ums->usb, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_ENDPOINT,
                               USB_ENDPOINT_HALT, ums->bulk_out_addr, ZX_TIME_INFINITE);
    if (status != ZX_OK) {
        DEBUG_PRINT(("UMS: clear endpoint halt failed %d\n", status));
        return status;
    }
    return ZX_OK;
}

static void ums_req_complete(void* ctx, usb_request_t* req) {
    if (ctx) {
        sync_completion_signal((sync_completion_t*)ctx);
    }
}

static void ums_send_cbw(ums_t* ums, uint8_t lun, uint32_t transfer_length, uint8_t flags,
                         uint8_t command_len, void* command) {
    usb_request_t* req = ums->cbw_req;

    ums_cbw_t* cbw;
    zx_status_t status = usb_request_mmap(req, (void**)&cbw);
    if (status != ZX_OK) {
        DEBUG_PRINT(("UMS: usb request mmap failed: %d\n", status));
        return;
    }

    memset(cbw, 0, sizeof(*cbw));
    cbw->dCBWSignature = htole32(CBW_SIGNATURE);
    cbw->dCBWTag = htole32(ums->tag_send++);
    cbw->dCBWDataTransferLength = htole32(transfer_length);
    cbw->bmCBWFlags = flags;
    cbw->bCBWLUN = lun;
    cbw->bCBWCBLength = command_len;

    // copy command_len bytes from the command passed in into the command_len
    memcpy(cbw->CBWCB, command, command_len);

    sync_completion_t completion;
    usb_request_complete_t complete = {
        .callback = ums_req_complete,
        .ctx = &completion,
    };
    usb_request_queue(&ums->usb, req, &complete);
    sync_completion_wait(&completion, ZX_TIME_INFINITE);
}

static zx_status_t ums_read_csw(ums_t* ums, uint32_t* out_residue) {
    sync_completion_t completion;
    usb_request_complete_t complete = {
        .callback = ums_req_complete,
        .ctx = &completion,
    };

    usb_request_t* csw_request = ums->csw_req;
    usb_request_queue(&ums->usb, csw_request, &complete);
    sync_completion_wait(&completion, ZX_TIME_INFINITE);

    csw_status_t csw_error = ums_verify_csw(ums, csw_request, out_residue);

    if (csw_error == CSW_SUCCESS) {
        return ZX_OK;
    } else if (csw_error == CSW_FAILED) {
        return ZX_ERR_BAD_STATE;
    } else {
        // FIXME - best way to handle this?
        // print error and then reset device due to it
        DEBUG_PRINT(("UMS: CSW verify returned error. Check ums-hw.h csw_status_t for enum = %d\n", csw_error));
        ums_reset(ums);
        return ZX_ERR_INTERNAL;
    }
}

static csw_status_t ums_verify_csw(ums_t* ums, usb_request_t* csw_request, uint32_t* out_residue) {
    ums_csw_t csw;
    usb_request_copy_from(csw_request, &csw, sizeof(csw), 0);

    // check signature is "USBS"
    if (letoh32(csw.dCSWSignature) != CSW_SIGNATURE) {
        DEBUG_PRINT(("UMS:invalid csw sig: %08x \n", letoh32(csw.dCSWSignature)));
        return CSW_INVALID;
    }
    // check if tag matches the tag of last CBW
    if (letoh32(csw.dCSWTag) != ums->tag_receive++) {
        DEBUG_PRINT(("UMS:csw tag mismatch, expected:%08x got in csw:%08x \n", ums->tag_receive - 1,
                     letoh32(csw.dCSWTag)));
        return CSW_TAG_MISMATCH;
    }
    // check if success is true or not?
    if (csw.bmCSWStatus == CSW_FAILED) {
        return CSW_FAILED;
    } else if (csw.bmCSWStatus == CSW_PHASE_ERROR) {
        return CSW_PHASE_ERROR;
    }

    if (out_residue) {
        *out_residue = letoh32(csw.dCSWDataResidue);
    }
    return CSW_SUCCESS;
}

static void ums_queue_read(ums_t* ums, uint16_t transfer_length) {
    // read request sense response
    usb_request_t* read_request = ums->data_req;
    read_request->header.length = transfer_length;
    usb_request_complete_t complete = {
        .callback = ums_req_complete,
        .ctx = NULL,
    };

    usb_request_queue(&ums->usb, read_request, &complete);
}

static zx_status_t ums_inquiry(ums_t* ums, uint8_t lun, uint8_t* out_data) {
    // CBW Configuration
    scsi_command6_t command;
    memset(&command, 0, sizeof(command));
    command.opcode = UMS_INQUIRY;
    command.length = UMS_INQUIRY_TRANSFER_LENGTH;
    ums_send_cbw(ums, lun, UMS_INQUIRY_TRANSFER_LENGTH, USB_DIR_IN, sizeof(command), &command);

    // read inquiry response
    ums_queue_read(ums, UMS_INQUIRY_TRANSFER_LENGTH);

    // wait for CSW
    zx_status_t status = ums_read_csw(ums, NULL);
    if (status == ZX_OK) {
        usb_request_copy_from(ums->data_req, out_data, UMS_INQUIRY_TRANSFER_LENGTH, 0);
    }
    return status;
}

static zx_status_t ums_test_unit_ready(ums_t* ums, uint8_t lun) {
    // CBW Configuration
    scsi_command6_t command;
    memset(&command, 0, sizeof(command));
    command.opcode = UMS_TEST_UNIT_READY;
    ums_send_cbw(ums, lun, 0, USB_DIR_IN, sizeof(command), &command);

    // wait for CSW
    return ums_read_csw(ums, NULL);
}

static zx_status_t ums_request_sense(ums_t* ums, uint8_t lun, uint8_t* out_data) {
    // CBW Configuration
    scsi_command6_t command;
    memset(&command, 0, sizeof(command));
    command.opcode = UMS_REQUEST_SENSE;
    command.length = UMS_REQUEST_SENSE_TRANSFER_LENGTH;
    ums_send_cbw(ums, lun, UMS_REQUEST_SENSE_TRANSFER_LENGTH, USB_DIR_IN, sizeof(command), &command);

    // read request sense response
    ums_queue_read(ums, UMS_REQUEST_SENSE_TRANSFER_LENGTH);

    // wait for CSW
    zx_status_t status = ums_read_csw(ums, NULL);
    if (status == ZX_OK) {
        usb_request_copy_from(ums->data_req, out_data, UMS_REQUEST_SENSE_TRANSFER_LENGTH, 0);
    }
    return status;
}

static zx_status_t ums_read_capacity10(ums_t* ums, uint8_t lun, scsi_read_capacity_10_t* out_data) {
    // CBW Configuration
    scsi_command10_t command;
    memset(&command, 0, sizeof(command));
    command.opcode = UMS_READ_CAPACITY10;
    ums_send_cbw(ums, lun, sizeof(*out_data), USB_DIR_IN, sizeof(command), &command);

    // read capacity10 response
    ums_queue_read(ums, sizeof(*out_data));

    zx_status_t status = ums_read_csw(ums, NULL);
    if (status == ZX_OK) {
        usb_request_copy_from(ums->data_req, out_data, sizeof(*out_data), 0);
    }
    return status;
}

static zx_status_t ums_read_capacity16(ums_t* ums, uint8_t lun, scsi_read_capacity_16_t* out_data) {
    // CBW Configuration
    scsi_command16_t command;
    memset(&command, 0, sizeof(command));
    command.opcode = UMS_READ_CAPACITY16;
    // service action = 10, not sure what that means
    command.misc = 0x10;
    command.length = sizeof(*out_data);
    ums_send_cbw(ums, lun, sizeof(*out_data), USB_DIR_IN, sizeof(command), &command);

    // read capacity16 response
    ums_queue_read(ums, sizeof(*out_data));

    zx_status_t status = ums_read_csw(ums, NULL);
    if (status == ZX_OK) {
        usb_request_copy_from(ums->data_req, out_data, sizeof(*out_data), 0);
    }
    return status;
}

static zx_status_t ums_mode_sense6(ums_t* ums, uint8_t lun, scsi_mode_sense_6_data_t* out_data) {
    // CBW Configuration
    scsi_mode_sense_6_command_t command;
    memset(&command, 0, sizeof(command));
    command.opcode = UMS_MODE_SENSE6;
    command.page = 0x3F; // all pages, current values
    command.allocation_length = sizeof(*out_data);

    ums_send_cbw(ums, lun, sizeof(*out_data), USB_DIR_IN, sizeof(command), &command);

    // read mode sense response
    ums_queue_read(ums, sizeof(*out_data));

    zx_status_t status = ums_read_csw(ums, NULL);
    if (status == ZX_OK) {
        usb_request_copy_from(ums->data_req, out_data, sizeof(*out_data), 0);
    }
    return status;
}

static zx_status_t ums_mode_sense_6(ums_t* ums, uint8_t lun, uint8_t page,
                                    void* data, uint8_t transfer_length) {
    // CBW Configuration
    scsi_mode_sense_6_command_t command;
    memset(&command, 0, sizeof(command));
    command.opcode = UMS_MODE_SENSE6;
    command.page = page; // all pages, current values
    command.allocation_length = transfer_length;

    ums_send_cbw(ums, lun, transfer_length, USB_DIR_IN, sizeof(command), &command);

    // read mode sense response
    ums_queue_read(ums, transfer_length);

    zx_status_t status = ums_read_csw(ums, NULL);
    if (status == ZX_OK) {
        usb_request_copy_from(ums->data_req, data, transfer_length, 0);
    }
    return status;
}

static zx_status_t ums_data_transfer(ums_t* ums, ums_txn_t* txn, zx_off_t offset, size_t length,
                                     uint8_t ep_address) {
    usb_request_t* req = ums->data_transfer_req;

    zx_status_t status = usb_request_init(req, txn->op.rw.vmo, offset, length, ep_address);
    if (status != ZX_OK) {
        return status;
    }

    sync_completion_t completion;
    usb_request_complete_t complete = {
        .callback = ums_req_complete,
        .ctx = &completion,
    };
    usb_request_queue(&ums->usb, req, &complete);
    sync_completion_wait(&completion, ZX_TIME_INFINITE);

    status = req->response.status;
    if (status == ZX_OK && req->response.actual != length) {
        status = ZX_ERR_IO;
    }

    usb_request_release(req);
    return status;
}

static zx_status_t ums_read(ums_block_t* dev, ums_txn_t* txn) {
    ums_t* ums = block_to_ums(dev);

    zx_off_t block_offset = txn->op.rw.offset_dev;
    uint32_t num_blocks = txn->op.rw.length;
    if ((block_offset >= dev->total_blocks) || ((dev->total_blocks - block_offset) < num_blocks)) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    size_t block_size = dev->block_size;
    zx_off_t vmo_offset = txn->op.rw.offset_vmo * block_size;
    size_t max_blocks = ums->max_transfer / block_size;
    zx_status_t status = ZX_OK;

    while (status == ZX_OK && num_blocks > 0) {
        size_t blocks = num_blocks;
        if (blocks > max_blocks) {
            blocks = max_blocks;
        }
        size_t length = blocks * block_size;

        // CBW Configuration
        // Need to use UMS_READ16 if block addresses are greater than 32 bit
        if (dev->total_blocks > UINT32_MAX) {
            scsi_command16_t command;
            memset(&command, 0, sizeof(command));
            command.opcode = UMS_READ16;
            command.lba = htobe64(block_offset);
            command.length = htobe32(static_cast<uint32_t>(blocks));
            ums_send_cbw(ums, dev->lun, static_cast<uint32_t>(length), USB_DIR_IN, sizeof(command), &command);
        } else if (blocks <= UINT16_MAX) {
            scsi_command10_t command;
            memset(&command, 0, sizeof(command));
            command.opcode = UMS_READ10;
            command.lba = htobe32(static_cast<uint32_t>(block_offset));
            command.length_hi = static_cast<uint8_t>(blocks >> 8);
            command.length_lo = static_cast<uint8_t>(blocks & 0xFF);
            ums_send_cbw(ums, dev->lun, static_cast<uint32_t>(length), USB_DIR_IN, sizeof(command), &command);
        } else {
            scsi_command12_t command;
            memset(&command, 0, sizeof(command));
            command.opcode = UMS_READ12;
            command.lba = htobe32(static_cast<uint32_t>(block_offset));
            command.length = htobe32(static_cast<uint32_t>(blocks));
            ums_send_cbw(ums, dev->lun, static_cast<uint32_t>(length), USB_DIR_IN, sizeof(command), &command);
        }

        status = ums_data_transfer(ums, txn, vmo_offset, length, ums->bulk_in_addr);

        block_offset += blocks;
        num_blocks -= static_cast<uint32_t>(blocks);
        vmo_offset += (blocks * block_size);

        // receive CSW
        uint32_t residue;
        status = ums_read_csw(ums, &residue);
        if (status == ZX_OK && residue) {
            zxlogf(ERROR, "unexpected residue in ums_read\n");
            status = ZX_ERR_IO;
        }
    }

    return status;
}

static zx_status_t ums_write(ums_block_t* dev, ums_txn_t* txn) {
    ums_t* ums = block_to_ums(dev);

    zx_off_t block_offset = txn->op.rw.offset_dev;
    uint32_t num_blocks = txn->op.rw.length;
    if ((block_offset >= dev->total_blocks) || ((dev->total_blocks - block_offset) < num_blocks)) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    size_t block_size = dev->block_size;
    zx_off_t vmo_offset = txn->op.rw.offset_vmo * block_size;
    size_t max_blocks = ums->max_transfer / block_size;
    zx_status_t status = ZX_OK;

    while (status == ZX_OK && num_blocks > 0) {
        size_t blocks = num_blocks;
        if (blocks > max_blocks) {
            blocks = max_blocks;
        }
        size_t length = blocks * block_size;

        // CBW Configuration
        // Need to use UMS_WRITE16 if block addresses are greater than 32 bit
        if (dev->total_blocks > UINT32_MAX) {
            scsi_command16_t command;
            memset(&command, 0, sizeof(command));
            command.opcode = UMS_WRITE16;
            command.lba = htobe64(block_offset);
            command.length = htobe32(static_cast<uint32_t>(blocks));
            ums_send_cbw(ums, dev->lun, static_cast<uint32_t>(length), USB_DIR_OUT, sizeof(command), &command);
        } else if (blocks <= UINT16_MAX) {
            scsi_command10_t command;
            memset(&command, 0, sizeof(command));
            command.opcode = UMS_WRITE10;
            command.lba = htobe32(static_cast<uint32_t>(block_offset));
            command.length_hi = static_cast<uint8_t>(static_cast<uint32_t>(blocks) >> 8);
            command.length_lo = static_cast<uint8_t>(static_cast<uint32_t>(blocks) & 0xFF);
            ums_send_cbw(ums, dev->lun, static_cast<uint32_t>(length), USB_DIR_OUT, sizeof(command), &command);
        } else {
            scsi_command12_t command;
            memset(&command, 0, sizeof(command));
            command.opcode = UMS_WRITE12;
            command.lba = htobe32(static_cast<uint32_t>(block_offset));
            command.length = htobe32(static_cast<uint32_t>(blocks));
            ums_send_cbw(ums, dev->lun, static_cast<uint32_t>(length), USB_DIR_OUT, sizeof(command), &command);
        }

        status = ums_data_transfer(ums, txn, vmo_offset, length, ums->bulk_out_addr);

        block_offset += blocks;
        num_blocks -= static_cast<uint32_t>(blocks);
        vmo_offset += (blocks * block_size);

        // receive CSW
        uint32_t residue;
        status = ums_read_csw(ums, &residue);
        if (status == ZX_OK && residue) {
            zxlogf(ERROR, "unexpected residue in ums_write\n");
            status = ZX_ERR_IO;
        }
    }

    return status;
}

static void ums_unbind(void* ctx) {
    ums_t* ums = static_cast<ums_t*>(ctx);

    // terminate our worker thread
    mtx_lock(&ums->txn_lock);
    ums->dead = true;
    mtx_unlock(&ums->txn_lock);
    sync_completion_signal(&ums->txn_completion);

    // wait for worker thread to finish before removing devices
    thrd_join(ums->worker_thread, NULL);

    for (uint8_t lun = 0; lun <= ums->max_lun; lun++) {
        ums_block_t* dev = &ums->block_devs[lun];

        if (dev->device_added) {
            device_remove(dev->zxdev);
        }
    }

    // remove our root device
    device_remove(ums->zxdev);
}

static void ums_release(void* ctx) {
    ums_t* ums = static_cast<ums_t*>(ctx);

    if (ums->cbw_req) {
        usb_request_release(ums->cbw_req);
    }
    if (ums->data_req) {
        usb_request_release(ums->data_req);
    }
    if (ums->csw_req) {
        usb_request_release(ums->csw_req);
    }
    if (ums->data_transfer_req) {
        usb_request_release(ums->data_transfer_req);
        //The release_cb of data_transfer_req does not free the req.
        free(ums->data_transfer_req);
    }

    free(ums);
}

static zx_status_t ums_add_block_device(ums_block_t* dev) {
    ums_t* ums = block_to_ums(dev);
    uint8_t lun = dev->lun;

    scsi_read_capacity_10_t data;
    zx_status_t status = ums_read_capacity10(ums, lun, &data);
    if (status < 0) {
        zxlogf(ERROR, "read_capacity10 failed: %d\n", status);
        return status;
    }

    dev->total_blocks = betoh32(data.lba);
    dev->block_size = betoh32(data.block_length);

    if (dev->total_blocks == 0xFFFFFFFF) {
        scsi_read_capacity_16_t data;
        status = ums_read_capacity16(ums, lun, &data);
        if (status < 0) {
            zxlogf(ERROR, "read_capacity16 failed: %d\n", status);
            return status;
        }

        dev->total_blocks = betoh64(data.lba);
        dev->block_size = betoh32(data.block_length);
    }
    if (dev->block_size == 0) {
        zxlogf(ERROR, "UMS zero block size\n");
        return ZX_ERR_INVALID_ARGS;
    }

    // +1 because this returns the address of the final block, and blocks are zero indexed
    dev->total_blocks++;

    // determine if LUN is read-only
    scsi_mode_sense_6_data_t ms_data;
    status = ums_mode_sense6(ums, lun, &ms_data);
    if (status != ZX_OK) {
        zxlogf(ERROR, "ums_mode_sense6 failed: %d\n", status);
        return status;
    }

    unsigned char cache_sense[20];
    status = ums_mode_sense_6(ums, lun, 0x08, cache_sense, sizeof(cache_sense));
    if (status != ZX_OK) {
        zxlogf(WARN, "CacheSense failed: %d\n", status);
        ums->cache_enabled = true;
    } else {
        ums->cache_enabled = cache_sense[6] & (1 << 2);
    }

    if (ms_data.device_specific_param & MODE_SENSE_DSP_RO) {
        dev->flags |= BLOCK_FLAG_READONLY;
    } else {
        dev->flags &= ~BLOCK_FLAG_READONLY;
    }

    DEBUG_PRINT(("UMS: block size is: 0x%08x\n", dev->block_size));
    DEBUG_PRINT(("UMS: total blocks is: %" PRId64 "\n", dev->total_blocks));
    DEBUG_PRINT(("UMS: total size is: %" PRId64 "\n", dev->total_blocks * dev->block_size));
    DEBUG_PRINT(("UMS: read-only: %d removable: %d\n", !!(dev->flags & BLOCK_FLAG_READONLY),
                 !!(dev->flags & BLOCK_FLAG_REMOVABLE)));

    return ums_block_add_device(ums, dev);
}

static zx_status_t ums_check_luns_ready(ums_t* ums) {
    zx_status_t status = ZX_OK;

    for (uint8_t lun = 0; lun <= ums->max_lun && status == ZX_OK; lun++) {
        ums_block_t* dev = &ums->block_devs[lun];
        bool ready = false;

        status = ums_test_unit_ready(ums, lun);
        if (status == ZX_OK) {
            ready = true;
        }
        if (status == ZX_ERR_BAD_STATE) {
            ready = false;
            // command returned CSW_FAILED. device is there but media is not ready.
            uint8_t request_sense_data[UMS_REQUEST_SENSE_TRANSFER_LENGTH];
            status = ums_request_sense(ums, lun, request_sense_data);
        }
        if (status != ZX_OK) {
            break;
        }

        if (ready && !dev->device_added) {
            // this will set ums_block_t.device_added if it succeeds
            status = ums_add_block_device(dev);
            if (status == ZX_OK) {
                dev->device_added = true;
            } else {
                zxlogf(ERROR, "UMS: device_add for block device failed %d\n", status);
            }
        } else if (!ready && dev->device_added) {
            device_remove(dev->zxdev);
            dev->device_added = false;
        }
    }

    return status;
}

static zx_protocol_device_t ums_device_proto = []() {
    zx_protocol_device_t ops;
    ops.version = DEVICE_OPS_VERSION;
    ops.unbind = ums_unbind;
    ops.release = ums_release;
    return ops;
}();

static int ums_worker_thread(void* arg) {
    ums_t* ums = (ums_t*)arg;
    zx_status_t status = ZX_OK;

    for (uint8_t lun = 0; lun <= ums->max_lun; lun++) {
        uint8_t inquiry_data[UMS_INQUIRY_TRANSFER_LENGTH];
        status = ums_inquiry(ums, lun, inquiry_data);
        if (status < 0) {
            zxlogf(ERROR, "ums_inquiry failed for lun %d status: %d\n", lun, status);
            device_remove(ums->zxdev);
            return status;
        }
        uint8_t rmb = inquiry_data[1] & 0x80; // Removable Media Bit
        if (rmb) {
            ums->block_devs[lun].flags |= BLOCK_FLAG_REMOVABLE;
        }
    }

    device_make_visible(ums->zxdev);

    bool wait = true;
    while (1) {
        if (wait) {
            status = sync_completion_wait(&ums->txn_completion, ZX_SEC(1));
            if (list_is_empty(&ums->queued_txns) && !ums->dead) {
                if (ums_check_luns_ready(ums) != ZX_OK) {
                    return status;
                }
                continue;
            }
            sync_completion_reset(&ums->txn_completion);
        }

        mtx_lock(&ums->txn_lock);
        if (ums->dead) {
            mtx_unlock(&ums->txn_lock);
            break;
        }
        ums_txn_t* txn = list_remove_head_type(&ums->queued_txns, ums_txn_t, node);
        if (txn == NULL) {
            mtx_unlock(&ums->txn_lock);
            wait = true;
            continue;
        } else {
            wait = false;
        }

        mtx_unlock(&ums->txn_lock);
        zxlogf(TRACE, "UMS PROCESS (%p)\n", &txn->op);

        ums_block_t* dev = txn->dev;

        zx_status_t status;
        switch (txn->op.command & BLOCK_OP_MASK) {
        case BLOCK_OP_READ:
            if ((status = ums_read(dev, txn)) != ZX_OK) {
                zxlogf(ERROR, "ums: read of %u @ %zu failed: %d\n",
                       txn->op.rw.length, txn->op.rw.offset_dev, status);
            }
            break;
        case BLOCK_OP_WRITE:
            if ((status = ums_write(dev, txn)) != ZX_OK) {
                zxlogf(ERROR, "ums: write of %u @ %zu failed: %d\n",
                       txn->op.rw.length, txn->op.rw.offset_dev, status);
            }
            break;
        case BLOCK_OP_FLUSH:
            if (ums->cache_enabled) {
                scsi_command10_t command;
                memset(&command, 0, sizeof(command));
                command.opcode = UMS_SYNCHRONIZE_CACHE;
                command.misc = 0;
                ums_send_cbw(ums, dev->lun, 0, USB_DIR_OUT, sizeof(command), &command);
                uint32_t residue;
                status = ums_read_csw(ums, &residue);
                if (status == ZX_OK && residue) {
                    zxlogf(ERROR, "unexpected residue in Write\n");
                    status = ZX_ERR_IO;
                }
            } else {
                status = ZX_OK;
            }
            break;
        default:
            status = ZX_ERR_INVALID_ARGS;
            break;
        }

        txn_complete(txn, status);
    }

    // complete any pending txns
    list_node_t txns = LIST_INITIAL_VALUE(txns);
    mtx_lock(&ums->txn_lock);
    list_move(&ums->queued_txns, &txns);
    mtx_unlock(&ums->txn_lock);

    ums_txn_t* txn;
    while ((txn = list_remove_head_type(&ums->queued_txns, ums_txn_t, node)) != NULL) {
        switch (txn->op.command & BLOCK_OP_MASK) {
        case BLOCK_OP_READ:
            zxlogf(ERROR, "ums: read of %u @ %zu discarded during unbind\n",
                   txn->op.rw.length, txn->op.rw.offset_dev);
            break;
        case BLOCK_OP_WRITE:
            zxlogf(ERROR, "ums: write of %u @ %zu discarded during unbind\n",
                   txn->op.rw.length, txn->op.rw.offset_dev);
            break;
        }
        txn_complete(txn, ZX_ERR_IO_NOT_PRESENT);
    }

    return ZX_OK;
}

static zx_status_t ums_bind(void* ctx, zx_device_t* device) {
    usb_protocol_t usb;
    zx_status_t status;
    device_add_args_t args = {};
    int ret;
    if (device_get_protocol(device, ZX_PROTOCOL_USB, &usb)) {
        return 0;
    }

    // find our endpoints
    usb::InterfaceList interfaces(&usb, true);
    if ((status = interfaces.check()) != ZX_OK) {
        return status;
    }
    auto interface = interfaces.begin();
    const usb_interface_descriptor_t* interface_descriptor = interface->descriptor();
    if (interface == interfaces.end()) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (interface_descriptor->bNumEndpoints < 2) {
        DEBUG_PRINT(("UMS:ums_bind wrong number of endpoints: %d\n",
                     interface_descriptor->bNumEndpoints));
        return ZX_ERR_NOT_SUPPORTED;
    }

    uint8_t interface_number = interface_descriptor->bInterfaceNumber;
    uint8_t bulk_in_addr = 0;
    uint8_t bulk_out_addr = 0;
    size_t bulk_in_max_packet = 0;
    size_t bulk_out_max_packet = 0;
    for (auto endpoint : *interface) {
        usb_endpoint_descriptor_t* endp = &endpoint;
        if (usb_ep_direction(endp) == USB_ENDPOINT_OUT) {
            if (usb_ep_type(endp) == USB_ENDPOINT_BULK) {
                bulk_out_addr = endp->bEndpointAddress;
                bulk_out_max_packet = usb_ep_max_packet(endp);
            }
        } else {
            if (usb_ep_type(endp) == USB_ENDPOINT_BULK) {
                bulk_in_addr = endp->bEndpointAddress;
                bulk_in_max_packet = usb_ep_max_packet(endp);
            }
        }
    }

    if (!bulk_in_addr || !bulk_out_addr) {
        DEBUG_PRINT(("UMS:ums_bind could not find endpoints\n"));
        return ZX_ERR_NOT_SUPPORTED;
    }

    uint8_t max_lun;
    size_t out_length;
    status = usb_control_in(&usb, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                            USB_REQ_GET_MAX_LUN, 0x00, 0x00, ZX_TIME_INFINITE,
                            &max_lun, sizeof(max_lun), &out_length);

    if (status == ZX_ERR_IO_REFUSED) {
        // Devices that do not support multiple LUNS may stall this command.
        // See USB Mass Storage Class Spec. 3.2 Get Max LUN.
        // Clear the stall.
        usb_reset_endpoint(&usb, 0);
        zxlogf(INFO, "Device does not support multiple LUNs\n");
        max_lun = 0;
    } else if (status != ZX_OK) {
        return status;
    } else if (out_length != sizeof(max_lun)) {
        return ZX_ERR_BAD_STATE;
    }

    ums_t* ums = static_cast<ums_t*>(calloc(1, sizeof(ums_t) + (max_lun + 1) * sizeof(ums_block_t)));
    if (!ums) {
        DEBUG_PRINT(("UMS:Not enough memory for ums_t\n"));
        return ZX_ERR_NO_MEMORY;
    }

    DEBUG_PRINT(("UMS:Max lun is: %u\n", max_lun));
    ums->max_lun = max_lun;

    for (uint8_t lun = 0; lun <= max_lun; lun++) {
        ums_block_t* dev = &ums->block_devs[lun];
        dev->lun = lun;
    }

    list_initialize(&ums->queued_txns);
    sync_completion_reset(&ums->txn_completion);
    mtx_init(&ums->txn_lock, mtx_plain);

    ums->usb_zxdev = device;
    memcpy(&ums->usb, &usb, sizeof(ums->usb));
    ums->bulk_in_addr = bulk_in_addr;
    ums->bulk_out_addr = bulk_out_addr;
    ums->bulk_in_max_packet = bulk_in_max_packet;
    ums->bulk_out_max_packet = bulk_out_max_packet;
    ums->interface_number = interface_number;

    size_t max_in = usb_get_max_transfer_size(&usb, bulk_in_addr);
    size_t max_out = usb_get_max_transfer_size(&usb, bulk_out_addr);
    ums->max_transfer = (max_in < max_out ? max_in : max_out);

    ums->parent_req_size = usb_get_request_size(&ums->usb);
    ZX_DEBUG_ASSERT(ums->parent_req_size != 0);

    status = usb_request_alloc(&ums->cbw_req, sizeof(ums_cbw_t), bulk_out_addr,
                               ums->parent_req_size);
    if (status != ZX_OK) {
        goto fail;
    }
    status = usb_request_alloc(&ums->data_req, PAGE_SIZE, bulk_in_addr,
                               ums->parent_req_size);
    if (status != ZX_OK) {
        goto fail;
    }
    status = usb_request_alloc(&ums->csw_req, sizeof(ums_csw_t), bulk_in_addr,
                               ums->parent_req_size);
    if (status != ZX_OK) {
        goto fail;
    }

    status = usb_request_alloc(&ums->data_transfer_req, 0, bulk_in_addr,
                               ums->parent_req_size);
    if (status != ZX_OK) {
        goto fail;
    }

    ums->tag_send = ums->tag_receive = 8;

    // Add root device, which will contain block devices for logical units

    args.version = DEVICE_ADD_ARGS_VERSION;
    args.name = "ums";
    args.ctx = ums;
    args.ops = &ums_device_proto;
    args.flags = DEVICE_ADD_NON_BINDABLE | DEVICE_ADD_INVISIBLE;

    status = device_add(ums->usb_zxdev, &args, &ums->zxdev);
    if (status != ZX_OK) {
        goto fail;
    }

    ret = thrd_create_with_name(&ums->worker_thread, ums_worker_thread, ums, "ums_worker_thread");
    if (ret != thrd_success) {
        device_remove(ums->zxdev);
        return ZX_ERR_NO_MEMORY;
    }

    return status;

fail:
    zxlogf(ERROR, "ums_bind failed: %d\n", status);
    ums_release(ums);
    return status;
}

static zx_driver_ops_t usb_mass_storage_driver_ops = []() {
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = ums_bind;
    return ops;
}();
// clang-format off
ZIRCON_DRIVER_BEGIN(usb_mass_storage, usb_mass_storage_driver_ops, "zircon", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_USB),
    BI_ABORT_IF(NE, BIND_USB_CLASS, USB_CLASS_MSC),
    BI_ABORT_IF(NE, BIND_USB_SUBCLASS, USB_SUBCLASS_MSC_SCSI),
    BI_MATCH_IF(EQ, BIND_USB_PROTOCOL, USB_PROTOCOL_MSC_BULK_ONLY),
ZIRCON_DRIVER_END(usb_mass_storage)
    // clang-format on