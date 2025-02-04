/*
 * Copyright Runtime.io 2018. All rights reserved.
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/net/buf.h>
#include <zephyr/mgmt/mcumgr/buf.h>
#include "mgmt/mgmt.h"
#include "smp/smp.h"
#include <zephyr/mgmt/mcumgr/smp.h>
#include "smp_reassembly.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(mcumgr_smp, CONFIG_MCUMGR_SMP_LOG_LEVEL);

/* To be able to unit test some callers some functions need to be
 * demoted to allow overriding them.
 */
#ifdef CONFIG_ZTEST
#define WEAK __weak
#else
#define WEAK
#endif

K_THREAD_STACK_DEFINE(smp_work_queue_stack, CONFIG_MCUMGR_SMP_WORKQUEUE_STACK_SIZE);

static struct k_work_q smp_work_queue;

static const struct k_work_queue_config smp_work_queue_config = {
	.name = "mcumgr smp"
};

/**
 * @brief Allocates a response buffer.
 *
 * If a source buf is provided, its user data is copied into the new buffer.
 *
 * @param req		An optional source buffer to copy user data from.
 * @param arg		The streamer providing the callback.
 *
 * @return	Newly-allocated buffer on success
 *		NULL on failure.
 */
void *smp_alloc_rsp(const void *req, void *arg)
{
	const struct net_buf *req_nb;
	struct net_buf *rsp_nb;
	struct smp_transport *smpt = arg;

	req_nb = req;

	rsp_nb = mcumgr_buf_alloc();
	if (rsp_nb == NULL) {
		return NULL;
	}

	if (smpt->ud_copy) {
		smpt->ud_copy(rsp_nb, req_nb);
	} else {
		memcpy(net_buf_user_data(rsp_nb),
		       net_buf_user_data((void *)req_nb),
		       req_nb->user_data_size);
	}

	return rsp_nb;
}

void smp_free_buf(void *buf, void *arg)
{
	struct smp_transport *smpt = arg;

	if (!buf) {
		return;
	}

	if (smpt->ud_free) {
		smpt->ud_free(net_buf_user_data((struct net_buf *)buf));
	}

	mcumgr_buf_free(buf);
}

/**
 * Processes a single SMP packet and sends the corresponding response(s).
 */
static int
smp_process_packet(struct smp_transport *smpt, struct net_buf *nb)
{
	struct cbor_nb_reader reader;
	struct cbor_nb_writer writer;
	struct smp_streamer streamer;
	int rc;

	streamer = (struct smp_streamer) {
		.reader = &reader,
		.writer = &writer,
		.smpt = smpt,
	};

	rc = smp_process_request_packet(&streamer, nb);
	return rc;
}

/**
 * Processes all received SNP request packets.
 */
static void
smp_handle_reqs(struct k_work *work)
{
	struct smp_transport *smpt;
	struct net_buf *nb;

	smpt = (void *)work;

	while ((nb = net_buf_get(&smpt->fifo, K_NO_WAIT)) != NULL) {
		smp_process_packet(smpt, nb);
	}
}

void
smp_transport_init(struct smp_transport *smpt,
		   smp_transport_out_fn output_func,
		   smp_transport_get_mtu_fn get_mtu_func,
		   smp_transport_ud_copy_fn ud_copy_func,
		   smp_transport_ud_free_fn ud_free_func)
{
	*smpt = (struct smp_transport) {
		.output = output_func,
		.get_mtu = get_mtu_func,
		.ud_copy = ud_copy_func,
		.ud_free = ud_free_func,
	};

#ifdef CONFIG_MCUMGR_SMP_REASSEMBLY
	smp_reassembly_init(smpt);
#endif

	k_work_init(&smpt->work, smp_handle_reqs);
	k_fifo_init(&smpt->fifo);
}

/**
 * @brief Enqueues an incoming SMP request packet for processing.
 *
 * This function always consumes the supplied net_buf.
 *
 * @param smpt                  The transport to use to send the corresponding
 *                                  response(s).
 * @param nb                    The request packet to process.
 */
WEAK void
smp_rx_req(struct smp_transport *smpt, struct net_buf *nb)
{
	net_buf_put(&smpt->fifo, nb);
	k_work_submit_to_queue(&smp_work_queue, &smpt->work);
}

static int smp_init(const struct device *dev)
{
	k_work_queue_init(&smp_work_queue);

	k_work_queue_start(&smp_work_queue, smp_work_queue_stack,
			   K_THREAD_STACK_SIZEOF(smp_work_queue_stack),
			   CONFIG_MCUMGR_SMP_WORKQUEUE_THREAD_PRIO, &smp_work_queue_config);

	return 0;
}

SYS_INIT(smp_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
