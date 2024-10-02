// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018-2021, The Linux Foundation. All rights reserved.
 *
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/mhi.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include "internal.h"

int __must_check mhi_read_reg(struct mhi_controller *mhi_cntrl,
			      void __iomem *base, u32 offset, u32 *out)
{
	return mhi_cntrl->read_reg(mhi_cntrl, base + offset, out);
}

int __must_check mhi_read_reg_field(struct mhi_controller *mhi_cntrl,
				    void __iomem *base, u32 offset,
				    u32 mask, u32 shift, u32 *out)
{
	u32 tmp;
	int ret;

	ret = mhi_read_reg(mhi_cntrl, base, offset, &tmp);
	if (ret)
		return ret;

	*out = (tmp & mask) >> shift;

	return 0;
}

int __must_check mhi_poll_reg_field(struct mhi_controller *mhi_cntrl,
				    void __iomem *base, u32 offset,
				    u32 mask, u32 shift, u32 val, u32 delayus)
{
	int ret;
	u32 out, retry = (mhi_cntrl->timeout_ms * 1000) / delayus;

	while (retry--) {
		ret = mhi_read_reg_field(mhi_cntrl, base, offset, mask, shift,
					 &out);
		if (ret)
			return ret;

		if (out == val)
			return 0;

		fsleep(delayus);
	}

	return -ETIMEDOUT;
}

void mhi_write_reg(struct mhi_controller *mhi_cntrl, void __iomem *base,
		   u32 offset, u32 val)
{
	mhi_cntrl->write_reg(mhi_cntrl, base + offset, val);
}

void mhi_write_reg_field(struct mhi_controller *mhi_cntrl, void __iomem *base,
			 u32 offset, u32 mask, u32 shift, u32 val)
{
	int ret;
	u32 tmp;

	ret = mhi_read_reg(mhi_cntrl, base, offset, &tmp);
	if (ret)
		return;

	tmp &= ~mask;
	tmp |= (val << shift);
	mhi_write_reg(mhi_cntrl, base, offset, tmp);
}

void mhi_write_db(struct mhi_controller *mhi_cntrl, void __iomem *db_addr,
		  dma_addr_t db_val)
{
	mhi_write_reg(mhi_cntrl, db_addr, 4, upper_32_bits(db_val));
	mhi_write_reg(mhi_cntrl, db_addr, 0, lower_32_bits(db_val));
}

void mhi_db_brstmode(struct mhi_controller *mhi_cntrl,
		     struct db_cfg *db_cfg,
		     void __iomem *db_addr,
		     dma_addr_t db_val)
{
	if (db_cfg->db_mode) {
		db_cfg->db_val = db_val;
		mhi_write_db(mhi_cntrl, db_addr, db_val);
		db_cfg->db_mode = 0;
	}
}

void mhi_db_brstmode_disable(struct mhi_controller *mhi_cntrl,
			     struct db_cfg *db_cfg,
			     void __iomem *db_addr,
			     dma_addr_t db_val)
{
	db_cfg->db_val = db_val;
	mhi_write_db(mhi_cntrl, db_addr, db_val);
}

void mhi_ring_er_db(struct mhi_event *mhi_event)
{
	struct mhi_ring *ring = &mhi_event->ring;

	mhi_event->db_cfg.process_db(mhi_event->mhi_cntrl, &mhi_event->db_cfg,
				     ring->db_addr, *ring->ctxt_wp);
}

void mhi_ring_cmd_db(struct mhi_controller *mhi_cntrl, struct mhi_cmd *mhi_cmd)
{
	dma_addr_t db;
	struct mhi_ring *ring = &mhi_cmd->ring;

	db = ring->iommu_base + (ring->wp - ring->base);
	*ring->ctxt_wp = db;
	mhi_write_db(mhi_cntrl, ring->db_addr, db);
}

void mhi_ring_chan_db(struct mhi_controller *mhi_cntrl,
		      struct mhi_chan *mhi_chan)
{
	struct mhi_ring *ring = &mhi_chan->tre_ring;
	dma_addr_t db;

	db = ring->iommu_base + (ring->wp - ring->base);
	*ring->ctxt_wp = db;
	mhi_chan->db_cfg.process_db(mhi_cntrl, &mhi_chan->db_cfg,
				    ring->db_addr, db);
}

enum mhi_ee_type mhi_get_exec_env(struct mhi_controller *mhi_cntrl)
{
	u32 exec;
	int ret = mhi_read_reg(mhi_cntrl, mhi_cntrl->bhi, BHI_EXECENV, &exec);

	return (ret) ? MHI_EE_MAX : exec;
}
EXPORT_SYMBOL_GPL(mhi_get_exec_env);

enum mhi_state mhi_get_mhi_state(struct mhi_controller *mhi_cntrl)
{
	u32 state;
	int ret = mhi_read_reg_field(mhi_cntrl, mhi_cntrl->regs, MHISTATUS,
				     MHISTATUS_MHISTATE_MASK,
				     MHISTATUS_MHISTATE_SHIFT, &state);
	return ret ? MHI_STATE_MAX : state;
}
EXPORT_SYMBOL_GPL(mhi_get_mhi_state);

void mhi_soc_reset(struct mhi_controller *mhi_cntrl)
{
	if (mhi_cntrl->reset) {
		mhi_cntrl->reset(mhi_cntrl);
		return;
	}

	/* Generic MHI SoC reset */
	mhi_write_reg(mhi_cntrl, mhi_cntrl->regs, MHI_SOC_RESET_REQ_OFFSET,
		      MHI_SOC_RESET_REQ);
}
EXPORT_SYMBOL_GPL(mhi_soc_reset);

int mhi_map_single_no_bb(struct mhi_controller *mhi_cntrl,
			 struct mhi_buf_info *buf_info)
{
	buf_info->p_addr = dma_map_single(mhi_cntrl->cntrl_dev,
					  buf_info->v_addr, buf_info->len,
					  buf_info->dir);
	if (dma_mapping_error(mhi_cntrl->cntrl_dev, buf_info->p_addr))
		return -ENOMEM;

	return 0;
}

int mhi_map_single_use_bb(struct mhi_controller *mhi_cntrl,
			  struct mhi_buf_info *buf_info)
{
	void *buf = dma_alloc_coherent(mhi_cntrl->cntrl_dev, buf_info->len,
				       &buf_info->p_addr, GFP_ATOMIC);

	if (!buf)
		return -ENOMEM;

	if (buf_info->dir == DMA_TO_DEVICE)
		memcpy(buf, buf_info->v_addr, buf_info->len);

	buf_info->bb_addr = buf;

	return 0;
}

void mhi_unmap_single_no_bb(struct mhi_controller *mhi_cntrl,
			    struct mhi_buf_info *buf_info)
{
	dma_unmap_single(mhi_cntrl->cntrl_dev, buf_info->p_addr, buf_info->len,
			 buf_info->dir);
}

void mhi_unmap_single_use_bb(struct mhi_controller *mhi_cntrl,
			     struct mhi_buf_info *buf_info)
{
	if (buf_info->dir == DMA_FROM_DEVICE)
		memcpy(buf_info->v_addr, buf_info->bb_addr, buf_info->len);

	dma_free_coherent(mhi_cntrl->cntrl_dev, buf_info->len, buf_info->bb_addr,
			  buf_info->p_addr);
}

static int get_nr_avail_ring_elements(struct mhi_controller *mhi_cntrl,
				      struct mhi_ring *ring)
{
	int nr_el;

	if (ring->wp < ring->rp) {
		nr_el = ((ring->rp - ring->wp) / ring->el_size) - 1;
	} else {
		nr_el = (ring->rp - ring->base) / ring->el_size;
		nr_el += ((ring->base + ring->len - ring->wp) /
			  ring->el_size) - 1;
	}

	return nr_el;
}

void *mhi_to_virtual(struct mhi_ring *ring, dma_addr_t addr)
{
	return (addr - ring->iommu_base) + ring->base;
}

static void mhi_add_ring_element(struct mhi_controller *mhi_cntrl,
				 struct mhi_ring *ring)
{
	ring->wp += ring->el_size;
	if (ring->wp >= (ring->base + ring->len))
		ring->wp = ring->base;
	/* smp update */
	smp_wmb();
}

static void mhi_del_ring_element(struct mhi_controller *mhi_cntrl,
				 struct mhi_ring *ring)
{
	ring->rp += ring->el_size;
	if (ring->rp >= (ring->base + ring->len))
		ring->rp = ring->base;
	/* smp update */
	smp_wmb();
}

static bool is_valid_ring_ptr(struct mhi_ring *ring, dma_addr_t addr)
{
	return addr >= ring->iommu_base && addr < ring->iommu_base + ring->len;
}

int mhi_destroy_device(struct device *dev, void *data)
{
	struct mhi_chan *ul_chan, *dl_chan;
	struct mhi_device *mhi_dev;
	struct mhi_controller *mhi_cntrl;
	enum mhi_ee_type ee = MHI_EE_MAX;

	if (dev->bus != &mhi_bus_type)
		return 0;

	mhi_dev = to_mhi_device(dev);
	mhi_cntrl = mhi_dev->mhi_cntrl;

	/* Only destroy virtual devices thats attached to bus */
	if (mhi_dev->dev_type == MHI_DEVICE_CONTROLLER)
		return 0;

	ul_chan = mhi_dev->ul_chan;
	dl_chan = mhi_dev->dl_chan;

	/*
	 * If execution environment is specified, remove only those devices that
	 * started in them based on ee_mask for the channels as we move on to a
	 * different execution environment
	 */
	if (data)
		ee = *(enum mhi_ee_type *)data;

	/*
	 * For the suspend and resume case, this function will get called
	 * without mhi_unregister_controller(). Hence, we need to drop the
	 * references to mhi_dev created for ul and dl channels. We can
	 * be sure that there will be no instances of mhi_dev left after
	 * this.
	 */
	if (ul_chan) {
		if (ee != MHI_EE_MAX && !(ul_chan->ee_mask & BIT(ee)))
			return 0;

		put_device(&ul_chan->mhi_dev->dev);
	}

	if (dl_chan) {
		if (ee != MHI_EE_MAX && !(dl_chan->ee_mask & BIT(ee)))
			return 0;

		put_device(&dl_chan->mhi_dev->dev);
	}

	dev_dbg(&mhi_cntrl->mhi_dev->dev, "destroy device for chan:%s\n",
		 mhi_dev->name);

	/* Notify the client and remove the device from MHI bus */
	device_del(dev);
	put_device(dev);

	return 0;
}

int mhi_get_free_desc_count(struct mhi_device *mhi_dev,
				enum dma_data_direction dir)
{
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct mhi_chan *mhi_chan = (dir == DMA_TO_DEVICE) ?
		mhi_dev->ul_chan : mhi_dev->dl_chan;
	struct mhi_ring *tre_ring = &mhi_chan->tre_ring;

	return get_nr_avail_ring_elements(mhi_cntrl, tre_ring);
}
EXPORT_SYMBOL(mhi_get_free_desc_count);

void mhi_notify(struct mhi_device *mhi_dev, enum mhi_callback cb_reason)
{
	struct mhi_driver *mhi_drv;

	if (!mhi_dev->dev.driver)
		return;

	mhi_drv = to_mhi_driver(mhi_dev->dev.driver);

	if (mhi_drv->status_cb)
		mhi_drv->status_cb(mhi_dev, cb_reason);
}
EXPORT_SYMBOL_GPL(mhi_notify);

/* Bind MHI channels to MHI devices */
void mhi_create_devices(struct mhi_controller *mhi_cntrl)
{
	struct mhi_chan *mhi_chan;
	struct mhi_device *mhi_dev;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	int i, ret;

	mhi_chan = mhi_cntrl->mhi_chan;
	for (i = 0; i < mhi_cntrl->max_chan; i++, mhi_chan++) {
		if (!mhi_chan->configured || mhi_chan->mhi_dev ||
		    !(mhi_chan->ee_mask & BIT(mhi_cntrl->ee)))
			continue;
		mhi_dev = mhi_alloc_device(mhi_cntrl);
		if (IS_ERR(mhi_dev))
			return;

		mhi_dev->dev_type = MHI_DEVICE_XFER;
		switch (mhi_chan->dir) {
		case DMA_TO_DEVICE:
			mhi_dev->ul_chan = mhi_chan;
			mhi_dev->ul_chan_id = mhi_chan->chan;
			mhi_dev->ul_event_id = mhi_chan->er_index;
			break;
		case DMA_FROM_DEVICE:
			/* We use dl_chan as offload channels */
			mhi_dev->dl_chan = mhi_chan;
			mhi_dev->dl_chan_id = mhi_chan->chan;
			mhi_dev->dl_event_id = mhi_chan->er_index;
			break;
		default:
			MHI_ERR("Direction not supported\n");
			put_device(&mhi_dev->dev);
			return;
		}

		get_device(&mhi_dev->dev);
		mhi_chan->mhi_dev = mhi_dev;

		/* Check next channel if it matches */
		if ((i + 1) < mhi_cntrl->max_chan && mhi_chan[1].configured) {
			if (!strcmp(mhi_chan[1].name, mhi_chan->name)) {
				i++;
				mhi_chan++;
				if (mhi_chan->dir == DMA_TO_DEVICE) {
					mhi_dev->ul_chan = mhi_chan;
					mhi_dev->ul_chan_id = mhi_chan->chan;
					mhi_dev->ul_event_id = mhi_chan->er_index;
				} else {
					mhi_dev->dl_chan = mhi_chan;
					mhi_dev->dl_chan_id = mhi_chan->chan;
					mhi_dev->dl_event_id = mhi_chan->er_index;
				}
				get_device(&mhi_dev->dev);
				mhi_chan->mhi_dev = mhi_dev;
			}
		}

		/* Channel name is same for both UL and DL */
		mhi_dev->name = mhi_chan->name;
		dev_set_name(&mhi_dev->dev, "%s_%s", dev_name(dev),
			     mhi_dev->name);

		/* Init wakeup source if available */
		if (mhi_dev->dl_chan && mhi_dev->dl_chan->wake_capable)
			device_init_wakeup(&mhi_dev->dev, true);

		ret = device_add(&mhi_dev->dev);
		if (ret)
			put_device(&mhi_dev->dev);
	}
}

void mhi_process_sleeping_events(struct mhi_controller *mhi_cntrl)
{
	struct mhi_event *mhi_event;
	struct mhi_event_ctxt *er_ctxt;
	struct mhi_ring *ev_ring;
	int i;

	mhi_event = mhi_cntrl->mhi_event;
	for (i = 0; i < mhi_cntrl->total_ev_rings; i++, mhi_event++) {
		if (mhi_event->offload_ev || mhi_event->priority !=
		    MHI_ER_PRIORITY_HI_SLEEP)
			continue;

		er_ctxt = &mhi_cntrl->mhi_ctxt->er_ctxt[mhi_event->er_index];
		ev_ring = &mhi_event->ring;

		/* Only proceed if event ring has pending events */
		if (ev_ring->rp == mhi_to_virtual(ev_ring, er_ctxt->rp))
			continue;

		queue_work(mhi_cntrl->hiprio_wq, &mhi_event->work);
	}
}

irqreturn_t mhi_irq_handler(int irq_number, void *priv)
{
	struct mhi_event *mhi_event = priv;
	struct mhi_controller *mhi_cntrl = mhi_event->mhi_cntrl;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	struct mhi_event_ctxt *er_ctxt =
		&mhi_cntrl->mhi_ctxt->er_ctxt[mhi_event->er_index];
	struct mhi_ring *ev_ring = &mhi_event->ring;
	dma_addr_t ptr = er_ctxt->rp;
	void *dev_rp;

	if (!is_valid_ring_ptr(ev_ring, ptr)) {
		MHI_ERR("Event ring rp points outside of the event ring\n");
		return IRQ_HANDLED;
	}

	dev_rp = mhi_to_virtual(ev_ring, ptr);

	/* Only proceed if event ring has pending events */
	if (ev_ring->rp == dev_rp)
		return IRQ_HANDLED;

	/* For client managed event ring, notify pending data */
	if (mhi_event->cl_manage) {
		struct mhi_chan *mhi_chan = mhi_event->mhi_chan;
		struct mhi_device *mhi_dev = mhi_chan->mhi_dev;

		if (mhi_dev)
			mhi_notify(mhi_dev, MHI_CB_PENDING_DATA);

		return IRQ_HANDLED;
	}

	switch (mhi_event->priority) {
	case MHI_ER_PRIORITY_HI_NOSLEEP:
		tasklet_hi_schedule(&mhi_event->task);
		break;
	case MHI_ER_PRIORITY_DEFAULT_NOSLEEP:
		tasklet_schedule(&mhi_event->task);
		break;
	case MHI_ER_PRIORITY_HI_SLEEP:
		queue_work(mhi_cntrl->hiprio_wq, &mhi_event->work);
		break;
	default:
		MHI_VERB("skip unknown priority event\n");
		break;
	}

	return IRQ_HANDLED;
}

irqreturn_t mhi_intvec_threaded_handler(int irq_number, void *priv)
{
	struct mhi_controller *mhi_cntrl = priv;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	enum mhi_state state = MHI_STATE_MAX;
	enum mhi_pm_state pm_state = 0;
	enum mhi_ee_type ee = MHI_EE_MAX;

	write_lock_irq(&mhi_cntrl->pm_lock);
	if (!MHI_REG_ACCESS_VALID(mhi_cntrl->pm_state)) {
		write_unlock_irq(&mhi_cntrl->pm_lock);
		goto exit_intvec;
	}

	state = mhi_get_mhi_state(mhi_cntrl);
	ee = mhi_get_exec_env(mhi_cntrl);
	MHI_VERB("local ee:%s state:%s device ee:%s state:%s\n",
		TO_MHI_EXEC_STR(mhi_cntrl->ee),
		TO_MHI_STATE_STR(mhi_cntrl->dev_state),
		TO_MHI_EXEC_STR(ee), TO_MHI_STATE_STR(state));

	if (state == MHI_STATE_SYS_ERR) {
		MHI_VERB("System error detected\n");
		pm_state = mhi_tryset_pm_state(mhi_cntrl,
					       MHI_PM_SYS_ERR_DETECT);
	}
	write_unlock_irq(&mhi_cntrl->pm_lock);

	 /* If device supports RDDM don't bother processing SYS error */
	if (mhi_cntrl->rddm_image) {
		/* host may be performing a device power down already */
		if (!mhi_is_active(mhi_cntrl))
			goto exit_intvec;

		if (ee == MHI_EE_RDDM && mhi_cntrl->ee != MHI_EE_RDDM) {
			mhi_cntrl->status_cb(mhi_cntrl, MHI_CB_EE_RDDM);
			mhi_cntrl->ee = ee;
			wake_up_all(&mhi_cntrl->state_event);
		}
		goto exit_intvec;
	}

	if (pm_state == MHI_PM_SYS_ERR_DETECT) {
		wake_up_all(&mhi_cntrl->state_event);

		/* For fatal errors, we let controller decide next step */
		if (MHI_IN_PBL(ee)) {
			mhi_cntrl->status_cb(mhi_cntrl, MHI_CB_FATAL_ERROR);
			mhi_cntrl->ee = ee;
		} else {
			mhi_pm_sys_err_handler(mhi_cntrl);
		}
	}

exit_intvec:

	return IRQ_HANDLED;
}

irqreturn_t mhi_intvec_handler(int irq_number, void *dev)
{
	struct mhi_controller *mhi_cntrl = dev;

	/* Wake up events waiting for state change */
	wake_up_all(&mhi_cntrl->state_event);

	return IRQ_WAKE_THREAD;
}

static void mhi_recycle_ev_ring_element(struct mhi_controller *mhi_cntrl,
					struct mhi_ring *ring)
{
	dma_addr_t ctxt_wp;

	/* Update the WP */
	ring->wp += ring->el_size;
	ctxt_wp = *ring->ctxt_wp + ring->el_size;

	if (ring->wp >= (ring->base + ring->len)) {
		ring->wp = ring->base;
		ctxt_wp = ring->iommu_base;
	}

	*ring->ctxt_wp = ctxt_wp;

	/* Update the RP */
	ring->rp += ring->el_size;
	if (ring->rp >= (ring->base + ring->len))
		ring->rp = ring->base;

	/* Update to all cores */
	smp_wmb();
}

static int parse_xfer_event(struct mhi_controller *mhi_cntrl,
			    struct mhi_tre *event,
			    struct mhi_chan *mhi_chan)
{
	struct mhi_ring *buf_ring, *tre_ring;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	struct mhi_result result;
	unsigned long flags = 0;
	u32 ev_code;

	ev_code = MHI_TRE_GET_EV_CODE(event);
	buf_ring = &mhi_chan->buf_ring;
	tre_ring = &mhi_chan->tre_ring;

	result.transaction_status = (ev_code == MHI_EV_CC_OVERFLOW) ?
		-EOVERFLOW : 0;

	/*
	 * If it's a DB Event then we need to grab the lock
	 * with preemption disabled and as a write because we
	 * have to update db register and there are chances that
	 * another thread could be doing the same.
	 */
	if (ev_code >= MHI_EV_CC_OOB)
		write_lock_irqsave(&mhi_chan->lock, flags);
	else
		read_lock_bh(&mhi_chan->lock);

	if (mhi_chan->ch_state != MHI_CH_STATE_ENABLED)
		goto end_process_tx_event;

	switch (ev_code) {
	case MHI_EV_CC_OVERFLOW:
	case MHI_EV_CC_EOB:
	case MHI_EV_CC_EOT:
	{
		dma_addr_t ptr = MHI_TRE_GET_EV_PTR(event);
		struct mhi_tre *local_rp, *ev_tre;
		void *dev_rp;
		struct mhi_buf_info *buf_info;
		u16 xfer_len;

		if (!is_valid_ring_ptr(tre_ring, ptr)) {
			MHI_ERR("Event element points outside of the tre ring\n");
			break;
		}
		/* Get the TRB this event points to */
		ev_tre = mhi_to_virtual(tre_ring, ptr);

		dev_rp = ev_tre + 1;
		if (dev_rp >= (tre_ring->base + tre_ring->len))
			dev_rp = tre_ring->base;

		result.dir = mhi_chan->dir;

		local_rp = tre_ring->rp;
		while (local_rp != dev_rp) {
			buf_info = buf_ring->rp;
			/* If it's the last TRE, get length from the event */
			if (local_rp == ev_tre)
				xfer_len = MHI_TRE_GET_EV_LEN(event);
			else
				xfer_len = buf_info->len;

			/* Unmap if it's not pre-mapped by client */
			if (likely(!buf_info->pre_mapped))
				mhi_cntrl->unmap_single(mhi_cntrl, buf_info);

			result.buf_addr = buf_info->cb_buf;

			/* truncate to buf len if xfer_len is larger */
			result.bytes_xferd =
				min_t(u16, xfer_len, buf_info->len);
			mhi_del_ring_element(mhi_cntrl, buf_ring);
			mhi_del_ring_element(mhi_cntrl, tre_ring);
			local_rp = tre_ring->rp;

			/* notify client */
			mhi_chan->xfer_cb(mhi_chan->mhi_dev, &result);

			if (mhi_chan->dir == DMA_TO_DEVICE)
				atomic_dec(&mhi_cntrl->pending_pkts);

			/*
			 * Recycle the buffer if buffer is pre-allocated,
			 * if there is an error, not much we can do apart
			 * from dropping the packet
			 */
			if (mhi_chan->pre_alloc) {
				if (mhi_queue_buf(mhi_chan->mhi_dev,
						  mhi_chan->dir,
						  buf_info->cb_buf,
						  buf_info->len, MHI_EOT)) {
					MHI_ERR(
						"Error recycling buffer for chan:%d\n",
						mhi_chan->chan);
					kfree(buf_info->cb_buf);
				}
			}
		}
		break;
	} /* CC_EOT */
	case MHI_EV_CC_OOB:
	case MHI_EV_CC_DB_MODE:
	{
		unsigned long flags;

		mhi_chan->db_cfg.db_mode = 1;
		read_lock_irqsave(&mhi_cntrl->pm_lock, flags);
		if (tre_ring->wp != tre_ring->rp &&
		    MHI_DB_ACCESS_VALID(mhi_cntrl)) {
			mhi_ring_chan_db(mhi_cntrl, mhi_chan);
		}
		read_unlock_irqrestore(&mhi_cntrl->pm_lock, flags);
		break;
	}
	case MHI_EV_CC_BAD_TRE:
	default:
		MHI_ERR("Unknown event 0x%x\n", ev_code);
		panic("Unknown event 0x%x\n", ev_code);
		break;
	} /* switch(MHI_EV_READ_CODE(EV_TRB_CODE,event)) */

end_process_tx_event:
	if (ev_code >= MHI_EV_CC_OOB)
		write_unlock_irqrestore(&mhi_chan->lock, flags);
	else
		read_unlock_bh(&mhi_chan->lock);

	return 0;
}

static int parse_rsc_event(struct mhi_controller *mhi_cntrl,
			   struct mhi_tre *event,
			   struct mhi_chan *mhi_chan)
{
	struct mhi_ring *buf_ring, *tre_ring;
	struct mhi_buf_info *buf_info;
	struct mhi_result result;
	int ev_code;
	u32 cookie; /* offset to local descriptor */
	u16 xfer_len;

	buf_ring = &mhi_chan->buf_ring;
	tre_ring = &mhi_chan->tre_ring;

	ev_code = MHI_TRE_GET_EV_CODE(event);
	cookie = MHI_TRE_GET_EV_COOKIE(event);
	xfer_len = MHI_TRE_GET_EV_LEN(event);

	/* Received out of bound cookie */
	WARN_ON(cookie >= buf_ring->len);

	buf_info = buf_ring->base + cookie;

	result.transaction_status = (ev_code == MHI_EV_CC_OVERFLOW) ?
		-EOVERFLOW : 0;

	/* truncate to buf len if xfer_len is larger */
	result.bytes_xferd = min_t(u16, xfer_len, buf_info->len);
	result.buf_addr = buf_info->cb_buf;
	result.dir = mhi_chan->dir;

	read_lock_bh(&mhi_chan->lock);

	if (mhi_chan->ch_state != MHI_CH_STATE_ENABLED)
		goto end_process_rsc_event;

	WARN_ON(!buf_info->used);

	/* notify the client */
	mhi_chan->xfer_cb(mhi_chan->mhi_dev, &result);

	/*
	 * Note: We're arbitrarily incrementing RP even though, completion
	 * packet we processed might not be the same one, reason we can do this
	 * is because device guaranteed to cache descriptors in order it
	 * receive, so even though completion event is different we can re-use
	 * all descriptors in between.
	 * Example:
	 * Transfer Ring has descriptors: A, B, C, D
	 * Last descriptor host queue is D (WP) and first descriptor
	 * host queue is A (RP).
	 * The completion event we just serviced is descriptor C.
	 * Then we can safely queue descriptors to replace A, B, and C
	 * even though host did not receive any completions.
	 */
	mhi_del_ring_element(mhi_cntrl, tre_ring);
	buf_info->used = false;

end_process_rsc_event:
	read_unlock_bh(&mhi_chan->lock);

	return 0;
}

static void mhi_process_cmd_completion(struct mhi_controller *mhi_cntrl,
				       struct mhi_tre *tre)
{
	dma_addr_t ptr = MHI_TRE_GET_EV_PTR(tre);
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	struct mhi_cmd *cmd_ring = &mhi_cntrl->mhi_cmd[PRIMARY_CMD_RING];
	struct mhi_ring *mhi_ring = &cmd_ring->ring;
	struct mhi_tre *cmd_pkt;
	struct mhi_chan *mhi_chan;
	u32 chan;

	if (!is_valid_ring_ptr(mhi_ring, ptr)) {
		MHI_ERR("Event element points outside of the cmd ring\n");
		return;
	}

	cmd_pkt = mhi_to_virtual(mhi_ring, ptr);

	if (cmd_pkt != mhi_ring->rp)
		panic("Out of order cmd completion: 0x%llx. Expected: 0x%llx\n",
		      cmd_pkt, mhi_ring->rp);

	if (MHI_TRE_GET_CMD_TYPE(cmd_pkt) == MHI_CMD_SFR_CFG) {
		mhi_misc_cmd_completion(mhi_cntrl, MHI_CMD_SFR_CFG,
					MHI_TRE_GET_EV_CODE(tre));
		goto exit_cmd_completion;
	}

	chan = MHI_TRE_GET_CMD_CHID(cmd_pkt);
	if (chan >= mhi_cntrl->max_chan) {
		MHI_ERR("Invalid channel id: %u\n", chan);
		goto exit_cmd_completion;
	}
	mhi_chan = &mhi_cntrl->mhi_chan[chan];
	write_lock_bh(&mhi_chan->lock);
	mhi_chan->ccs = MHI_TRE_GET_EV_CODE(tre);
	complete(&mhi_chan->completion);
	write_unlock_bh(&mhi_chan->lock);

exit_cmd_completion:
	mhi_del_ring_element(mhi_cntrl, mhi_ring);
}

int mhi_process_ctrl_ev_ring(struct mhi_controller *mhi_cntrl,
			     struct mhi_event *mhi_event,
			     u32 event_quota)
{
	struct mhi_tre *dev_rp, *local_rp;
	struct mhi_ring *ev_ring = &mhi_event->ring;
	struct mhi_event_ctxt *er_ctxt =
		&mhi_cntrl->mhi_ctxt->er_ctxt[mhi_event->er_index];
	struct mhi_chan *mhi_chan;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	u32 chan;
	int count = 0;
	dma_addr_t ptr = er_ctxt->rp;

	/*
	 * This is a quick check to avoid unnecessary event processing
	 * in case MHI is already in error state, but it's still possible
	 * to transition to error state while processing events
	 */
	if (unlikely(MHI_EVENT_ACCESS_INVALID(mhi_cntrl->pm_state)))
		return -EIO;

	if (!is_valid_ring_ptr(ev_ring, ptr)) {
		MHI_ERR("Event ring rp points outside of the event ring\n");
		return -EIO;
	}

	dev_rp = mhi_to_virtual(ev_ring, ptr);
	local_rp = ev_ring->rp;

	while (dev_rp != local_rp) {
		enum mhi_pkt_type type = MHI_TRE_GET_EV_TYPE(local_rp);

		MHI_VERB("Processing Event:0x%llx 0x%08x 0x%08x\n",
			 local_rp->ptr, local_rp->dword[0], local_rp->dword[1]);

		switch (type) {
		case MHI_PKT_TYPE_BW_REQ_EVENT:
		{
			struct mhi_link_info *link_info;

			link_info = &mhi_cntrl->mhi_link_info;
			write_lock_irq(&mhi_cntrl->pm_lock);
			link_info->target_link_speed =
				MHI_TRE_GET_EV_LINKSPEED(local_rp);
			link_info->target_link_width =
				MHI_TRE_GET_EV_LINKWIDTH(local_rp);
			write_unlock_irq(&mhi_cntrl->pm_lock);
			MHI_VERB("Received BW_REQ event\n");
			mhi_cntrl->status_cb(mhi_cntrl, MHI_CB_BW_REQ);
			break;
		}
		case MHI_PKT_TYPE_STATE_CHANGE_EVENT:
		{
			enum mhi_state new_state;

			new_state = MHI_TRE_GET_EV_STATE(local_rp);

			MHI_VERB("State change event to state: %s\n",
				TO_MHI_STATE_STR(new_state));

			switch (new_state) {
			case MHI_STATE_M0:
				mhi_pm_m0_transition(mhi_cntrl);
				break;
			case MHI_STATE_M1:
				mhi_pm_m1_transition(mhi_cntrl);
				break;
			case MHI_STATE_M3:
				mhi_pm_m3_transition(mhi_cntrl);
				break;
			case MHI_STATE_SYS_ERR:
			{
				enum mhi_pm_state new_state;

				MHI_VERB("System error detected\n");
				write_lock_irq(&mhi_cntrl->pm_lock);
				new_state = mhi_tryset_pm_state(mhi_cntrl,
							MHI_PM_SYS_ERR_DETECT);
				write_unlock_irq(&mhi_cntrl->pm_lock);
				if (new_state == MHI_PM_SYS_ERR_DETECT)
					mhi_pm_sys_err_handler(mhi_cntrl);
				break;
			}
			default:
				MHI_ERR("Invalid state: %s\n",
					TO_MHI_STATE_STR(new_state));
			}

			break;
		}
		case MHI_PKT_TYPE_CMD_COMPLETION_EVENT:
			mhi_process_cmd_completion(mhi_cntrl, local_rp);
			break;
		case MHI_PKT_TYPE_EE_EVENT:
		{
			enum dev_st_transition st = DEV_ST_TRANSITION_MAX;
			enum mhi_ee_type event = MHI_TRE_GET_EV_EXECENV(local_rp);

			MHI_VERB("Received EE event: %s\n",
				TO_MHI_EXEC_STR(event));
			switch (event) {
			case MHI_EE_SBL:
				st = DEV_ST_TRANSITION_SBL;
				break;
			case MHI_EE_WFW:
			case MHI_EE_AMSS:
				st = DEV_ST_TRANSITION_MISSION_MODE;
				break;
			case MHI_EE_RDDM:
				mhi_cntrl->status_cb(mhi_cntrl, MHI_CB_EE_RDDM);
				write_lock_irq(&mhi_cntrl->pm_lock);
				mhi_cntrl->ee = event;
				write_unlock_irq(&mhi_cntrl->pm_lock);
				wake_up_all(&mhi_cntrl->state_event);
				break;
			default:
				MHI_ERR(
					"Unhandled EE event: 0x%x\n", type);
			}
			if (st != DEV_ST_TRANSITION_MAX)
				mhi_queue_state_transition(mhi_cntrl, st);

			break;
		}
		case MHI_PKT_TYPE_TX_EVENT:
			chan = MHI_TRE_GET_EV_CHID(local_rp);

			WARN_ON(chan >= mhi_cntrl->max_chan);

			/*
			 * Only process the event ring elements whose channel
			 * ID is within the maximum supported range.
			 */
			if (chan < mhi_cntrl->max_chan) {
				mhi_chan = &mhi_cntrl->mhi_chan[chan];
				if (!mhi_chan->configured)
					break;
				parse_xfer_event(mhi_cntrl, local_rp, mhi_chan);
				event_quota--;
			}
			break;
		default:
			MHI_ERR("Unhandled event type: %d\n", type);
			break;
		}

		mhi_recycle_ev_ring_element(mhi_cntrl, ev_ring);
		local_rp = ev_ring->rp;

		ptr = er_ctxt->rp;
		if (!is_valid_ring_ptr(ev_ring, ptr)) {
			MHI_ERR("Event ring rp points outside of the event ring\n");
			return -EIO;
		}

		dev_rp = mhi_to_virtual(ev_ring, ptr);
		count++;
	}

	read_lock_bh(&mhi_cntrl->pm_lock);
	if (likely(MHI_DB_ACCESS_VALID(mhi_cntrl)))
		mhi_ring_er_db(mhi_event);
	read_unlock_bh(&mhi_cntrl->pm_lock);

	return count;
}

int mhi_process_data_event_ring(struct mhi_controller *mhi_cntrl,
				struct mhi_event *mhi_event,
				u32 event_quota)
{
	struct mhi_tre *dev_rp, *local_rp;
	struct mhi_ring *ev_ring = &mhi_event->ring;
	struct mhi_event_ctxt *er_ctxt =
		&mhi_cntrl->mhi_ctxt->er_ctxt[mhi_event->er_index];
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	int count = 0;
	u32 chan;
	struct mhi_chan *mhi_chan;
	dma_addr_t ptr = er_ctxt->rp;

	if (unlikely(MHI_EVENT_ACCESS_INVALID(mhi_cntrl->pm_state)))
		return -EIO;

	if (!is_valid_ring_ptr(ev_ring, ptr)) {
		MHI_ERR("Event ring rp points outside of the event ring\n");
		return -EIO;
	}

	dev_rp = mhi_to_virtual(ev_ring, ptr);
	local_rp = ev_ring->rp;

	while (dev_rp != local_rp && event_quota > 0) {
		enum mhi_pkt_type type = MHI_TRE_GET_EV_TYPE(local_rp);

		MHI_VERB("Processing Event:0x%llx 0x%08x 0x%08x\n",
			 local_rp->ptr, local_rp->dword[0], local_rp->dword[1]);

		chan = MHI_TRE_GET_EV_CHID(local_rp);

		WARN_ON(chan >= mhi_cntrl->max_chan);

		/*
		 * Only process the event ring elements whose channel
		 * ID is within the maximum supported range.
		 */
		if (chan < mhi_cntrl->max_chan &&
		    mhi_cntrl->mhi_chan[chan].configured) {
			mhi_chan = &mhi_cntrl->mhi_chan[chan];

			if (likely(type == MHI_PKT_TYPE_TX_EVENT)) {
				parse_xfer_event(mhi_cntrl, local_rp, mhi_chan);
				event_quota--;
			} else if (type == MHI_PKT_TYPE_RSC_TX_EVENT) {
				parse_rsc_event(mhi_cntrl, local_rp, mhi_chan);
				event_quota--;
			}
		}

		mhi_recycle_ev_ring_element(mhi_cntrl, ev_ring);
		local_rp = ev_ring->rp;

		ptr = er_ctxt->rp;
		if (!is_valid_ring_ptr(ev_ring, ptr)) {
			MHI_ERR("Event ring rp points outside of the event ring\n");
			return -EIO;
		}

		dev_rp = mhi_to_virtual(ev_ring, ptr);
		count++;
	}
	read_lock_bh(&mhi_cntrl->pm_lock);
	if (likely(MHI_DB_ACCESS_VALID(mhi_cntrl)))
		mhi_ring_er_db(mhi_event);
	read_unlock_bh(&mhi_cntrl->pm_lock);

	return count;
}

void mhi_ev_task(unsigned long data)
{
	unsigned long flags;
	struct mhi_event *mhi_event = (struct mhi_event *)data;
	struct mhi_controller *mhi_cntrl = mhi_event->mhi_cntrl;

	/* process all pending events */
	spin_lock_irqsave(&mhi_event->lock, flags);
	mhi_event->process_event(mhi_cntrl, mhi_event, U32_MAX);
	spin_unlock_irqrestore(&mhi_event->lock, flags);
}

void mhi_ctrl_ev_task(unsigned long data)
{
	struct mhi_event *mhi_event = (struct mhi_event *)data;
	struct mhi_controller *mhi_cntrl = mhi_event->mhi_cntrl;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	enum mhi_state state;
	enum mhi_pm_state pm_state = 0;
	int ret;

	/*
	 * We can check PM state w/o a lock here because there is no way
	 * PM state can change from reg access valid to no access while this
	 * thread being executed.
	 */
	if (!MHI_REG_ACCESS_VALID(mhi_cntrl->pm_state)) {
		/*
		 * We may have a pending event but not allowed to
		 * process it since we are probably in a suspended state,
		 * so trigger a resume.
		 */
		mhi_trigger_resume(mhi_cntrl);

		return;
	}

	/* Process ctrl events events */
	ret = mhi_event->process_event(mhi_cntrl, mhi_event, U32_MAX);

	/*
	 * We received an IRQ but no events to process, maybe device went to
	 * SYS_ERR state? Check the state to confirm.
	 */
	if (!ret) {
		write_lock_irq(&mhi_cntrl->pm_lock);
		state = mhi_get_mhi_state(mhi_cntrl);
		if (state == MHI_STATE_SYS_ERR) {
			MHI_VERB("System error detected\n");
			pm_state = mhi_tryset_pm_state(mhi_cntrl,
						       MHI_PM_SYS_ERR_DETECT);
		}
		write_unlock_irq(&mhi_cntrl->pm_lock);
		if (pm_state == MHI_PM_SYS_ERR_DETECT)
			mhi_pm_sys_err_handler(mhi_cntrl);
	}
}

void mhi_process_ev_work(struct work_struct *work)
{
	struct mhi_event *mhi_event = container_of(work, struct mhi_event,
						   work);
	struct mhi_controller *mhi_cntrl = mhi_event->mhi_cntrl;
	struct device *dev = mhi_cntrl->cntrl_dev;

	MHI_VERB("Enter with pm_state:%s MHI_STATE:%s ee:%s\n",
		to_mhi_pm_state_str(mhi_cntrl->pm_state),
		TO_MHI_STATE_STR(mhi_cntrl->dev_state),
		TO_MHI_EXEC_STR(mhi_cntrl->ee));

	if (unlikely(MHI_EVENT_ACCESS_INVALID(mhi_cntrl->pm_state)))
		return;

	mhi_event->process_event(mhi_cntrl, mhi_event, U32_MAX);
}

static bool mhi_is_ring_full(struct mhi_controller *mhi_cntrl,
			     struct mhi_ring *ring)
{
	void *tmp = ring->wp + ring->el_size;

	if (tmp >= (ring->base + ring->len))
		tmp = ring->base;

	return (tmp == ring->rp);
}

int mhi_queue_skb(struct mhi_device *mhi_dev, enum dma_data_direction dir,
		  struct sk_buff *skb, size_t len, enum mhi_flags mflags)
{
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct mhi_chan *mhi_chan = (dir == DMA_TO_DEVICE) ? mhi_dev->ul_chan :
							     mhi_dev->dl_chan;
	struct mhi_ring *tre_ring = &mhi_chan->tre_ring;
	struct mhi_buf_info buf_info = { };
	int ret;

	/* If MHI host pre-allocates buffers then client drivers cannot queue */
	if (mhi_chan->pre_alloc)
		return -EINVAL;

	if (mhi_is_ring_full(mhi_cntrl, tre_ring))
		return -ENOMEM;

	read_lock_bh(&mhi_cntrl->pm_lock);
	if (unlikely(MHI_PM_IN_ERROR_STATE(mhi_cntrl->pm_state))) {
		read_unlock_bh(&mhi_cntrl->pm_lock);
		return -EIO;
	}

	/* we're in M3 or transitioning to M3 */
	if (MHI_PM_IN_SUSPEND_STATE(mhi_cntrl->pm_state))
		mhi_trigger_resume(mhi_cntrl);

	/* Toggle wake to exit out of M2 */
	mhi_cntrl->wake_toggle(mhi_cntrl);

	buf_info.v_addr = skb->data;
	buf_info.cb_buf = skb;
	buf_info.len = len;

	ret = mhi_gen_tre(mhi_cntrl, mhi_chan, &buf_info, mflags);
	if (unlikely(ret)) {
		read_unlock_bh(&mhi_cntrl->pm_lock);
		return ret;
	}

	if (mhi_chan->dir == DMA_TO_DEVICE)
		atomic_inc(&mhi_cntrl->pending_pkts);

	if (likely(MHI_DB_ACCESS_VALID(mhi_cntrl))) {
		read_lock_bh(&mhi_chan->lock);
		mhi_ring_chan_db(mhi_cntrl, mhi_chan);
		read_unlock_bh(&mhi_chan->lock);
	}

	read_unlock_bh(&mhi_cntrl->pm_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(mhi_queue_skb);

int mhi_queue_dma(struct mhi_device *mhi_dev, enum dma_data_direction dir,
		  struct mhi_buf *mhi_buf, size_t len, enum mhi_flags mflags)
{
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct mhi_chan *mhi_chan = (dir == DMA_TO_DEVICE) ? mhi_dev->ul_chan :
							     mhi_dev->dl_chan;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	struct mhi_ring *tre_ring = &mhi_chan->tre_ring;
	struct mhi_buf_info buf_info = { };
	int ret;

	/* If MHI host pre-allocates buffers then client drivers cannot queue */
	if (mhi_chan->pre_alloc)
		return -EINVAL;

	if (mhi_is_ring_full(mhi_cntrl, tre_ring))
		return -ENOMEM;

	read_lock_bh(&mhi_cntrl->pm_lock);
	if (unlikely(MHI_PM_IN_ERROR_STATE(mhi_cntrl->pm_state))) {
		MHI_ERR("MHI is not in activate state, PM state: %s\n",
			to_mhi_pm_state_str(mhi_cntrl->pm_state));
		read_unlock_bh(&mhi_cntrl->pm_lock);

		return -EIO;
	}

	/* we're in M3 or transitioning to M3 */
	if (MHI_PM_IN_SUSPEND_STATE(mhi_cntrl->pm_state))
		mhi_trigger_resume(mhi_cntrl);

	/* Toggle wake to exit out of M2 */
	mhi_cntrl->wake_toggle(mhi_cntrl);

	buf_info.p_addr = mhi_buf->dma_addr;
	buf_info.cb_buf = mhi_buf;
	buf_info.pre_mapped = true;
	buf_info.len = len;

	ret = mhi_gen_tre(mhi_cntrl, mhi_chan, &buf_info, mflags);
	if (unlikely(ret)) {
		read_unlock_bh(&mhi_cntrl->pm_lock);
		return ret;
	}

	if (mhi_chan->dir == DMA_TO_DEVICE)
		atomic_inc(&mhi_cntrl->pending_pkts);

	if (likely(MHI_DB_ACCESS_VALID(mhi_cntrl))) {
		read_lock_bh(&mhi_chan->lock);
		mhi_ring_chan_db(mhi_cntrl, mhi_chan);
		read_unlock_bh(&mhi_chan->lock);
	}

	read_unlock_bh(&mhi_cntrl->pm_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(mhi_queue_dma);

int mhi_gen_tre(struct mhi_controller *mhi_cntrl, struct mhi_chan *mhi_chan,
			struct mhi_buf_info *info, enum mhi_flags flags)
{
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	struct mhi_ring *buf_ring, *tre_ring;
	struct mhi_tre *mhi_tre;
	struct mhi_buf_info *buf_info;
	int eot, eob, chain, bei;
	int ret;

	buf_ring = &mhi_chan->buf_ring;
	tre_ring = &mhi_chan->tre_ring;

	buf_info = buf_ring->wp;
	WARN_ON(buf_info->used);
	buf_info->pre_mapped = info->pre_mapped;
	if (info->pre_mapped)
		buf_info->p_addr = info->p_addr;
	else
		buf_info->v_addr = info->v_addr;
	buf_info->cb_buf = info->cb_buf;
	buf_info->wp = tre_ring->wp;
	buf_info->dir = mhi_chan->dir;
	buf_info->len = info->len;

	if (!info->pre_mapped) {
		ret = mhi_cntrl->map_single(mhi_cntrl, buf_info);
		if (ret)
			return ret;
	}

	eob = !!(flags & MHI_EOB);
	eot = !!(flags & MHI_EOT);
	chain = !!(flags & MHI_CHAIN);
	bei = !!(mhi_chan->intmod);

	mhi_tre = tre_ring->wp;
	mhi_tre->ptr = MHI_TRE_DATA_PTR(buf_info->p_addr);
	mhi_tre->dword[0] = MHI_TRE_DATA_DWORD0(info->len);
	mhi_tre->dword[1] = MHI_TRE_DATA_DWORD1(bei, eot, eob, chain);

	MHI_VERB("Channel: %d WP: 0x%llx TRE: 0x%llx 0x%08x 0x%08x\n",
		 mhi_chan->chan, mhi_tre, mhi_tre->ptr, mhi_tre->dword[0],
		 mhi_tre->dword[1]);

	/* increment WP */
	mhi_add_ring_element(mhi_cntrl, tre_ring);
	mhi_add_ring_element(mhi_cntrl, buf_ring);

	return 0;
}

int mhi_queue_buf(struct mhi_device *mhi_dev, enum dma_data_direction dir,
		  void *buf, size_t len, enum mhi_flags mflags)
{
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct mhi_chan *mhi_chan = (dir == DMA_TO_DEVICE) ? mhi_dev->ul_chan :
							     mhi_dev->dl_chan;
	struct mhi_ring *tre_ring;
	struct mhi_buf_info buf_info = { };
	unsigned long flags;
	int ret;

	/*
	 * this check here only as a guard, it's always
	 * possible mhi can enter error while executing rest of function,
	 * which is not fatal so we do not need to hold pm_lock
	 */
	if (unlikely(MHI_PM_IN_ERROR_STATE(mhi_cntrl->pm_state)))
		return -EIO;

	tre_ring = &mhi_chan->tre_ring;
	if (mhi_is_ring_full(mhi_cntrl, tre_ring))
		return -ENOMEM;

	buf_info.v_addr = buf;
	buf_info.cb_buf = buf;
	buf_info.len = len;

	ret = mhi_gen_tre(mhi_cntrl, mhi_chan, &buf_info, mflags);
	if (unlikely(ret))
		return ret;

	read_lock_irqsave(&mhi_cntrl->pm_lock, flags);

	/* we're in M3 or transitioning to M3 */
	if (MHI_PM_IN_SUSPEND_STATE(mhi_cntrl->pm_state))
		mhi_trigger_resume(mhi_cntrl);

	/* Toggle wake to exit out of M2 */
	mhi_cntrl->wake_toggle(mhi_cntrl);

	if (mhi_chan->dir == DMA_TO_DEVICE)
		atomic_inc(&mhi_cntrl->pending_pkts);

	if (likely(MHI_DB_ACCESS_VALID(mhi_cntrl))) {
		unsigned long flags;

		read_lock_irqsave(&mhi_chan->lock, flags);
		mhi_ring_chan_db(mhi_cntrl, mhi_chan);
		read_unlock_irqrestore(&mhi_chan->lock, flags);
	}

	read_unlock_irqrestore(&mhi_cntrl->pm_lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(mhi_queue_buf);

int mhi_send_cmd(struct mhi_controller *mhi_cntrl,
		 struct mhi_chan *mhi_chan,
		 enum mhi_cmd_type cmd)
{
	struct mhi_tre *cmd_tre = NULL;
	struct mhi_cmd *mhi_cmd = &mhi_cntrl->mhi_cmd[PRIMARY_CMD_RING];
	struct mhi_ring *ring = &mhi_cmd->ring;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	int chan = 0;

	if (mhi_chan)
		chan = mhi_chan->chan;

	spin_lock_bh(&mhi_cmd->lock);
	if (!get_nr_avail_ring_elements(mhi_cntrl, ring)) {
		spin_unlock_bh(&mhi_cmd->lock);
		return -ENOMEM;
	}

	/* prepare the cmd tre */
	cmd_tre = ring->wp;
	switch (cmd) {
	case MHI_CMD_RESET_CHAN:
		cmd_tre->ptr = MHI_TRE_CMD_RESET_PTR;
		cmd_tre->dword[0] = MHI_TRE_CMD_RESET_DWORD0;
		cmd_tre->dword[1] = MHI_TRE_CMD_RESET_DWORD1(chan);
		break;
	case MHI_CMD_STOP_CHAN:
		cmd_tre->ptr = MHI_TRE_CMD_STOP_PTR;
		cmd_tre->dword[0] = MHI_TRE_CMD_STOP_DWORD0;
		cmd_tre->dword[1] = MHI_TRE_CMD_STOP_DWORD1(chan);
		break;
	case MHI_CMD_START_CHAN:
		cmd_tre->ptr = MHI_TRE_CMD_START_PTR;
		cmd_tre->dword[0] = MHI_TRE_CMD_START_DWORD0;
		cmd_tre->dword[1] = MHI_TRE_CMD_START_DWORD1(chan);
		break;
	case MHI_CMD_SFR_CFG:
		mhi_misc_cmd_configure(mhi_cntrl, MHI_CMD_SFR_CFG,
				       &cmd_tre->ptr, &cmd_tre->dword[0],
				       &cmd_tre->dword[1]);
		break;
	default:
		MHI_ERR("Command not supported\n");
		break;
	}

	/* queue to hardware */
	mhi_add_ring_element(mhi_cntrl, ring);
	read_lock_bh(&mhi_cntrl->pm_lock);
	if (likely(MHI_DB_ACCESS_VALID(mhi_cntrl)))
		mhi_ring_cmd_db(mhi_cntrl, mhi_cmd);
	read_unlock_bh(&mhi_cntrl->pm_lock);
	spin_unlock_bh(&mhi_cmd->lock);

	return 0;
}

static int mhi_update_channel_state(struct mhi_controller *mhi_cntrl,
				    struct mhi_chan *mhi_chan,
				    enum mhi_ch_state_type to_state)
{
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	enum mhi_cmd_type cmd = MHI_CMD_NOP;
	int ret = -EIO;

	MHI_VERB("Updating channel %s(%d) state to: %s\n", mhi_chan->name,
		mhi_chan->chan, TO_CH_STATE_TYPE_STR(to_state));

	switch (to_state) {
	case MHI_CH_STATE_TYPE_RESET:
		write_lock_irq(&mhi_chan->lock);
		if (mhi_chan->ch_state != MHI_CH_STATE_STOP &&
		    mhi_chan->ch_state != MHI_CH_STATE_ENABLED &&
		    mhi_chan->ch_state != MHI_CH_STATE_SUSPENDED) {
			write_unlock_irq(&mhi_chan->lock);
			goto exit_invalid_state;
		}
		mhi_chan->ch_state = MHI_CH_STATE_DISABLED;
		write_unlock_irq(&mhi_chan->lock);

		cmd = MHI_CMD_RESET_CHAN;
		break;
	case MHI_CH_STATE_TYPE_STOP:
		if (mhi_chan->ch_state != MHI_CH_STATE_ENABLED)
			goto exit_invalid_state;

		cmd = MHI_CMD_STOP_CHAN;
		break;
	case MHI_CH_STATE_TYPE_START:
		if (mhi_chan->ch_state != MHI_CH_STATE_STOP &&
		    mhi_chan->ch_state != MHI_CH_STATE_DISABLED)
			goto exit_invalid_state;

		cmd = MHI_CMD_START_CHAN;
		break;
	default:
		goto exit_invalid_state;
	}

	/* bring host and device out of suspended states */
	ret = mhi_device_get_sync(mhi_cntrl->mhi_dev);
	if (ret)
		return ret;
	mhi_cntrl->runtime_get(mhi_cntrl);

	reinit_completion(&mhi_chan->completion);
	ret = mhi_send_cmd(mhi_cntrl, mhi_chan, cmd);
	if (ret) {
		MHI_ERR("Failed to send %s(%d) %s command\n",
			mhi_chan->name, mhi_chan->chan,
			TO_CH_STATE_TYPE_STR(to_state));
		goto exit_command_failure;
	}

	ret = wait_for_completion_timeout(&mhi_chan->completion,
				       msecs_to_jiffies(mhi_cntrl->timeout_ms));
	if (!ret || mhi_chan->ccs != MHI_EV_CC_SUCCESS) {
		MHI_ERR("Failed to receive %s(%d) %s command completion\n",
			mhi_chan->name, mhi_chan->chan,
			TO_CH_STATE_TYPE_STR(to_state));
		ret = -EIO;
		goto exit_command_failure;
	}

	ret = 0;

	if (to_state != MHI_CH_STATE_TYPE_RESET) {
		write_lock_irq(&mhi_chan->lock);
		mhi_chan->ch_state = (to_state == MHI_CH_STATE_TYPE_START) ?
				      MHI_CH_STATE_ENABLED : MHI_CH_STATE_STOP;
		write_unlock_irq(&mhi_chan->lock);
	}

	MHI_VERB("Channel %s(%d) state change to %s successful\n",
		mhi_chan->name, mhi_chan->chan, TO_CH_STATE_TYPE_STR(to_state));

exit_command_failure:
	mhi_cntrl->runtime_put(mhi_cntrl);
	mhi_device_put(mhi_cntrl->mhi_dev);

	return ret;

exit_invalid_state:
	MHI_ERR("Channel %s(%d) update to %s not allowed\n",
		mhi_chan->name, mhi_chan->chan, TO_CH_STATE_TYPE_STR(to_state));

	return -EINVAL;
}

static void mhi_unprepare_channel(struct mhi_controller *mhi_cntrl,
				  struct mhi_chan *mhi_chan)
{
	int ret;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;

	/* no more processing events for this channel */
	mutex_lock(&mhi_chan->mutex);

	if (!(BIT(mhi_cntrl->ee) & mhi_chan->ee_mask)) {
		MHI_ERR(
			"Current EE: %s Required EE Mask: 0x%x for chan: %s\n",
			TO_MHI_EXEC_STR(mhi_cntrl->ee), mhi_chan->ee_mask,
			mhi_chan->name);
		goto exit_unprepare_channel;
	}

	ret = mhi_update_channel_state(mhi_cntrl, mhi_chan,
				       MHI_CH_STATE_TYPE_RESET);
	if (ret)
		MHI_ERR("Failed to reset channel, still resetting\n");

exit_unprepare_channel:
	write_lock_irq(&mhi_chan->lock);
	mhi_chan->ch_state = MHI_CH_STATE_DISABLED;
	write_unlock_irq(&mhi_chan->lock);

	if (!mhi_chan->offload_ch) {
		mhi_reset_chan(mhi_cntrl, mhi_chan);
		mhi_deinit_chan_ctxt(mhi_cntrl, mhi_chan);
	}
	MHI_VERB("chan:%d successfully reset\n", mhi_chan->chan);

	mutex_unlock(&mhi_chan->mutex);
}

int mhi_prepare_channel(struct mhi_controller *mhi_cntrl,
			struct mhi_chan *mhi_chan)
{
	int ret = 0;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;

	if (!(BIT(mhi_cntrl->ee) & mhi_chan->ee_mask)) {
		MHI_ERR(
			"Current EE: %s Required EE Mask: 0x%x for chan: %s\n",
			TO_MHI_EXEC_STR(mhi_cntrl->ee), mhi_chan->ee_mask,
			mhi_chan->name);
		return -ENOTCONN;
	}

	mutex_lock(&mhi_chan->mutex);

	/* Check of client manages channel context for offload channels */
	if (!mhi_chan->offload_ch) {
		ret = mhi_init_chan_ctxt(mhi_cntrl, mhi_chan);
		if (ret)
			goto error_init_chan;
	}

	ret = mhi_update_channel_state(mhi_cntrl, mhi_chan,
				       MHI_CH_STATE_TYPE_START);
	if (ret)
		goto error_pm_state;

	/* Pre-allocate buffer for xfer ring */
	if (mhi_chan->pre_alloc) {
		int nr_el = get_nr_avail_ring_elements(mhi_cntrl,
						       &mhi_chan->tre_ring);
		size_t len = mhi_cntrl->buffer_len;

		while (nr_el--) {
			void *buf;
			struct mhi_buf_info info = { };
			buf = kmalloc(len, GFP_KERNEL);
			if (!buf) {
				ret = -ENOMEM;
				goto error_pre_alloc;
			}

			/* Prepare transfer descriptors */
			info.v_addr = buf;
			info.cb_buf = buf;
			info.len = len;
			ret = mhi_gen_tre(mhi_cntrl, mhi_chan, &info, MHI_EOT);
			if (ret) {
				kfree(buf);
				goto error_pre_alloc;
			}
		}

		read_lock_bh(&mhi_cntrl->pm_lock);
		if (MHI_DB_ACCESS_VALID(mhi_cntrl)) {
			read_lock_irq(&mhi_chan->lock);
			mhi_ring_chan_db(mhi_cntrl, mhi_chan);
			read_unlock_irq(&mhi_chan->lock);
		}
		read_unlock_bh(&mhi_cntrl->pm_lock);
	}

	mutex_unlock(&mhi_chan->mutex);

	return 0;

error_pm_state:
	if (!mhi_chan->offload_ch)
		mhi_deinit_chan_ctxt(mhi_cntrl, mhi_chan);

error_init_chan:
	mutex_unlock(&mhi_chan->mutex);

	return ret;

error_pre_alloc:
	mutex_unlock(&mhi_chan->mutex);
	mhi_unprepare_channel(mhi_cntrl, mhi_chan);

	return ret;
}

static void mhi_mark_stale_events(struct mhi_controller *mhi_cntrl,
				  struct mhi_event *mhi_event,
				  struct mhi_event_ctxt *er_ctxt,
				  int chan)

{
	struct mhi_tre *dev_rp, *local_rp;
	struct mhi_ring *ev_ring;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	unsigned long flags;
	dma_addr_t ptr;

	MHI_VERB("Marking all events for chan: %d as stale\n", chan);

	ev_ring = &mhi_event->ring;

	/* mark all stale events related to channel as STALE event */
	spin_lock_irqsave(&mhi_event->lock, flags);

	ptr = er_ctxt->rp;
	if (!is_valid_ring_ptr(ev_ring, ptr)) {
		MHI_ERR("Event ring rp points outside of the event ring\n");
		dev_rp = ev_ring->rp;
	} else {
		dev_rp = mhi_to_virtual(ev_ring, ptr);
	}

	local_rp = ev_ring->rp;
	while (dev_rp != local_rp) {
		if (MHI_TRE_GET_EV_TYPE(local_rp) == MHI_PKT_TYPE_TX_EVENT &&
		    chan == MHI_TRE_GET_EV_CHID(local_rp))
			local_rp->dword[1] = MHI_TRE_EV_DWORD1(chan,
					MHI_PKT_TYPE_STALE_EVENT);
		local_rp++;
		if (local_rp == (ev_ring->base + ev_ring->len))
			local_rp = ev_ring->base;
	}

	MHI_VERB("Finished marking events as stale events\n");
	spin_unlock_irqrestore(&mhi_event->lock, flags);
}

static void mhi_reset_data_chan(struct mhi_controller *mhi_cntrl,
				struct mhi_chan *mhi_chan)
{
	struct mhi_ring *buf_ring, *tre_ring;
	struct mhi_result result;

	/* Reset any pending buffers */
	buf_ring = &mhi_chan->buf_ring;
	tre_ring = &mhi_chan->tre_ring;
	result.transaction_status = -ENOTCONN;
	result.bytes_xferd = 0;
	while (tre_ring->rp != tre_ring->wp) {
		struct mhi_buf_info *buf_info = buf_ring->rp;

		if (mhi_chan->dir == DMA_TO_DEVICE)
			atomic_dec(&mhi_cntrl->pending_pkts);

		if (!buf_info->pre_mapped)
			mhi_cntrl->unmap_single(mhi_cntrl, buf_info);

		mhi_del_ring_element(mhi_cntrl, buf_ring);
		mhi_del_ring_element(mhi_cntrl, tre_ring);

		if (mhi_chan->pre_alloc) {
			kfree(buf_info->cb_buf);
		} else {
			result.buf_addr = buf_info->cb_buf;
			mhi_chan->xfer_cb(mhi_chan->mhi_dev, &result);
		}
	}
}

void mhi_reset_chan(struct mhi_controller *mhi_cntrl, struct mhi_chan *mhi_chan)
{
	struct mhi_event *mhi_event;
	struct mhi_event_ctxt *er_ctxt;
	int chan = mhi_chan->chan;

	/* Nothing to reset, client doesn't queue buffers */
	if (mhi_chan->offload_ch)
		return;

	read_lock_bh(&mhi_cntrl->pm_lock);
	mhi_event = &mhi_cntrl->mhi_event[mhi_chan->er_index];
	er_ctxt = &mhi_cntrl->mhi_ctxt->er_ctxt[mhi_chan->er_index];

	mhi_mark_stale_events(mhi_cntrl, mhi_event, er_ctxt, chan);

	mhi_reset_data_chan(mhi_cntrl, mhi_chan);

	read_unlock_bh(&mhi_cntrl->pm_lock);
}

/* Move channel to start state */
int mhi_prepare_for_transfer(struct mhi_device *mhi_dev)
{
	int ret, dir;
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct mhi_chan *mhi_chan;

	for (dir = 0; dir < 2; dir++) {
		mhi_chan = dir ? mhi_dev->dl_chan : mhi_dev->ul_chan;
		if (!mhi_chan)
			continue;

		ret = mhi_prepare_channel(mhi_cntrl, mhi_chan);
		if (ret)
			goto error_open_chan;
	}

	return 0;

error_open_chan:
	for (--dir; dir >= 0; dir--) {
		mhi_chan = dir ? mhi_dev->dl_chan : mhi_dev->ul_chan;
		if (!mhi_chan)
			continue;

		mhi_unprepare_channel(mhi_cntrl, mhi_chan);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(mhi_prepare_for_transfer);

void mhi_unprepare_from_transfer(struct mhi_device *mhi_dev)
{
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct mhi_chan *mhi_chan;
	int dir;

	for (dir = 0; dir < 2; dir++) {
		mhi_chan = dir ? mhi_dev->ul_chan : mhi_dev->dl_chan;
		if (!mhi_chan)
			continue;

		mhi_unprepare_channel(mhi_cntrl, mhi_chan);
	}
}
EXPORT_SYMBOL_GPL(mhi_unprepare_from_transfer);

static int mhi_update_transfer_state(struct mhi_device *mhi_dev,
				     enum mhi_ch_state_type to_state)
{
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	struct mhi_chan *mhi_chan;
	struct mhi_chan_ctxt *chan_ctxt;
	int dir, ret;

	for (dir = 0; dir < 2; dir++) {
		mhi_chan = dir ? mhi_dev->ul_chan : mhi_dev->dl_chan;

		if (!mhi_chan)
			continue;

		/*
		 * Bail out if one of the channels fails as client will reset
		 * both upon failure
		 */
		mutex_lock(&mhi_chan->mutex);
		chan_ctxt = &mhi_cntrl->mhi_ctxt->chan_ctxt[mhi_chan->chan];
		if (!(chan_ctxt->chcfg & CHAN_CTX_CHSTATE_MASK)) {
			mutex_unlock(&mhi_chan->mutex);
			MHI_ERR("Channel %s(%u) context not initialized\n",
				mhi_chan->name, mhi_chan->chan);
			return -EINVAL;
		}
		ret = mhi_update_channel_state(mhi_cntrl, mhi_chan, to_state);
		if (ret) {
			mutex_unlock(&mhi_chan->mutex);
			return ret;
		}
		mutex_unlock(&mhi_chan->mutex);
	}

	return 0;
}

int mhi_stop_transfer(struct mhi_device *mhi_dev)
{
	return mhi_update_transfer_state(mhi_dev, MHI_CH_STATE_TYPE_STOP);
}
EXPORT_SYMBOL(mhi_stop_transfer);

int mhi_start_transfer(struct mhi_device *mhi_dev)
{
	return mhi_update_transfer_state(mhi_dev, MHI_CH_STATE_TYPE_START);
}
EXPORT_SYMBOL(mhi_start_transfer);

int mhi_poll(struct mhi_device *mhi_dev, u32 budget)
{
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct mhi_chan *mhi_chan = mhi_dev->dl_chan;
	struct mhi_event *mhi_event = &mhi_cntrl->mhi_event[mhi_chan->er_index];
	int ret;

	spin_lock_bh(&mhi_event->lock);
	ret = mhi_event->process_event(mhi_cntrl, mhi_event, budget);
	spin_unlock_bh(&mhi_event->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mhi_poll);
