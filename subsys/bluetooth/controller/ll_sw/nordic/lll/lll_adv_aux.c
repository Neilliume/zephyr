/*
 * Copyright (c) 2018-2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>

#include <toolchain.h>
#include <zephyr/types.h>
#include <sys/byteorder.h>

#include "hal/ccm.h"
#include "hal/radio.h"
#include "hal/ticker.h"

#include "util/util.h"
#include "util/memq.h"

#include "pdu.h"

#include "lll.h"
#include "lll_vendor.h"
#include "lll_clock.h"
#include "lll_chan.h"
#include "lll_adv.h"
#include "lll_adv_aux.h"

#include "lll_internal.h"
#include "lll_adv_internal.h"

#define BT_DBG_ENABLED IS_ENABLED(CONFIG_BT_DEBUG_HCI_DRIVER)
#define LOG_MODULE_NAME bt_ctlr_lll_adv_aux
#include "common/log.h"
#include <soc.h>
#include "hal/debug.h"

static int init_reset(void);
static int prepare_cb(struct lll_prepare_param *prepare_param);
static void abort_cb(struct lll_prepare_param *prepare_param, void *param);

int lll_adv_aux_init(void)
{
	int err;

	err = init_reset();
	if (err) {
		return err;
	}

	return 0;
}

int lll_adv_aux_reset(void)
{
	int err;

	err = init_reset();
	if (err) {
		return err;
	}

	return 0;
}

void lll_adv_aux_prepare(void *param)
{
	struct lll_prepare_param *p = param;
	int err;

	err = lll_hfclock_on();
	LL_ASSERT(!err || err == -EINPROGRESS);

	err = lll_prepare(lll_is_abort_cb, abort_cb, prepare_cb, 0, p);
	LL_ASSERT(!err || err == -EINPROGRESS);
}

void lll_adv_aux_offset_fill(uint32_t ticks_offset, uint32_t start_us,
			     struct pdu_adv *pdu)
{
	struct pdu_adv_com_ext_adv *p;
	struct ext_adv_aux_ptr *aux;
	struct ext_adv_hdr *h;
	uint8_t *ptr;

	p = (void *)&pdu->adv_ext_ind;
	h = (void *)p->ext_hdr_adi_adv_data;
	ptr = (uint8_t *)h + sizeof(*h);

	if (h->adv_addr) {
		ptr += BDADDR_SIZE;
	}

	if (h->adi) {
		ptr += sizeof(struct ext_adv_adi);
	}

	aux = (void *)ptr;
	aux->offs = (HAL_TICKER_TICKS_TO_US(ticks_offset) - start_us) / 30;
	if (aux->offs_units) {
		aux->offs /= 10;
	}
}

void lll_adv_aux_pback_prepare(void *param)
{
}

static int init_reset(void)
{
	return 0;
}

static int prepare_cb(struct lll_prepare_param *prepare_param)
{
	struct lll_adv_aux *lll = prepare_param->param;
	uint32_t aa = sys_cpu_to_le32(PDU_AC_ACCESS_ADDR);
	uint32_t ticks_at_event, ticks_at_start;
	struct pdu_adv_com_ext_adv *p;
	struct ext_adv_aux_ptr *aux;
	struct pdu_adv *pri, *sec;
	struct lll_adv *lll_adv;
	struct ext_adv_hdr *h;
	struct evt_hdr *evt;
	uint32_t remainder;
	uint32_t start_us;
	uint8_t phy_s;
	uint8_t *ptr;
	uint8_t upd;

	DEBUG_RADIO_START_A(1);

	/* FIXME: get latest only when primary PDU without Aux PDUs */
	sec = lll_adv_aux_data_latest_get(lll, &upd);

	/* Get reference to primary PDU */
	lll_adv = lll->adv;
	pri = lll_adv_data_curr_get(lll_adv);
	LL_ASSERT(pri->type == PDU_ADV_TYPE_EXT_IND);

	/* Get reference to extended header */
	p = (void *)&pri->adv_ext_ind;
	h = (void *)p->ext_hdr_adi_adv_data;
	ptr = (uint8_t *)h + sizeof(*h);

	/* traverse through adv_addr, if present */
	if (h->adv_addr) {
		ptr += BDADDR_SIZE;
	}

	/* traverse through adi, if present */
	if (h->adi) {
		ptr += sizeof(struct ext_adv_adi);
	}

	aux = (void *)ptr;

	/* Abort if no aux_ptr filled */
	if (!h->aux_ptr || !aux->offs) {
		radio_isr_set(lll_isr_abort, lll);
		radio_disable();
	}

#if !defined(BT_CTLR_ADV_EXT_PBACK)
	/* Set up Radio H/W */
	radio_reset();
#endif  /* !BT_CTLR_ADV_EXT_PBACK */

#if defined(CONFIG_BT_CTLR_TX_PWR_DYNAMIC_CONTROL)
	radio_tx_power_set(lll->tx_pwr_lvl);
#else
	radio_tx_power_set(RADIO_TXP_DEFAULT);
#endif /* CONFIG_BT_CTLR_TX_PWR_DYNAMIC_CONTROL */

	phy_s = lll_adv->phy_s;

	/* TODO: if coded we use S8? */
	radio_phy_set(phy_s, 1);
	radio_pkt_configure(8, PDU_AC_PAYLOAD_SIZE_MAX, (phy_s << 1));

#if !defined(BT_CTLR_ADV_EXT_PBACK)
	/* Access address and CRC */
	radio_aa_set((uint8_t *)&aa);
	radio_crc_configure(((0x5bUL) | ((0x06UL) << 8) | ((0x00UL) << 16)),
			    0x555555);
#endif  /* !BT_CTLR_ADV_EXT_PBACK */

	/* Use channel idx in aux_ptr */
	lll_chan_set(aux->chan_idx);

	/* Set the Radio Tx Packet */
	radio_pkt_tx_set(sec);

	/* TODO: Based on adv_mode switch to Rx, if needed */
	radio_isr_set(lll_isr_done, lll);
	radio_switch_complete_and_disable();

#if defined(BT_CTLR_ADV_EXT_PBACK)
	start_us = 1000;
	radio_tmr_start_us(1, start_us);
#else /* !BT_CTLR_ADV_EXT_PBACK */

	ticks_at_event = prepare_param->ticks_at_expire;
	evt = HDR_LLL2EVT(lll);
	ticks_at_event += lll_evt_offset_get(evt);

	ticks_at_start = ticks_at_event;
	ticks_at_start += HAL_TICKER_US_TO_TICKS(EVENT_OVERHEAD_START_US);

	remainder = prepare_param->remainder;
	start_us = radio_tmr_start(1, ticks_at_start, remainder);
#endif /* !BT_CTLR_ADV_EXT_PBACK */

	/* capture end of Tx-ed PDU, used to calculate HCTO. */
	radio_tmr_end_capture();

#if defined(CONFIG_BT_CTLR_GPIO_PA_PIN)
	radio_gpio_pa_setup();
	radio_gpio_pa_lna_enable(start_us + radio_tx_ready_delay_get(phy_s, 1) -
				 CONFIG_BT_CTLR_GPIO_PA_OFFSET);
#else /* !CONFIG_BT_CTLR_GPIO_PA_PIN */
	ARG_UNUSED(start_us);
#endif /* !CONFIG_BT_CTLR_GPIO_PA_PIN */

#if defined(CONFIG_BT_CTLR_XTAL_ADVANCED) && \
	(EVENT_OVERHEAD_PREEMPT_US <= EVENT_OVERHEAD_PREEMPT_MIN_US)
	/* check if preempt to start has changed */
	if (lll_preempt_calc(evt, (TICKER_ID_ADV_AUX_BASE +
				   ull_adv_aux_lll_handle_get(lll)),
			     ticks_at_event)) {
		radio_isr_set(lll_isr_abort, lll);
		radio_disable();
	} else
#endif /* CONFIG_BT_CTLR_XTAL_ADVANCED */
	{
		uint32_t ret;

		ret = lll_prepare_done(lll);
		LL_ASSERT(!ret);
	}

	DEBUG_RADIO_START_A(1);

	return 0;
}

static void abort_cb(struct lll_prepare_param *prepare_param, void *param)
{
	int err;

	/* NOTE: This is not a prepare being cancelled */
	if (!prepare_param) {
		/* Perform event abort here.
		 * After event has been cleanly aborted, clean up resources
		 * and dispatch event done.
		 */
		radio_isr_set(lll_isr_done, param);
		radio_disable();
		return;
	}

	/* NOTE: Else clean the top half preparations of the aborted event
	 * currently in preparation pipeline.
	 */
	err = lll_hfclock_off();
	LL_ASSERT(!err || err == -EBUSY);

	lll_done(param);
}
