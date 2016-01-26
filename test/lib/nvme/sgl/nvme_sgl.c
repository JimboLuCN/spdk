/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <sys/uio.h> /* for struct iovec */

#include <pciaccess.h>

#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_lcore.h>

#include "spdk/nvme.h"
#include "spdk/pci.h"

struct rte_mempool *request_mempool;

#define MAX_DEVS 64

#define MAX_IOVS 128

#define DATA_PATTERN 0x5A

#define BASE_LBA_START 0x100000

struct dev {
	struct pci_device			*pci_dev;
	struct nvme_controller 			*ctrlr;
	char 					name[100];
};

static struct dev devs[MAX_DEVS];
static int num_devs = 0;

#define foreach_dev(iter) \
	for (iter = devs; iter - devs < num_devs; iter++)

static int io_complete_flag = 0;

struct io_request {
	int current_iov_index;
	uint32_t current_iov_bytes_left;
	struct iovec iovs[MAX_IOVS];
	int nseg;
};

static void nvme_request_reset_sgl(void *cb_arg, uint32_t sgl_offset)
{
	int i;
	uint32_t offset = 0;
	struct iovec *iov;
	struct io_request *req = (struct io_request *)cb_arg;

	for (i = 0; i < req->nseg; i++) {
		iov = &req->iovs[i];
		offset += iov->iov_len;
		if (offset > sgl_offset)
			break;
	}
	req->current_iov_index = i;
	req->current_iov_bytes_left = offset - sgl_offset;
	return;
}

static int nvme_request_next_sge(void *cb_arg, uint64_t *address, uint32_t *length)
{
	struct io_request *req = (struct io_request *)cb_arg;
	struct iovec *iov;

	if (req->current_iov_index >= req->nseg) {
		*length = 0;
		*address = 0;
		return 0;
	}

	iov = &req->iovs[req->current_iov_index];

	if (req->current_iov_bytes_left) {
		*address = rte_malloc_virt2phy(iov->iov_base) + iov->iov_len - req->current_iov_bytes_left;
		*length = req->current_iov_bytes_left;
		req->current_iov_bytes_left = 0;
	} else {
		*address = rte_malloc_virt2phy(iov->iov_base);
		*length = iov->iov_len;
	}

	req->current_iov_index++;

	return 0;
}

static void
io_complete(void *ctx, const struct nvme_completion *cpl)
{
	if (nvme_completion_is_error(cpl))
		io_complete_flag = 2;
	else
		io_complete_flag = 1;
}

static uint32_t build_io_request_1(struct io_request *req)
{
	int i, found = 0;
	uint8_t *buf;
	uint64_t v_addr;
	uint32_t len = 0;

	req->nseg = 3;

	/* 2KB for 1st sge, make sure the iov address start at 0x800 boundary,
	 *  and end with 0x1000 boundary */
	for (i = 0; i < 8; i++) {
		buf = rte_zmalloc(NULL, 0x800, 0x800);
		v_addr = (uint64_t)buf;
		if (v_addr & 0x800ULL) {
			found = 1;
			break;
		}
	}
	if (!found)
		return 0;
	req->iovs[0].iov_base = rte_zmalloc(NULL, 0x800, 0x800);
	req->iovs[0].iov_len = 0x800;

	/* 4KB for 2th sge */
	req->iovs[1].iov_base = rte_zmalloc(NULL, 0x1000, 0x1000);
	req->iovs[1].iov_len = 0x1000;

	/* 12KB for 3th sge */
	req->iovs[2].iov_base = rte_zmalloc(NULL, 0x3000, 0x1000);
	req->iovs[2].iov_len = 0x3000;

	for (i = 0; i < req->nseg; i++)
		len += req->iovs[i].iov_len;

	return len;
}

static uint32_t build_io_request_2(struct io_request *req)
{
	int i;
	uint32_t len = 0;

	req->nseg = 32;

	/* 4KB for 1st sge */
	req->iovs[0].iov_base = rte_zmalloc(NULL, 0x1000, 0x1000);
	req->iovs[0].iov_len = 0x1000;

	/* 8KB for the rest 31 sge */
	for (i = 1; i < req->nseg; i++) {
		req->iovs[i].iov_base = rte_zmalloc(NULL, 0x2000, 0x1000);
		req->iovs[i].iov_len = 0x2000;
	}

	for (i = 0; i < req->nseg; i++)
		len += req->iovs[i].iov_len;

	return len;
}

static uint32_t build_io_request_3(struct io_request *req)
{
	int i;
	uint32_t len = 0;

	req->nseg = 1;

	/* 8KB for 1st sge */
	req->iovs[0].iov_base = rte_zmalloc(NULL, 0x2000, 0x1000);
	req->iovs[0].iov_len = 0x2000;

	for (i = 0; i < req->nseg; i++)
		len += req->iovs[i].iov_len;

	return len;
}

static uint32_t build_io_request_4(struct io_request *req)
{
	int i;
	uint32_t len = 0;

	req->nseg = 2;

	/* 4KB for 1st sge */
	req->iovs[0].iov_base = rte_zmalloc(NULL, 0x1000, 0x1000);
	req->iovs[0].iov_len = 0x1000;

	/* 4KB for 2st sge */
	req->iovs[1].iov_base = rte_zmalloc(NULL, 0x1000, 0x1000);
	req->iovs[1].iov_len = 0x1000;

	for (i = 0; i < req->nseg; i++)
		len += req->iovs[i].iov_len;

	return len;
}

static uint32_t build_io_request_5(struct io_request *req)
{
	int i;
	uint32_t len = 0;

	req->nseg = 1;

	/* 256KB for 1st sge */
	req->iovs[0].iov_base = rte_zmalloc(NULL, 0x40000, 0x1000);
	req->iovs[0].iov_len = 0x40000;

	for (i = 0; i < req->nseg; i++)
		len += req->iovs[i].iov_len;

	return len;
}

static uint32_t build_io_request_6(struct io_request *req)
{
	int i;
	uint32_t len = 0;

	req->nseg = 1;

	/* 512B for 1st sge */
	req->iovs[0].iov_base = rte_zmalloc(NULL, 0x200, 0x200);
	req->iovs[0].iov_len = 0x200;

	for (i = 0; i < req->nseg; i++)
		len += req->iovs[i].iov_len;

	return len;
}

typedef uint32_t (*nvme_build_io_req_fn_t)(struct io_request *req);

static int
writev_readv_tests(struct dev *dev, nvme_build_io_req_fn_t build_io_fn)
{
	int rc = 0;
	uint32_t len, lba_count;
	uint32_t i, j, nseg;
	char *buf;

	struct io_request *req;
	struct nvme_namespace *ns;
	const struct nvme_namespace_data *nsdata;

	ns = nvme_ctrlr_get_ns(dev->ctrlr, 1);
	nsdata = nvme_ns_get_data(ns);
	if (!ns || !nsdata || !nvme_ns_get_sector_size(ns))
		return -1;

	req = rte_zmalloc(NULL, sizeof(*req), 0);
	if (!req)
		return -1;

	/* IO parameters setting */
	len = build_io_fn(req);
	if (!len)
		return 0;

	lba_count = len / nvme_ns_get_sector_size(ns);
	if (BASE_LBA_START + lba_count > (uint32_t)nsdata->nsze) {
		rte_free(req);
		return -1;
	}

	nseg = req->nseg;
	for (i = 0; i < nseg; i++) {
		memset(req->iovs[i].iov_base, DATA_PATTERN, req->iovs[i].iov_len);
	}

	rc = nvme_ns_cmd_writev(ns, BASE_LBA_START, lba_count,
				io_complete, req, 0,
				nvme_request_reset_sgl,
				nvme_request_next_sge);

	if (rc != 0) {
		fprintf(stderr, "Writev Failed\n");
		rte_free(req);
		return -1;
	}

	io_complete_flag = 0;

	while (!io_complete_flag)
		nvme_ctrlr_process_io_completions(dev->ctrlr, 1);

	if (io_complete_flag != 1) {
		fprintf(stderr, "%s Writev Failed\n", dev->name);
		rte_free(req);
		return -1;
	}

	/* reset completion flag */
	io_complete_flag = 0;

	for (i = 0; i < nseg; i++) {
		memset(req->iovs[i].iov_base, 0, req->iovs[i].iov_len);
	}

	rc = nvme_ns_cmd_readv(ns, BASE_LBA_START, lba_count,
			       io_complete, req, 0,
			       nvme_request_reset_sgl,
			       nvme_request_next_sge);

	if (rc != 0) {
		fprintf(stderr, "Readv Failed\n");
		rte_free(req);
		return -1;
	}

	while (!io_complete_flag)
		nvme_ctrlr_process_io_completions(dev->ctrlr, 1);

	if (io_complete_flag != 1) {
		fprintf(stderr, "%s Readv Failed\n", dev->name);
		rte_free(req);
		return -1;
	}

	for (i = 0; i < nseg; i++) {
		buf = (char *)req->iovs[i].iov_base;
		for (j = 0; j < req->iovs[i].iov_len; j++) {
			if (buf[j] != DATA_PATTERN) {
				fprintf(stderr, "Write/Read Sucess, But %s Memcmp Failed\n", dev->name);
				rte_free(req);
				return -1;
			}
		}
	}

	fprintf(stdout, "%s %s Test Passed\n", dev->name, __func__);
	rte_free(req);
	return rc;
}

static const char *ealargs[] = {
	"nvme_sgl",
	"-c 0x1",
	"-n 4",
};

int main(int argc, char **argv)
{
	struct pci_device_iterator	*pci_dev_iter;
	struct pci_device		*pci_dev;
	struct dev			*iter;
	struct pci_id_match		match;
	int				rc, i;

	printf("NVMe Readv/Writev Request test\n");

	rc = rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]),
			  (char **)(void *)(uintptr_t)ealargs);

	if (rc < 0) {
		fprintf(stderr, "could not initialize dpdk\n");
		exit(1);
	}

	request_mempool = rte_mempool_create("nvme_request", 8192,
					     nvme_request_size(), 128, 0,
					     NULL, NULL, NULL, NULL,
					     SOCKET_ID_ANY, 0);

	if (request_mempool == NULL) {
		fprintf(stderr, "could not initialize request mempool\n");
		exit(1);
	}

	pci_system_init();

	match.vendor_id =	PCI_MATCH_ANY;
	match.subvendor_id =	PCI_MATCH_ANY;
	match.subdevice_id =	PCI_MATCH_ANY;
	match.device_id =	PCI_MATCH_ANY;
	match.device_class =	NVME_CLASS_CODE;
	match.device_class_mask = 0xFFFFFF;

	pci_dev_iter = pci_id_match_iterator_create(&match);

	rc = 0;
	while ((pci_dev = pci_device_next(pci_dev_iter))) {
		struct dev *dev;

		if (pci_device_has_non_uio_driver(pci_dev)) {
			fprintf(stderr, "non-null kernel driver attached to nvme\n");
			fprintf(stderr, " controller at pci bdf %d:%d:%d\n",
				pci_dev->bus, pci_dev->dev, pci_dev->func);
			fprintf(stderr, " skipping...\n");
			continue;
		}

		pci_device_probe(pci_dev);

		/* add to dev list */
		dev = &devs[num_devs++];

		dev->pci_dev = pci_dev;

		snprintf(dev->name, sizeof(dev->name), "%04X:%02X:%02X.%02X",
			 pci_dev->domain, pci_dev->bus, pci_dev->dev, pci_dev->func);

		printf("%s: attaching NVMe driver...\n", dev->name);

		dev->ctrlr = nvme_attach(pci_dev);
		if (dev->ctrlr == NULL) {
			fprintf(stderr, "failed to attach to NVMe controller %s\n", dev->name);
			rc = 1;
			continue; /* TODO: just abort */
		}
	}
	pci_iterator_destroy(pci_dev_iter);

	if (num_devs) {
		rc = nvme_register_io_thread();
		if (rc != 0)
			return rc;
	}

	foreach_dev(iter) {
		if (writev_readv_tests(iter, build_io_request_1)
		    || writev_readv_tests(iter, build_io_request_2)
		    || writev_readv_tests(iter, build_io_request_3)
		    || writev_readv_tests(iter, build_io_request_4)
		    || writev_readv_tests(iter, build_io_request_5)
		    || writev_readv_tests(iter, build_io_request_6)) {
			printf("%s: failed sgl tests\n", iter->name);
		}
	}

	printf("Cleaning up...\n");

	for (i = 0; i < num_devs; i++) {
		struct dev *dev = &devs[i];

		nvme_detach(dev->ctrlr);
	}

	if (num_devs)
		nvme_unregister_io_thread();

	return rc;
}
