/* The MIT License (MIT)
 *
 * Copyright (c) 2014 Pavel Krajcevski
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "howler.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int is_howler(libusb_device *device) {
  struct libusb_device_descriptor desc;
  libusb_get_device_descriptor(device, &desc);

  if(desc.idVendor != HOWLER_VENDOR_ID) {
    return 0;
  }

  int has_device_id = 0;
  unsigned int i = 0;
  for(; i < MAX_HOWLER_DEVICE_IDS; i++) {
    if(desc.idProduct == HOWLER_DEVICE_ID[i]) {
      has_device_id = 1;
    }
  }

  if(!has_device_id) {
    return 0;
  }
  return 1;
}

int howler_init(howler_context **ctx_ptr) {
  // Convenience variables
  unsigned int i;

  // First, check if the pointer is valid...
  int error = HOWLER_SUCCESS;
  if(!ctx_ptr) { return HOWLER_ERROR_INVALID_PTR; }

  // Allocate a USB context so that we can talk to the devices...
  libusb_context *usb_ctx;
  int ret = libusb_init(&usb_ctx);
  if(ret) {
    error = HOWLER_ERROR_LIBUSB_CONTEXT_ERROR;
    goto err;
  }

  libusb_set_debug(usb_ctx, 3);

  // Get a list of all available devices.
  libusb_device **device_list;
  ssize_t nDevices = libusb_get_device_list(usb_ctx, &device_list);
  if(nDevices < 0) {
    error = HOWLER_ERROR_LIBUSB_DEVICE_LIST_ERROR;
    goto err_after_libusb_context;
  }

  // Find which ones are howlers...
  size_t nHowlers = 0;
  for(i = 0; i < nDevices; i++) {
    if(is_howler(device_list[i])) {
      nHowlers++;
    }
  }

  // Short circuit if no howlers...
  if(!nHowlers) {
    howler_context *result = malloc(sizeof(howler_context));
    result->usb_ctx = usb_ctx;
    result->nDevices = nHowlers;
    result->devices = NULL;
    *ctx_ptr = result;
  }

  howler_device *howlers = malloc(nHowlers * sizeof(howler_device));
  unsigned int howler_idx = 0;
  for(i = 0; i < nDevices; i++) {
    if(!is_howler(device_list[i]))
      continue;

    libusb_device *d = device_list[i];
    libusb_device_handle *h = NULL;
    int err = libusb_open(d, &h);
    if(err < 0) {
      if(err == LIBUSB_ERROR_ACCESS) {
        fprintf(stderr,
                "WARNING: Unable to open interface to Howler device: "
                "Permission Denied\n");
      }

      // Just skip this device...
      nHowlers--;
      continue;
    }
    
    assert(howler_idx < nHowlers);
    howler_device *howler = &(howlers[howler_idx]);
    howler->usb_device = d;
    howler->usb_handle = h;
    memset(howler->led_banks, 0, 6*sizeof(howler_led_bank));
    howler_idx++;
  }

  assert(howler_idx == nHowlers);

  // Everything is OK...
  howler_context *result = malloc(sizeof(howler_context));
  result->usb_ctx = usb_ctx;
  result->nDevices = nHowlers;
  result->devices = howlers;
  *ctx_ptr = result;

  // Cleanup
  libusb_free_device_list(device_list, 1);
  return HOWLER_SUCCESS;

  // Errors...
 err_after_libusb_context:
  libusb_exit(usb_ctx);
 err:
  *ctx_ptr = NULL;
  return error;
}

void howler_destroy(howler_context *ctx) {
  if(!ctx) { return; }
  unsigned int i = 0;
  for(; i < ctx->nDevices; i++) {
    libusb_close(ctx->devices[i].usb_handle);
  }
  free(ctx->devices);

  libusb_exit(ctx->usb_ctx);
  free(ctx);
}

int howler_sendrcv(howler_device *dev,
                   unsigned char *cmd_buf,
                   unsigned char *output) {
  // Claim the interface. Make sure the kernel driver is not attached 
  // first, however.
  libusb_device_handle *handle = dev->usb_handle;
  int err = libusb_kernel_driver_active(handle, 0);
  if(err < 0) {
    return -1;
  }

  int kernel_driver_attached = 0;
  if(err) {
    kernel_driver_attached = 1;
    err = libusb_detach_kernel_driver(handle, 0);
    if(err < 0) {
      return -1;
    }
  }

  err = libusb_claim_interface(handle, 0);
  if(err < 0) {
    goto detach;
  }

  // Write the command
  int transferred = 0;
  err = libusb_interrupt_transfer(handle, 0x02, cmd_buf, 24, &transferred, 0);
  if(err < 0) {
    goto error;
  }

  // Read the following command
  if(output) {
    unsigned char endpoint = 0x81;
    while(err = libusb_interrupt_transfer(handle, endpoint, output, 24, &transferred, 0)) {
      if(0x81 == endpoint) {
        endpoint = 0x83;
      } else if(0x83 == endpoint) {
        endpoint = 0x86;
      } else {
        break;
      }
    }
  }

 error:
  libusb_release_interface(handle, 0);
 detach:
  if(kernel_driver_attached) {
    libusb_attach_kernel_driver(handle, 0);
  }
  return err;
}