
/*
 * CAAM Secure Memory Storage Interface
 * Copyright (c) 2008, 2012 Freescale Semiconductor, Inc.
 *
 * Loosely based on the SHW Keystore API for SCC/SCC2
 * Experimental implementation and NOT intended for upstream use. Expect
 * this interface to be amended significantly in the future once it becomes
 * integrated into live applications.
 *
 * Known issues:
 *
 * - Executes one instance of an secure memory "driver". This is tied to the
 *   fact that job rings can't run as standalone instances in the present
 *   configuration.
 *
 * - It does not expose a userspace interface. The value of a userspace
 *   interface for access to secrets is a point for further architectural
 *   discussion.
 *
 * - Partition/permission management is not part of this interface. It
 *   depends on some level of "knowledge" agreed upon between bootloader,
 *   provisioning applications, and OS-hosted software (which uses this
 *   driver).
 *
 * - No means of identifying the location or purpose of secrets managed by
 *   this interface exists; "slot location" and format of a given secret
 *   needs to be agreed upon between bootloader, provisioner, and OS-hosted
 *   application.
 */

#include "compat.h"
#include "regs.h"
#include "jr.h"
#include "desc.h"
#include "intern.h"
#include "error.h"
#include "sm.h"


#ifdef SM_DEBUG_CONT
void sm_show_page(struct device *dev, struct sm_page_descriptor *pgdesc)
{
	struct caam_drv_private_sm *smpriv = dev_get_drvdata(dev);
	u32 i, *smdata;

	dev_info(dev, "physical page %d content at 0x%08x\n",
		 pgdesc->phys_pagenum, pgdesc->pg_base);
	smdata = pgdesc->pg_base;
	for (i = 0; i < (smpriv->page_size / sizeof(u32)); i += 4)
		dev_info(dev, "[0x%08x] 0x%08x 0x%08x 0x%08x 0x%08x\n",
			 (u32)&smdata[i], smdata[i], smdata[i+1], smdata[i+2],
			 smdata[i+3]);
}
#endif

/*
 * Construct a secure memory blob encapsulation job descriptor
 *
 * - desc	pointer to hold new (to be allocated) pointer to the generated
 *		descriptor for later use. Calling thread can kfree the
 *		descriptor after execution.
 * - keymod	Physical pointer to key modifier (contiguous piece).
 * - keymodsz	Size of key modifier in bytes (should normally be 8).
 * - secretbuf	Physical pointer (within an accessible secure memory page)
 *		of the secret to be encapsulated.
 * - outbuf	Physical pointer (within an accessible secure memory page)
 *		of the encapsulated output. This will be larger than the
 *		input secret because of the added encapsulation data.
 * - secretsz	Size of input secret, in bytes.
 * - auth	If nonzero, use AES-CCM for encapsulation, else use ECB
 *
 * Note: this uses 32-bit pointers at present
 */
#define INITIAL_DESCSZ 16	/* size of tmp buffer for descriptor const. */
static int blob_encap_desc(u32 **desc, dma_addr_t keymod, u16 keymodsz,
			   dma_addr_t secretbuf, dma_addr_t outbuf,
			   u16 secretsz, bool auth)
{
	u32 *tdesc, tmpdesc[INITIAL_DESCSZ];
	u16 dsize, idx;

	memset(tmpdesc, 0, INITIAL_DESCSZ * sizeof(u32));
	idx = 1;

	/* Load key modifier */
	tmpdesc[idx++] = CMD_LOAD | LDST_CLASS_2_CCB | LDST_SRCDST_BYTE_KEY |
			 ((12 << LDST_OFFSET_SHIFT) & LDST_OFFSET_MASK) |
			 (keymodsz & LDST_LEN_MASK);

	tmpdesc[idx++] = (u32)keymod;

	/* Encapsulate to secure memory */
	tmpdesc[idx++] = CMD_SEQ_IN_PTR | secretsz;
	tmpdesc[idx++] = (u32)secretbuf;

	/* Add space for BKEK and MAC tag */
	tmpdesc[idx++] = CMD_SEQ_IN_PTR | (secretsz + (32 + 16));

	tmpdesc[idx++] = (u32)outbuf;
	tmpdesc[idx] = CMD_OPERATION | OP_TYPE_ENCAP_PROTOCOL | OP_PCLID_BLOB |
		     OP_PCL_BLOB_PTXT_SECMEM;
	if (auth)
		tmpdesc[idx] |= OP_PCL_BLOB_EKT;

	idx++;
	tmpdesc[0] = CMD_DESC_HDR | HDR_ONE | (idx & HDR_DESCLEN_MASK);
	dsize = idx * sizeof(u32);

	tdesc = kmalloc(dsize, GFP_KERNEL | GFP_DMA);
	if (tdesc == NULL)
		return 0;

	memcpy(tdesc, tmpdesc, dsize);
	*desc = tdesc;
	return dsize;
}

/*
 * Construct a secure memory blob decapsulation job descriptor
 *
 * - desc	pointer to hold new (to be allocated) pointer to the generated
 *		descriptor for later use. Calling thread can kfree the
 *		descriptor after execution.
 * - keymod	Physical pointer to key modifier (contiguous piece).
 * - keymodsz	Size of key modifier in bytes (should normally be 16).
 * - blobbuf	Physical pointer (within an accessible secure memory page)
 *		of the blob to be decapsulated.
 * - outbuf	Physical pointer (within an accessible secure memory page)
 *		of the decapsulated output.
 * - secretsz	Size of input blob, in bytes.
 * - auth	If nonzero, assume AES-CCM for decapsulation, else use ECB
 *
 * Note: this uses 32-bit pointers at present
 */
static int blob_decap_desc(u32 **desc, dma_addr_t keymod, u16 keymodsz,
			   dma_addr_t blobbuf, dma_addr_t outbuf,
			   u16 blobsz, bool auth)
{
	u32 *tdesc, tmpdesc[INITIAL_DESCSZ];
	u16 dsize, idx;

	memset(tmpdesc, 0, INITIAL_DESCSZ * sizeof(u32));
	idx = 1;

	/* Load key modifier */
	tmpdesc[idx++] = CMD_LOAD | LDST_CLASS_2_CCB | LDST_SRCDST_BYTE_KEY |
			 ((12 << LDST_OFFSET_SHIFT) & LDST_OFFSET_MASK) |
			 (keymodsz & LDST_LEN_MASK);

	tmpdesc[idx++] = (u32)keymod;

	/* Compensate BKEK + MAC tag */
	tmpdesc[idx++] = CMD_SEQ_IN_PTR | (blobsz + 32 + 16);

	tmpdesc[idx++] = (u32)blobbuf;
	tmpdesc[idx++] = CMD_SEQ_OUT_PTR | blobsz;
	tmpdesc[idx++] = (u32)outbuf;

	/* Decapsulate from secure memory partition to black blob */
	tmpdesc[idx] = CMD_OPERATION | OP_TYPE_DECAP_PROTOCOL | OP_PCLID_BLOB |
		     OP_PCL_BLOB_PTXT_SECMEM | OP_PCL_BLOB_BLACK;
	if (auth)
		tmpdesc[idx] |= OP_PCL_BLOB_EKT;

	idx++;
	tmpdesc[0] = CMD_DESC_HDR | HDR_ONE | (idx & HDR_DESCLEN_MASK);
	dsize = idx * sizeof(u32);

	tdesc = kmalloc(dsize, GFP_KERNEL | GFP_DMA);
	if (tdesc == NULL)
		return 0;

	memcpy(tdesc, tmpdesc, dsize);
	*desc = tdesc;
	return dsize;
}

/*
 * Pseudo-synchronous ring access functions for carrying out key
 * encapsulation and decapsulation
 */

struct sm_key_job_result {
	int error;
	struct completion completion;
};

void sm_key_job_done(struct device *dev, u32 *desc, u32 err, void *context)
{
	struct sm_key_job_result *res = context;

	res->error = err;	/* save off the error for postprocessing */
	complete(&res->completion);	/* mark us complete */
}

static int sm_key_job(struct device *ksdev, u32 *jobdesc)
{
	struct sm_key_job_result testres;
	struct caam_drv_private_sm *kspriv;
	int rtn = 0;

	kspriv = dev_get_drvdata(ksdev);

	init_completion(&testres.completion);

	rtn = caam_jr_enqueue(kspriv->smringdev, jobdesc, sm_key_job_done,
			      &testres);
	if (!rtn) {
		wait_for_completion_interruptible(&testres.completion);
		rtn = testres.error;
	}
	return rtn;
}

/*
 * Following section establishes the default methods for keystore access
 * They are NOT intended for use external to this module
 *
 * In the present version, these are the only means for the higher-level
 * interface to deal with the mechanics of accessing the phyiscal keystore
 */


int slot_alloc(struct device *dev, u32 unit, u32 size, u32 *slot)
{
	struct caam_drv_private_sm *smpriv = dev_get_drvdata(dev);
	struct keystore_data *ksdata = smpriv->pagedesc[unit].ksdata;
	u32 i;

#ifdef SM_DEBUG
	dev_info(dev, "slot_alloc(): requesting slot for %d bytes\n", size);
#endif

	if (size > smpriv->slot_size)
		return -EKEYREJECTED;

	for (i = 0; i < ksdata->slot_count; i++) {
		if (ksdata->slot[i].allocated == 0) {
			ksdata->slot[i].allocated = 1;
			(*slot) = i;
#ifdef SM_DEBUG
			dev_info(dev, "slot_alloc(): new slot %d allocated\n",
				 *slot);
#endif
			return 0;
		}
	}

	return -ENOSPC;
}

int slot_dealloc(struct device *dev, u32 unit, u32 slot)
{
	struct caam_drv_private_sm *smpriv = dev_get_drvdata(dev);
	struct keystore_data *ksdata = smpriv->pagedesc[unit].ksdata;
	u8 __iomem *slotdata;

#ifdef SM_DEBUG
	dev_info(dev, "slot_dealloc(): releasing slot %d\n", slot);
#endif
	if (slot >= ksdata->slot_count)
		return -EINVAL;
	slotdata = ksdata->base_address + slot * smpriv->slot_size;

	if (ksdata->slot[slot].allocated == 1) {
		/* Forcibly overwrite the data from the keystore */
		memset(ksdata->base_address + slot * smpriv->slot_size, 0,
		       smpriv->slot_size);

		ksdata->slot[slot].allocated = 0;
#ifdef SM_DEBUG
		dev_info(dev, "slot_dealloc(): slot %d released\n", slot);
#endif
		return 0;
	}

	return -EINVAL;
}

void *slot_get_address(struct device *dev, u32 unit, u32 slot)
{
	struct caam_drv_private_sm *smpriv = dev_get_drvdata(dev);
	struct keystore_data *ksdata = smpriv->pagedesc[unit].ksdata;

	if (slot >= ksdata->slot_count)
		return NULL;

#ifdef SM_DEBUG
	dev_info(dev, "slot_get_address(): slot %d is 0x%08x\n", slot,
		 (u32)ksdata->base_address + slot * smpriv->slot_size);
#endif

	return ksdata->base_address + slot * smpriv->slot_size;
}

u32 slot_get_base(struct device *dev, u32 unit, u32 slot)
{
	struct caam_drv_private_sm *smpriv = dev_get_drvdata(dev);
	struct keystore_data *ksdata = smpriv->pagedesc[unit].ksdata;

	/*
	 * There could potentially be more than one secure partition object
	 * associated with this keystore.  For now, there is just one.
	 */

	(void)slot;

#ifdef SM_DEBUG
	dev_info(dev, "slot_get_base(): slot %d = 0x%08x\n",
		slot, (u32)ksdata->base_address);
#endif

	return (u32)(ksdata->base_address);
}

u32 slot_get_offset(struct device *dev, u32 unit, u32 slot)
{
	struct caam_drv_private_sm *smpriv = dev_get_drvdata(dev);
	struct keystore_data *ksdata = smpriv->pagedesc[unit].ksdata;

	if (slot >= ksdata->slot_count)
		return -EINVAL;

#ifdef SM_DEBUG
	dev_info(dev, "slot_get_offset(): slot %d = %d\n", slot,
		slot * smpriv->slot_size);
#endif

	return slot * smpriv->slot_size;
}

u32 slot_get_slot_size(struct device *dev, u32 unit, u32 slot)
{
	struct caam_drv_private_sm *smpriv = dev_get_drvdata(dev);


#ifdef SM_DEBUG
	dev_info(dev, "slot_get_slot_size(): slot %d = %d\n", slot,
		 smpriv->slot_size);
#endif
	/* All slots are the same size in the default implementation */
	return smpriv->slot_size;
}



int kso_init_data(struct device *dev, u32 unit)
{
	struct caam_drv_private_sm *smpriv = dev_get_drvdata(dev);
	int retval = -EINVAL;
	struct keystore_data *keystore_data = NULL;
	u32 slot_count;
	u32 keystore_data_size;

	/*
	 * Calculate the required size of the keystore data structure, based
	 * on the number of keys that can fit in the partition.
	 */
	slot_count = smpriv->page_size / smpriv->slot_size;
#ifdef SM_DEBUG
	dev_info(dev, "kso_init_data: %d slots initializing\n", slot_count);
#endif

	keystore_data_size = sizeof(struct keystore_data) +
				slot_count *
				sizeof(struct keystore_data_slot_info);

	keystore_data = kzalloc(keystore_data_size, GFP_KERNEL);

	if (keystore_data == NULL) {
		retval = -ENOSPC;
		goto out;
	}

#ifdef SM_DEBUG
	dev_info(dev, "kso_init_data: keystore data size = %d\n",
		 keystore_data_size);
#endif

	/*
	 * Place the slot information structure directly after the keystore data
	 * structure.
	 */
	keystore_data->slot = (struct keystore_data_slot_info *)
			      (keystore_data + 1);
	keystore_data->slot_count = slot_count;

	smpriv->pagedesc[unit].ksdata = keystore_data;
	smpriv->pagedesc[unit].ksdata->base_address =
		smpriv->pagedesc[unit].pg_base;

	retval = 0;

out:
	if (retval != 0)
		if (keystore_data != NULL)
			kfree(keystore_data);


	return retval;
}

void kso_cleanup_data(struct device *dev, u32 unit)
{
	struct caam_drv_private_sm *smpriv = dev_get_drvdata(dev);
	struct keystore_data *keystore_data = NULL;

	if (smpriv->pagedesc[unit].ksdata != NULL)
		keystore_data = smpriv->pagedesc[unit].ksdata;

	/* Release the allocated keystore management data */
	kfree(smpriv->pagedesc[unit].ksdata);

	return;
}



/*
 * Keystore management section
 */

void sm_init_keystore(struct device *dev)
{
	struct caam_drv_private_sm *smpriv = dev_get_drvdata(dev);

	smpriv->data_init = kso_init_data;
	smpriv->data_cleanup = kso_cleanup_data;
	smpriv->slot_alloc = slot_alloc;
	smpriv->slot_dealloc = slot_dealloc;
	smpriv->slot_get_address = slot_get_address;
	smpriv->slot_get_base = slot_get_base;
	smpriv->slot_get_offset = slot_get_offset;
	smpriv->slot_get_slot_size = slot_get_slot_size;
#ifdef SM_DEBUG
	dev_info(dev, "sm_init_keystore(): handlers installed\n");
#endif
}
EXPORT_SYMBOL(sm_init_keystore);

/* Return available pages/units */
u32 sm_detect_keystore_units(struct device *dev)
{
	struct caam_drv_private_sm *smpriv = dev_get_drvdata(dev);

	return smpriv->localpages;
}
EXPORT_SYMBOL(sm_detect_keystore_units);

/*
 * Do any keystore specific initializations
 */
int sm_establish_keystore(struct device *dev, u32 unit)
{
	struct caam_drv_private_sm *smpriv = dev_get_drvdata(dev);

#ifdef SM_DEBUG
	dev_info(dev, "sm_establish_keystore(): unit %d initializing\n", unit);
#endif

	if (smpriv->data_init == NULL)
		return -EINVAL;

	/* Call the data_init function for any user setup */
	return smpriv->data_init(dev, unit);
}
EXPORT_SYMBOL(sm_establish_keystore);

void sm_release_keystore(struct device *dev, u32 unit)
{
	struct caam_drv_private_sm *smpriv = dev_get_drvdata(dev);

#ifdef SM_DEBUG
	dev_info(dev, "sm_establish_keystore(): unit %d releasing\n", unit);
#endif
	if ((smpriv != NULL) && (smpriv->data_cleanup != NULL))
		smpriv->data_cleanup(dev, unit);

	return;
}
EXPORT_SYMBOL(sm_release_keystore);

/*
 * Subsequent interfacce (sm_keystore_*) forms the accessor interfacce to
 * the keystore
 */
int sm_keystore_slot_alloc(struct device *dev, u32 unit, u32 size, u32 *slot)
{
	struct caam_drv_private_sm *smpriv = dev_get_drvdata(dev);
	int retval = -EINVAL;

	spin_lock(&smpriv->kslock);

	if ((smpriv->slot_alloc == NULL) ||
	    (smpriv->pagedesc[unit].ksdata == NULL))
		goto out;

	retval =  smpriv->slot_alloc(dev, unit, size, slot);

out:
	spin_unlock(&smpriv->kslock);
	return retval;
}
EXPORT_SYMBOL(sm_keystore_slot_alloc);

int sm_keystore_slot_dealloc(struct device *dev, u32 unit, u32 slot)
{
	struct caam_drv_private_sm *smpriv = dev_get_drvdata(dev);
	int retval = -EINVAL;

	spin_lock(&smpriv->kslock);

	if ((smpriv->slot_alloc == NULL) ||
	    (smpriv->pagedesc[unit].ksdata == NULL))
		goto out;

	retval = smpriv->slot_dealloc(dev, unit, slot);
out:
	spin_unlock(&smpriv->kslock);
	return retval;
}
EXPORT_SYMBOL(sm_keystore_slot_dealloc);

int sm_keystore_slot_load(struct device *dev, u32 unit, u32 slot,
			  const u8 *key_data, u32 key_length)
{
	struct caam_drv_private_sm *smpriv = dev_get_drvdata(dev);
	int retval = -EINVAL;
	u32 slot_size;
	u32 i;
	u8 __iomem *slot_location;

	spin_lock(&smpriv->kslock);

	slot_size = smpriv->slot_get_slot_size(dev, unit, slot);

	if (key_length > slot_size) {
		retval = -EFBIG;
		goto out;
	}

	slot_location = smpriv->slot_get_address(dev, unit, slot);

	for (i = 0; i < key_length; i++)
		slot_location[i] = key_data[i];

	retval = 0;

out:
	spin_unlock(&smpriv->kslock);
	return retval;
}
EXPORT_SYMBOL(sm_keystore_slot_load);

int sm_keystore_slot_read(struct device *dev, u32 unit, u32 slot,
			  u32 key_length, u8 *key_data)
{
	struct caam_drv_private_sm *smpriv = dev_get_drvdata(dev);
	int retval = -EINVAL;
	u8 __iomem *slot_addr;
	u32 slot_size;

	spin_lock(&smpriv->kslock);

	slot_addr = smpriv->slot_get_address(dev, unit, slot);
	slot_size = smpriv->slot_get_slot_size(dev, unit, slot);

	if (key_length > slot_size) {
		retval = -EKEYREJECTED;
		goto out;
	}

	memcpy(key_data, slot_addr, key_length);
	retval = 0;

out:
	spin_unlock(&smpriv->kslock);
	return retval;
}
EXPORT_SYMBOL(sm_keystore_slot_read);

int sm_keystore_slot_encapsulate(struct device *dev, u32 unit, u32 inslot,
				 u32 outslot, u16 secretlen, u8 *keymod,
				 u16 keymodlen)
{
	struct caam_drv_private_sm *smpriv = dev_get_drvdata(dev);
	int retval = 0;
	u32 slot_length, dsize, jstat;
	u32 __iomem *encapdesc = NULL;
	u8 __iomem *lkeymod, *inpslotaddr, *outslotaddr;
	dma_addr_t keymod_dma;

	/* Ensure that the full blob  will fit in the key slot */
	slot_length = smpriv->slot_get_slot_size(dev, unit, outslot);
	if ((secretlen + 48) > slot_length)
		goto out;

	/* Get the base addresses of both keystore slots */
	inpslotaddr = (u8 *)smpriv->slot_get_address(dev, unit, inslot);
	outslotaddr = (u8 *)smpriv->slot_get_address(dev, unit, outslot);

	/* Build the key modifier */
	lkeymod = kmalloc(keymodlen, GFP_KERNEL | GFP_DMA);
	memcpy(lkeymod, keymod, keymodlen);
	keymod_dma = dma_map_single(dev, lkeymod, keymodlen, DMA_TO_DEVICE);
	dma_sync_single_for_device(dev, keymod_dma, keymodlen, DMA_TO_DEVICE);

	/* Build the encapsulation job descriptor */
	dsize = blob_encap_desc(&encapdesc, keymod_dma, keymodlen,
				__pa(inpslotaddr), __pa(outslotaddr),
				secretlen, 0);
	if (!dsize) {
		dev_err(dev, "can't alloc an encap descriptor\n");
		retval = -ENOMEM;
		goto out;
	}
	jstat = sm_key_job(dev, encapdesc);

	dma_unmap_single(dev, keymod_dma, keymodlen, DMA_TO_DEVICE);
	kfree(encapdesc);

out:
	return retval;

}
EXPORT_SYMBOL(sm_keystore_slot_encapsulate);

int sm_keystore_slot_decapsulate(struct device *dev, u32 unit, u32 inslot,
				 u32 outslot, u16 secretlen, u8 *keymod,
				 u16 keymodlen)
{
	struct caam_drv_private_sm *smpriv = dev_get_drvdata(dev);
	int retval = 0;
	u32 slot_length, dsize, jstat;
	u32 __iomem *decapdesc = NULL;
	u8 __iomem *lkeymod, *inpslotaddr, *outslotaddr;
	dma_addr_t keymod_dma;

	/* Ensure that the decap data will fit in the key slot */
	slot_length = smpriv->slot_get_slot_size(dev, unit, outslot);
	if (secretlen > slot_length)
		goto out;

	/* Get the base addresses of both keystore slots */
	inpslotaddr = (u8 *)smpriv->slot_get_address(dev, unit, inslot);
	outslotaddr = (u8 *)smpriv->slot_get_address(dev, unit, outslot);

	/* Build the key modifier */
	lkeymod = kmalloc(keymodlen, GFP_KERNEL | GFP_DMA);
	memcpy(lkeymod, keymod, keymodlen);
	keymod_dma = dma_map_single(dev, lkeymod, keymodlen, DMA_TO_DEVICE);
	dma_sync_single_for_device(dev, keymod_dma, keymodlen, DMA_TO_DEVICE);

	/* Build the decapsulation job descriptor */
	dsize = blob_decap_desc(&decapdesc, keymod_dma, keymodlen,
				__pa(inpslotaddr), __pa(outslotaddr),
				secretlen, 0);
	if (!dsize) {
		dev_err(dev, "can't alloc a decap descriptor\n");
		retval = -ENOMEM;
		goto out;
	}
	jstat = sm_key_job(dev, decapdesc);

	dma_unmap_single(dev, keymod_dma, keymodlen, DMA_TO_DEVICE);
	kfree(decapdesc);

out:
	return retval;

}
EXPORT_SYMBOL(sm_keystore_slot_decapsulate);


/*
 * Initialization/shutdown subsystem
 * Assumes statically-invoked startup/shutdown from the controller driver
 * for the present time, to be reworked when a device tree becomes
 * available. This code will not modularize in present form.
 *
 * Also, simply uses ring 0 for execution at the present
 */

int caam_sm_startup(struct platform_device *pdev)
{
	struct device *ctrldev, *smdev;
	struct caam_drv_private *ctrlpriv;
	struct caam_drv_private_sm *smpriv;
	struct caam_drv_private_jr *jrpriv;	/* need this for reg page */
	struct platform_device *sm_pdev;
	struct sm_page_descriptor *lpagedesc;
	u32 page, pgstat, lpagect, detectedpage;

	ctrldev = &pdev->dev;
	ctrlpriv = dev_get_drvdata(ctrldev);

	/*
	 * Set up the private block for secure memory
	 * Only one instance is possible
	 */
	smpriv = kzalloc(sizeof(struct caam_drv_private_sm), GFP_KERNEL);
	if (smpriv == NULL) {
		dev_err(ctrldev, "can't alloc private mem for secure memory\n");
		return -ENOMEM;
	}
	smpriv->parentdev = ctrldev; /* copy of parent dev is handy */

	/* Create the dev */
#ifdef CONFIG_OF
	sm_pdev = of_platform_device_create(np, NULL, ctrldev);
#else
	sm_pdev = platform_device_register_data(ctrldev, "caam_sm", 0,
						smpriv,
					sizeof(struct caam_drv_private_sm));
#endif
	if (sm_pdev == NULL) {
		kfree(smpriv);
		return -EINVAL;
	}
	smdev = &sm_pdev->dev;
	dev_set_drvdata(smdev, smpriv);
	ctrlpriv->smdev = smdev;

	/*
	 * Collect configuration limit data for reference
	 * This batch comes from the partition data/vid registers in perfmon
	 */
	smpriv->max_pages = ((rd_reg32(&ctrlpriv->ctrl->perfmon.smpart)
			    & SMPART_MAX_NUMPG_MASK) >>
			    SMPART_MAX_NUMPG_SHIFT) + 1;
	smpriv->top_partition = ((rd_reg32(&ctrlpriv->ctrl->perfmon.smpart)
				& SMPART_MAX_PNUM_MASK) >>
				SMPART_MAX_PNUM_SHIFT) + 1;
	smpriv->top_page =  ((rd_reg32(&ctrlpriv->ctrl->perfmon.smpart)
			    & SMPART_MAX_PG_MASK) >> SMPART_MAX_PG_SHIFT) + 1;
	smpriv->page_size = 1024 << ((rd_reg32(&ctrlpriv->ctrl->perfmon.smvid)
			    & SMVID_PG_SIZE_MASK) >> SMVID_PG_SIZE_SHIFT);
	smpriv->slot_size = 1 << CONFIG_CRYPTO_DEV_FSL_CAAM_SM_SLOTSIZE;

#ifdef SM_DEBUG
	dev_info(smdev, "max pages = %d, top partition = %d\n",
			smpriv->max_pages, smpriv->top_partition);
	dev_info(smdev, "top page = %d, page size = %d (total = %d)\n",
			smpriv->top_page, smpriv->page_size,
			smpriv->top_page * smpriv->page_size);
	dev_info(smdev, "selected slot size = %d\n", smpriv->slot_size);
#endif

	/*
	 * Now probe for partitions/pages to which we have access. Note that
	 * these have likely been set up by a bootloader or platform
	 * provisioning application, so we have to assume that we "inherit"
	 * a configuration and work within the constraints of what it might be.
	 *
	 * Assume use of the zeroth ring in the present iteration (until
	 * we can divorce the controller and ring drivers, and then assign
	 * an SM instance to any ring instance).
	 */
	smpriv->smringdev = ctrlpriv->jrdev[0];
	jrpriv = dev_get_drvdata(smpriv->smringdev);
	lpagect = 0;
	lpagedesc = kzalloc(sizeof(struct sm_page_descriptor)
			    * smpriv->max_pages, GFP_KERNEL);
	if (lpagedesc == NULL) {
		kfree(smpriv);
		return -ENOMEM;
	}

	for (page = 0; page < smpriv->max_pages; page++) {
		wr_reg32(&jrpriv->rregs->sm_cmd,
			 ((page << SMC_PAGE_SHIFT) & SMC_PAGE_MASK) |
			 (SMC_CMD_PAGE_INQUIRY & SMC_CMD_MASK));
		pgstat = rd_reg32(&jrpriv->rregs->sm_status);
		if (((pgstat & SMCS_PGWON_MASK) >> SMCS_PGOWN_SHIFT)
		    == SMCS_PGOWN_OWNED) { /* our page? */
			lpagedesc[page].phys_pagenum =
				(pgstat & SMCS_PAGE_MASK) >> SMCS_PAGE_SHIFT;
			lpagedesc[page].own_part =
				(pgstat & SMCS_PART_SHIFT) >> SMCS_PART_MASK;
			lpagedesc[page].pg_base = ctrlpriv->sm_base +
				((smpriv->page_size * page) / sizeof(u32));
			lpagect++;
#ifdef SM_DEBUG
			dev_info(smdev,
				"physical page %d, owning partition = %d\n",
				lpagedesc[page].phys_pagenum,
				lpagedesc[page].own_part);
#endif
		}
	}

	smpriv->pagedesc = kmalloc(sizeof(struct sm_page_descriptor) * lpagect,
				   GFP_KERNEL);
	if (smpriv->pagedesc == NULL) {
		kfree(lpagedesc);
		kfree(smpriv);
		return -ENOMEM;
	}
	smpriv->localpages = lpagect;

	detectedpage = 0;
	for (page = 0; page < smpriv->max_pages; page++) {
		if (lpagedesc[page].pg_base != NULL) {	/* e.g. live entry */
			memcpy(&smpriv->pagedesc[detectedpage],
			       &lpagedesc[page],
			       sizeof(struct sm_page_descriptor));
#ifdef SM_DEBUG_CONT
			sm_show_page(smdev, &smpriv->pagedesc[detectedpage]);
#endif
			detectedpage++;
		}
	}

	kfree(lpagedesc);

	sm_init_keystore(smdev);

	return 0;
}

void caam_sm_shutdown(struct platform_device *pdev)
{
	struct device *ctrldev, *smdev;
	struct caam_drv_private *priv;
	struct caam_drv_private_sm *smpriv;

	ctrldev = &pdev->dev;
	priv = dev_get_drvdata(ctrldev);
	smdev = priv->smdev;
	smpriv = dev_get_drvdata(smdev);

	kfree(smpriv->pagedesc);
	kfree(smpriv);
}

#ifdef CONFIG_OF
static void __exit caam_sm_exit(void)
{
	struct device_node *dev_node;
	struct platform_device *pdev;

	dev_node = of_find_compatible_node(NULL, NULL, "fsl,sec-v4.0");
	if (!dev_node) {
		dev_node = of_find_compatible_node(NULL, NULL, "fsl,sec4.0");
		if (!dev_node)
			return -ENODEV;
	}

	pdev = of_find_device_by_node(dev_node);
	if (!pdev)
		return -ENODEV;

	of_node_put(dev_node);

	caam_sm_shutdown(pdev);
}

static int __init caam_sm_init(void)
{
	struct device_node *dev_node;
	struct platform_device *pdev;

	/*
	 * Do of_find_compatible_node() then of_find_device_by_node()
	 * once a functional device tree is available
	 */
	dev_node = of_find_compatible_node(NULL, NULL, "fsl,sec-v4.0");
	if (!dev_node) {
		dev_node = of_find_compatible_node(NULL, NULL, "fsl,sec4.0");
		if (!dev_node)
			return -ENODEV;
	}

	pdev = of_find_device_by_node(dev_node);
	if (!pdev)
		return -ENODEV;

	of_node_put(dev_node);

	return caam_sm_startup(pdev);
}

module_init(caam_sm_init);
module_exit(caam_sm_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("FSL CAAM Secure Memory / Keystore");
MODULE_AUTHOR("Freescale Semiconductor - NMSG/MAD");
#endif
