/*
 * (C) Copyright 2020 Hewlett Packard Enterprise Development LP
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#if HAVE_CONFIG_H
#include <config.h>
#endif

#include "ofi_hmem.h"
#include "ofi.h"
#include "ofi_iov.h"

struct ofi_hmem_ops {
	bool initialized;
	int (*init)(void);
	int (*cleanup)(void);
	int (*copy_to_hmem)(void *dest, const void *src, size_t size);
	int (*copy_from_hmem)(void *dest, const void *src, size_t size);
	bool (*is_addr_valid)(const void *addr);
};

static struct ofi_hmem_ops hmem_ops[] = {
	[FI_HMEM_SYSTEM] = {
		.initialized = false,
		.init = ofi_hmem_init_noop,
		.cleanup = ofi_hmem_cleanup_noop,
		.copy_to_hmem = ofi_memcpy,
		.copy_from_hmem = ofi_memcpy,
	},
	[FI_HMEM_CUDA] = {
		.initialized = false,
		.init = cuda_hmem_init,
		.cleanup = cuda_hmem_cleanup,
		.copy_to_hmem = cuda_copy_to_dev,
		.copy_from_hmem = cuda_copy_from_dev,
		.is_addr_valid = cuda_is_addr_valid,
	},
};

static inline int ofi_copy_to_hmem(void *dest, const void *src, size_t size,
				   enum fi_hmem_iface iface)
{
	return hmem_ops[iface].copy_to_hmem(dest, src, size);
}

static inline int ofi_copy_from_hmem(void *dest, const void *src, size_t size,
				     enum fi_hmem_iface iface)
{
	return hmem_ops[iface].copy_from_hmem(dest, src, size);
}

static ssize_t ofi_copy_hmem_iov_buf(const struct iovec *hmem_iov,
				     enum fi_hmem_iface hmem_iface,
				     size_t hmem_iov_count,
				     uint64_t hmem_iov_offset, void *buf,
				     size_t size, int dir)
{
	uint64_t done = 0, len;
	char *hmem_buf;
	size_t i;
	int ret;

	for (i = 0; i < hmem_iov_count && size; i++) {
		len = hmem_iov[i].iov_len;

		if (hmem_iov_offset > len) {
			hmem_iov_offset -= len;
			continue;
		}

		hmem_buf = (char *)hmem_iov[i].iov_base + hmem_iov_offset;
		len -= hmem_iov_offset;

		len = MIN(len, size);
		if (dir == OFI_COPY_BUF_TO_IOV)
			ret = ofi_copy_to_hmem(hmem_buf, (char *)buf + done,
					       len, hmem_iface);
		else
			ret = ofi_copy_from_hmem((char *)buf + done, hmem_buf,
						 len, hmem_iface);

		if (ret)
			return ret;

		hmem_iov_offset = 0;
		size -= len;
		done += len;
	}
	return done;
}

ssize_t ofi_copy_from_hmem_iov(void *dest, size_t size,
				const struct iovec *hmem_iov,
				enum fi_hmem_iface hmem_iface,
				size_t hmem_iov_count,
				uint64_t hmem_iov_offset)
{
	return ofi_copy_hmem_iov_buf(hmem_iov, hmem_iface, hmem_iov_count,
				     hmem_iov_offset, dest, size,
				     OFI_COPY_IOV_TO_BUF);
}

ssize_t ofi_copy_to_hmem_iov(const struct iovec *hmem_iov,
			      enum fi_hmem_iface hmem_iface,
			      size_t hmem_iov_count, uint64_t hmem_iov_offset,
			      void *src, size_t size)
{
	return ofi_copy_hmem_iov_buf(hmem_iov, hmem_iface, hmem_iov_count,
				     hmem_iov_offset, src, size,
				     OFI_COPY_BUF_TO_IOV);
}

void ofi_hmem_init(void)
{
	enum fi_hmem_iface iface;
	int ret;

	for (iface = 0; iface < ARRAY_SIZE(hmem_ops); iface++) {
		ret = hmem_ops[iface].init();
		if (ret != FI_SUCCESS)
			FI_WARN(&core_prov, FI_LOG_CORE,
				"Failed to initialize hmem iface %s",
				fi_tostr(&iface, FI_TYPE_HMEM_IFACE));
		else
			hmem_ops[iface].initialized = true;
	}
}

void ofi_hmem_cleanup(void)
{
	enum fi_hmem_iface iface;

	for (iface = 0; iface < ARRAY_SIZE(hmem_ops); iface++) {
		if (hmem_ops[iface].initialized)
			hmem_ops[iface].cleanup();
	}
}

enum fi_hmem_iface ofi_get_hmem_iface(const void *addr)
{
	enum fi_hmem_iface iface;

	/* Since a is_addr_valid function is not implemented for FI_HMEM_SYSTEM,
	 * HMEM iface is skipped. In addition, if no other HMEM ifaces claim the
	 * address as valid, it is assumed the address is FI_HMEM_SYSTEM.
	 */
	for (iface = ARRAY_SIZE(hmem_ops) - 1; iface > FI_HMEM_SYSTEM;
	     iface--) {
		if (hmem_ops[iface].initialized &&
		    hmem_ops[iface].is_addr_valid(addr))
			return iface;
	}

	return FI_HMEM_SYSTEM;
}
