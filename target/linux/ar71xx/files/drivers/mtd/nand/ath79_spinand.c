/*
 * Copyright (c) 2015 The Linux Foundation. All rights reserved.
 *
 * Copyright (c) 2003-2013 Broadcom Corporation
 *
 * Copyright (c) 2009-2010 Micron Technology, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/nand.h>
#include <linux/spi/spi.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/mtd/mtd.h>

/* cmd */
#define CMD_READ			0x13
#define CMD_READ_RDM			0x03
#define CMD_PROG_PAGE_LOAD		0x02
#define CMD_PROG_PAGE			0x84
#define CMD_PROG_PAGE_EXC		0x10
#define CMD_ERASE_BLK			0xd8
#define CMD_WR_ENABLE			0x06
#define CMD_WR_DISABLE			0x04
#define CMD_READ_ID			0x9f
#define CMD_RESET			0xff
#define CMD_READ_REG			0x0f
#define CMD_WRITE_REG			0x1f

/* feature/ status reg */
#define REG_BLOCK_LOCK			0xa0
#define REG_OTP				0xb0
#define REG_STATUS			0xc0

/* status */
#define STATUS_OIP_MASK			0x01
#define STATUS_READY			(0 << 0)
#define STATUS_BUSY			(1 << 0)

#define STATUS_E_FAIL_MASK		0x04
#define STATUS_E_FAIL			(1 << 2)

#define STATUS_P_FAIL_MASK		0x08
#define STATUS_P_FAIL			(1 << 3)

#define STATUS_ECC_MASK			0x07
#define STATUS_ECC_ERR_BITS5		0x03
#define STATUS_ECC_ERR_BITS6		0x04
#define STATUS_ECC_ERR_BITS7		0x05
#define STATUS_ECC_ERR_BITS8		0x06
#define STATUS_ECC_ERROR		0x07
#define STATUS2ECC(status) 		(((status) >> 4) & STATUS_ECC_MASK)

/* ECC/OTP enable defines */
#define REG_ECC_MASK			0x10
#define REG_ECC_OFF			(0 << 4)
#define REG_ECC_ON			(1 << 4)

#define REG_OTP_EN			(1 << 6)
#define REG_OTP_PRT			(1 << 7)

/* block lock */
#define BL_ALL_UNLOCKED			0

#define BLOCK_TO_RA(b)			((b) << 6)

#define BUFSIZE				(10 * 64 * 2048)
#define CACHE_BUF			2112

struct ath79_spinand_info {
	bool			init;
	struct spi_device	*spi;
	void			*priv;
};

struct ath79_spinand_state {
	uint32_t	col;
	uint32_t	row;
	int		buf_ptr;
	u8		*buf;
};

struct ath79_spinand_command {
	u8		cmd;
	u32		n_addr;		/* Number of address */
	u8		addr[3];	/* Reg Offset */
	u32		n_dummy;	/* Dummy use */
	u32		n_tx;		/* Number of tx bytes */
	u8		*tx_buf;	/* Tx buf */
	u32		n_rx;		/* Number of rx bytes */
	u8		*rx_buf;	/* Rx buf */
};

static const struct nand_ecclayout ath79_spinand_oob_128 = {
	.eccbytes = 64,
	.eccpos = {
		64, 65, 66, 67, 68, 69, 70, 71,
		72, 73, 74, 75, 76, 77, 78, 79,
		80, 81, 82, 83, 84, 85, 86, 87,
		88, 89, 90, 91, 92, 93, 94, 95,
		96, 97, 98, 99, 100, 101, 102, 103,
		104, 105, 106, 107, 108, 109, 110, 111,
		112, 113, 114, 115, 116, 117, 118, 119,
		120, 121, 122, 123, 124, 125, 126, 127},
	.oobavail = 48,
	.oobfree = { {.offset = 16, .length = 48}, }
};

static u8 badblock_pattern[] = { 0xff, };

static struct nand_bbt_descr ath79_badblock_pattern = {
	.options = 0,
	.offs = 0,
	.len = 1,
	.pattern = badblock_pattern,
};

static inline struct ath79_spinand_state *mtd_to_state(struct mtd_info *mtd)
{
	struct nand_chip *chip = (struct nand_chip *)mtd->priv;
	struct ath79_spinand_info *info = (struct ath79_spinand_info *)chip->priv;
	struct ath79_spinand_state *state = (struct ath79_spinand_state *)info->priv;

	return state;
}

static int ath79_spinand_cmd(struct spi_device *spi, struct ath79_spinand_command *cmd)
{
	struct spi_message message;
	struct spi_transfer x[4];
	u8 dummy = 0xff;

	spi_message_init(&message);
	memset(x, 0, sizeof(x));

	x[0].len = 1;
	x[0].tx_buf = &cmd->cmd;
	spi_message_add_tail(&x[0], &message);

	if (cmd->n_addr) {
		x[1].len = cmd->n_addr;
		x[1].tx_buf = cmd->addr;
		spi_message_add_tail(&x[1], &message);
	}

	if (cmd->n_dummy) {
		x[2].len = cmd->n_dummy;
		x[2].tx_buf = &dummy;
		spi_message_add_tail(&x[2], &message);
	}

	if (cmd->n_tx) {
		x[3].len = cmd->n_tx;
		x[3].tx_buf = cmd->tx_buf;
		spi_message_add_tail(&x[3], &message);
	}

	if (cmd->n_rx) {
		x[3].len = cmd->n_rx;
		x[3].rx_buf = cmd->rx_buf;
		spi_message_add_tail(&x[3], &message);
	}

	return spi_sync(spi, &message);
}

static u8 ath79_spinand_id_map(struct spi_device *spi_nand, u8 device_id)
{
	u8 map_id;

	switch (device_id) {
	case 0xB1:
		/* 128MiB 3,3V 16-bit */
		map_id = 0xC1;
		break;
	case 0xB2:
		/* 256MiB 3,3V 16-bit */
		map_id = 0xCA;
		break;
	case 0xA1:
		/* 128MiB 1,8V 16-bit */
		map_id = 0xB1;
		break;
	case 0xA2:
		/* 256MiB 1,8V 16-bit */
		map_id = 0xBA;
		break;
	default:
		dev_err(&spi_nand->dev, "Unknow device id %02x\n", device_id);
		map_id = device_id;
	}

	pr_info("GD5FxGQ4x device id 0x%02x\n", device_id);

	return map_id;
}

static int ath79_spinand_read_id(struct spi_device *spi_nand, u8 *id)
{
	int retval;
	u8 nand_id[3];
	struct ath79_spinand_command cmd = {0};

	cmd.cmd = CMD_READ_ID;
	cmd.n_rx = 3;
	cmd.rx_buf = &nand_id[0];

	retval = ath79_spinand_cmd(spi_nand, &cmd);
	if (retval < 0) {
		dev_err(&spi_nand->dev, "error %d reading id\n", retval);
		return retval;
	}

	id[0] = nand_id[0];
	id[1] = ath79_spinand_id_map(spi_nand, nand_id[1]);

	return retval;
}

static int ath79_spinand_read_status(struct spi_device *spi_nand, uint8_t *status)
{
	struct ath79_spinand_command cmd = {0};
	int ret;

	cmd.cmd = CMD_READ_REG;
	cmd.n_addr = 1;
	cmd.addr[0] = REG_STATUS;
	cmd.n_rx = 1;
	cmd.rx_buf = status;

	ret = ath79_spinand_cmd(spi_nand, &cmd);
	if (ret < 0)
		dev_err(&spi_nand->dev, "err: %d read status register\n", ret);

	return ret;
}

#define MAX_WAIT_JIFFIES  (120 * HZ)
static int __ath79_wait_till_ready(struct spi_device *spi_nand, u8 *status)
{
	unsigned long deadline;

	deadline = jiffies + MAX_WAIT_JIFFIES;
	do {
		if (ath79_spinand_read_status(spi_nand, status))
			return -1;
		else if ((*status & STATUS_OIP_MASK) == STATUS_READY)
			break;

		cond_resched();
	} while (!time_after_eq(jiffies, deadline));

	return 0;
}

static int ath79_wait_till_ready(struct spi_device *spi_nand)
{
	u8 stat = 0;

	if (__ath79_wait_till_ready(spi_nand, &stat))
		return -1;

	if ((stat & STATUS_OIP_MASK) == STATUS_READY)
		return 0;

	return -1;
}

static int ath79_spinand_get_otp(struct spi_device *spi_nand, u8 *otp)
{
	struct ath79_spinand_command cmd = {0};
	int retval;

	cmd.cmd = CMD_READ_REG;
	cmd.n_addr = 1;
	cmd.addr[0] = REG_OTP;
	cmd.n_rx = 1;
	cmd.rx_buf = otp;

	retval = ath79_spinand_cmd(spi_nand, &cmd);
	if (retval < 0)
		dev_err(&spi_nand->dev, "error %d get otp\n", retval);

	return retval;
}

static int ath79_spinand_set_otp(struct spi_device *spi_nand, u8 *otp)
{
	int retval;
	struct ath79_spinand_command cmd = {0};

	cmd.cmd = CMD_WRITE_REG,
	cmd.n_addr = 1,
	cmd.addr[0] = REG_OTP,
	cmd.n_tx = 1,
	cmd.tx_buf = otp,

	retval = ath79_spinand_cmd(spi_nand, &cmd);
	if (retval < 0)
		dev_err(&spi_nand->dev, "error %d set otp\n", retval);

	return retval;
}

static int ath79_spinand_enable_ecc(struct spi_device *spi_nand)
{
	u8 otp = 0;

	if (ath79_spinand_get_otp(spi_nand, &otp))
		return -1;

	if ((otp & REG_ECC_MASK) == REG_ECC_ON)
		return 0;

	otp |= REG_ECC_ON;
	if (ath79_spinand_set_otp(spi_nand, &otp))
		return -1;

	return ath79_spinand_get_otp(spi_nand, &otp);
}

static int ath79_spinand_disable_ecc(struct spi_device *spi_nand)
{
	u8 otp = 0;

	if (ath79_spinand_get_otp(spi_nand, &otp))
		return -1;

	if ((otp & REG_ECC_MASK) == REG_ECC_OFF)
		return 0;

	otp &= ~REG_ECC_MASK;
	if (ath79_spinand_set_otp(spi_nand, &otp))
		return -1;

	return ath79_spinand_get_otp(spi_nand, &otp);
}

static int ath79_spinand_write_enable(struct spi_device *spi_nand)
{
	struct ath79_spinand_command cmd = {0};

	cmd.cmd = CMD_WR_ENABLE;

	return ath79_spinand_cmd(spi_nand, &cmd);
}

static int ath79_spinand_read_page_to_cache(struct spi_device *spi_nand, u32 page_id)
{
	struct ath79_spinand_command cmd = {0};

	cmd.cmd = CMD_READ;
	cmd.n_addr = 3;
	cmd.addr[0] = (u8)(page_id >> 16);
	cmd.addr[1] = (u8)(page_id >> 8);
	cmd.addr[2] = (u8)(page_id >> 0);

	return ath79_spinand_cmd(spi_nand, &cmd);
}

static int ath79_spinand_read_from_cache(struct spi_device *spi_nand,
		u32 offset, u32 len, u8 *rbuf)
{
	struct ath79_spinand_command cmd = {0};

	cmd.cmd = CMD_READ_RDM;
	cmd.n_addr = 3;
	cmd.addr[0] = 0;
	cmd.addr[1] = (u8)(offset >> 8);
	cmd.addr[2] = (u8)(offset >> 0);
	cmd.n_dummy = 0;
	cmd.n_rx = len;
	cmd.rx_buf = rbuf;

	return ath79_spinand_cmd(spi_nand, &cmd);
}

static int ath79_spinand_read_page(struct spi_device *spi_nand, u32 page_id,
		u32 offset, u32 len, u8 *rbuf)
{
	int ret;
	u8 status = 0;

	ret = ath79_spinand_read_page_to_cache(spi_nand, page_id);
	if (ret < 0) {
		dev_err(&spi_nand->dev, "Read page to cache failed!\n");
		return ret;
	}

	if (__ath79_wait_till_ready(spi_nand, &status)) {
		dev_err(&spi_nand->dev, "WAIT timedout!\n");
		return -EBUSY;
	}

	if ((status & STATUS_OIP_MASK) != STATUS_READY)
		return -EBUSY;

	if (STATUS2ECC(status) == STATUS_ECC_ERROR) {
		struct mtd_info *mtd = dev_get_drvdata(&spi_nand->dev);
		struct nand_chip *chip = (struct nand_chip *)mtd->priv;
		struct ath79_spinand_info *info = (void *)chip->priv;

		/* mark the block bad in nand scan stage */
		if (info->init) {
			memset(rbuf, 0, len);
			return 0;
		}

		dev_err(&spi_nand->dev,
			"ecc error, page=%d\n", page_id);

		return -1;
	}

	ret = ath79_spinand_read_from_cache(spi_nand, offset, len, rbuf);
	if (ret < 0) {
		dev_err(&spi_nand->dev, "read from cache failed!!\n");
		return ret;
	}

	return ret;
}

static int ath79_spinand_program_data_to_cache(struct spi_device *spi_nand,
		u32 offset, u32 len, u8 *wbuf)
{
	struct ath79_spinand_command cmd = {0};

	cmd.cmd = CMD_PROG_PAGE_LOAD;
	cmd.n_addr = 2;
	cmd.addr[0] = 0;
	cmd.addr[1] = 0;;
	cmd.n_tx = len;
	cmd.tx_buf = wbuf;

	return ath79_spinand_cmd(spi_nand, &cmd);
}

static int ath79_spinand_program_execute(struct spi_device *spi_nand, u32 page_id)
{
	struct ath79_spinand_command cmd = {0};

	cmd.cmd = CMD_PROG_PAGE_EXC;
	cmd.n_addr = 3;
	cmd.addr[0] = (u8)(page_id >> 16);
	cmd.addr[1] = (u8)(page_id >> 8);
	cmd.addr[2] = (u8)(page_id >> 0);

	return ath79_spinand_cmd(spi_nand, &cmd);
}

static int ath79_spinand_program_page(struct spi_device *spi_nand,
		u32 page_id, u32 offset, u32 len, u8 *buf)
{
	int retval;
	u8 status = 0;
	uint8_t *wbuf;
	unsigned int i, j;

	wbuf = devm_kzalloc(&spi_nand->dev, CACHE_BUF, GFP_KERNEL);
	if (!wbuf) {
		dev_err(&spi_nand->dev, "No memory\n");
		return -ENOMEM;
	}

	if (ath79_spinand_read_page(spi_nand, page_id, 0, CACHE_BUF, wbuf)) {
		devm_kfree(&spi_nand->dev, wbuf);
		return -1;
	}

	for (i = offset, j = 0; i < len; i++, j++)
		wbuf[i] &= buf[j];

	retval = ath79_spinand_program_data_to_cache(spi_nand, offset,
						     len, wbuf);

	devm_kfree(&spi_nand->dev, wbuf);

	if (retval < 0) {
		dev_err(&spi_nand->dev, "program data to cache failed\n");
		return retval;
	}

	retval = ath79_spinand_write_enable(spi_nand);
	if (retval < 0) {
		dev_err(&spi_nand->dev, "write enable failed!!\n");
		return retval;
	}

	if (ath79_wait_till_ready(spi_nand)) {
		dev_err(&spi_nand->dev, "wait timedout!!!\n");
		return -EBUSY;
	}

	retval = ath79_spinand_program_execute(spi_nand, page_id);
	if (retval < 0) {
		dev_err(&spi_nand->dev, "program execute failed\n");
		return retval;
	}

	if (__ath79_wait_till_ready(spi_nand, &status)) {
		dev_err(&spi_nand->dev, "wait timedout!!!\n");
		return -EBUSY;
	}

	if ((status & STATUS_OIP_MASK) != STATUS_READY)
		return -EBUSY;

	if ((status & STATUS_P_FAIL_MASK) == STATUS_P_FAIL) {
		dev_err(&spi_nand->dev,
			"program error, page %d\n", page_id);
		return -1;
	}

	return 0;
}

static int ath79_spinand_erase_block_erase(struct spi_device *spi_nand, u32 block_id)
{
	struct ath79_spinand_command cmd = {0};
	u32 row = BLOCK_TO_RA(block_id);

	cmd.cmd = CMD_ERASE_BLK;
	cmd.n_addr = 3;
	cmd.addr[0] = (u8)(row >> 16);
	cmd.addr[1] = (u8)(row >> 8);
	cmd.addr[2] = (u8)(row >> 0);

	return ath79_spinand_cmd(spi_nand, &cmd);
}

static int ath79_spinand_erase_block(struct mtd_info *mtd,
		struct spi_device *spi_nand, u32 page)
{
	int retval;
	u8 status = 0;
	u32 block_id = page >> (mtd->erasesize_shift - mtd->writesize_shift);

	retval = ath79_spinand_write_enable(spi_nand);
	if (retval < 0) {
		dev_err(&spi_nand->dev, "write enable failed!\n");
		return retval;
	}

	if (ath79_wait_till_ready(spi_nand)) {
		dev_err(&spi_nand->dev, "wait timedout!\n");
		return -EBUSY;
	}

	retval = ath79_spinand_erase_block_erase(spi_nand, block_id);
	if (retval < 0) {
		dev_err(&spi_nand->dev, "erase block failed!\n");
		return retval;
	}

	if (__ath79_wait_till_ready(spi_nand, &status)) {
		dev_err(&spi_nand->dev, "wait timedout!\n");
		return -EBUSY;
	}

	if ((status & STATUS_OIP_MASK) != STATUS_READY)
		return -EBUSY;

	if ((status & STATUS_E_FAIL_MASK) == STATUS_E_FAIL) {
		dev_err(&spi_nand->dev,
			"erase error, block %d\n", block_id);
		return -1;
	}

	return 0;
}

static void ath79_spinand_write_page_hwecc(struct mtd_info *mtd,
		struct nand_chip *chip, const uint8_t *buf)
{
	chip->write_buf(mtd, buf, chip->ecc.size * chip->ecc.steps);
}

static int ath79_spinand_read_page_hwecc(struct mtd_info *mtd,
		struct nand_chip *chip, uint8_t *buf, int page)
{
	u8 status;
	uint8_t *p = buf;
	struct ath79_spinand_info *info =
			(struct ath79_spinand_info *)chip->priv;
	struct spi_device *spi_nand = info->spi;

	chip->read_buf(mtd, p, chip->ecc.size * chip->ecc.steps);

	if (__ath79_wait_till_ready(spi_nand, &status)) {
		dev_err(&spi_nand->dev, "wait timedout!\n");
		return -EBUSY;
	}

	if ((status & STATUS_OIP_MASK) != STATUS_READY)
		return -EBUSY;

	if (STATUS2ECC(status) == STATUS_ECC_ERROR) {
		pr_info("%s: ECC error\n", __func__);
		mtd->ecc_stats.failed++;
	} else if (STATUS2ECC(status) >= STATUS_ECC_ERR_BITS7) {
		pr_debug("%s: ECC error %d corrected\n",
			 __func__, STATUS2ECC(status));
		/* mtd->ecc_stats.corrected++; */
	}

	return 0;
}

static void ath79_spinand_select_chip(struct mtd_info *mtd, int dev)
{
}

static uint8_t ath79_spinand_read_byte(struct mtd_info *mtd)
{
	struct ath79_spinand_state *state = mtd_to_state(mtd);

	return state->buf[state->buf_ptr++];
}

static int ath79_spinand_wait(struct mtd_info *mtd, struct nand_chip *chip)
{
	struct ath79_spinand_info *info = (struct ath79_spinand_info *)chip->priv;
	unsigned long timeo = jiffies;
	u8 status;

	if (chip->state == FL_ERASING)
		timeo += (HZ * 400) / 1000;
	else
		timeo += (HZ * 20) / 1000;

	while (time_before(jiffies, timeo)) {
		if (ath79_spinand_read_status(info->spi, &status))
			return -1;

		if ((status & STATUS_OIP_MASK) == STATUS_READY)
			return 0;

		cond_resched();
	}
	return 0;
}

static void ath79_spinand_write_buf(struct mtd_info *mtd, const uint8_t *buf, int len)
{
	struct ath79_spinand_state *state = mtd_to_state(mtd);

	memcpy(state->buf + state->buf_ptr, buf, len);
	state->buf_ptr += len;
}

static void ath79_spinand_read_buf(struct mtd_info *mtd, uint8_t *buf, int len)
{
	struct ath79_spinand_state *state = mtd_to_state(mtd);

	memcpy(buf, state->buf + state->buf_ptr, len);
	state->buf_ptr += len;
}

static int ath79_init_size(struct mtd_info *mtd, struct nand_chip *this, u8 *id_data)
{
	mtd->oobsize		= 128;
	mtd->writesize_shift	= 11;
	mtd->writesize		= (1 << mtd->writesize_shift);
	mtd->writesize_mask	= (mtd->writesize - 1);
	mtd->erasesize_shift	= 17;
	mtd->erasesize		= (1 << mtd->erasesize_shift);
	mtd->erasesize_mask	= (mtd->erasesize - 1);

	return NAND_BUSWIDTH_16;
}

static void ath79_spinand_reset(struct spi_device *spi_nand)
{
	struct ath79_spinand_command cmd = {0};

	cmd.cmd = CMD_RESET;

	if (ath79_spinand_cmd(spi_nand, &cmd) < 0)
		pr_info("ath79_spinand reset failed!\n");

	/* elapse 1ms before issuing any other command */
	udelay(1000);

	if (ath79_wait_till_ready(spi_nand))
		dev_err(&spi_nand->dev, "wait timedout!\n");
}

static void ath79_spinand_cmdfunc(struct mtd_info *mtd, unsigned int command,
		int column, int page)
{
	struct nand_chip *chip = (struct nand_chip *)mtd->priv;
	struct ath79_spinand_info *info = (struct ath79_spinand_info *)chip->priv;
	struct ath79_spinand_state *state = (struct ath79_spinand_state *)info->priv;

	switch (command) {
	case NAND_CMD_READ1:
	case NAND_CMD_READ0:
		state->buf_ptr = 0;
		ath79_spinand_read_page(info->spi, page, 0x0,
					mtd->writesize + mtd->oobsize,
					state->buf);
		break;
	case NAND_CMD_READOOB:
		state->buf_ptr = 0;
		ath79_spinand_read_page(info->spi, page, mtd->writesize,
					mtd->oobsize, state->buf);
		break;
	case NAND_CMD_RNDOUT:
		state->buf_ptr = column;
		break;
	case NAND_CMD_READID:
		state->buf_ptr = 0;
		ath79_spinand_read_id(info->spi, (u8 *)state->buf);
		break;
	case NAND_CMD_PARAM:
		state->buf_ptr = 0;
		break;
	/* ERASE1 stores the block and page address */
	case NAND_CMD_ERASE1:
		ath79_spinand_erase_block(mtd, info->spi, page);
		break;
	/* ERASE2 uses the block and page address from ERASE1 */
	case NAND_CMD_ERASE2:
		break;
	/* SEQIN sets up the addr buffer and all registers except the length */
	case NAND_CMD_SEQIN:
		state->col = column;
		state->row = page;
		state->buf_ptr = 0;
		break;
	/* PAGEPROG reuses all of the setup from SEQIN and adds the length */
	case NAND_CMD_PAGEPROG:
		ath79_spinand_program_page(info->spi, state->row, state->col,
					   state->buf_ptr, state->buf);
		break;
	case NAND_CMD_STATUS:
		ath79_spinand_get_otp(info->spi, state->buf);
		if (!(state->buf[0] & REG_OTP_PRT))
			state->buf[0] = REG_OTP_PRT;
		state->buf_ptr = 0;
		break;
	/* RESET command */
	case NAND_CMD_RESET:
		if (ath79_wait_till_ready(info->spi))
			dev_err(&info->spi->dev, "WAIT timedout!!!\n");

		/* a minimum of 250us must elapse before issuing RESET cmd*/
		udelay(250);
		ath79_spinand_reset(info->spi);
		break;
	default:
		dev_err(&mtd->dev, "Unknown CMD: 0x%x\n", command);
	}
}

static int ath79_spinand_lock_block(struct spi_device *spi_nand, u8 lock)
{
	struct ath79_spinand_command cmd = {0};
	int ret;

	cmd.cmd = CMD_WRITE_REG;
	cmd.n_addr = 1;
	cmd.addr[0] = REG_BLOCK_LOCK;
	cmd.n_tx = 1;
	cmd.tx_buf = &lock;

	ret = ath79_spinand_cmd(spi_nand, &cmd);
	if (ret < 0)
		dev_err(&spi_nand->dev, "error %d lock block\n", ret);

	return ret;
}

static int ath79_spinand_probe(struct spi_device *spi_nand)
{
	struct mtd_info *mtd;
	struct nand_chip *chip;
	struct ath79_spinand_info *info;
	struct ath79_spinand_state *state;

	info  = devm_kzalloc(&spi_nand->dev, sizeof(struct ath79_spinand_info),
			GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->init = true;
	info->spi = spi_nand;

	ath79_spinand_lock_block(spi_nand, BL_ALL_UNLOCKED);

	state = devm_kzalloc(&spi_nand->dev, sizeof(struct ath79_spinand_state),
			     GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	info->priv	= state;
	state->buf_ptr	= 0;
	state->buf	= devm_kzalloc(&spi_nand->dev, BUFSIZE, GFP_KERNEL);
	if (!state->buf)
		return -ENOMEM;

	chip = devm_kzalloc(&spi_nand->dev, sizeof(struct nand_chip),
			    GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->ecc.mode	= NAND_ECC_HW;
	chip->ecc.size	= 512;
	chip->ecc.bytes	= 16;
	chip->ecc.layout = (void *)&ath79_spinand_oob_128;
	chip->badblock_pattern = &ath79_badblock_pattern;
	chip->ecc.read_page = ath79_spinand_read_page_hwecc;
	chip->ecc.write_page = ath79_spinand_write_page_hwecc;

	chip->priv	= info;
	chip->read_buf	= ath79_spinand_read_buf;
	chip->write_buf	= ath79_spinand_write_buf;
	chip->read_byte	= ath79_spinand_read_byte;
	chip->cmdfunc	= ath79_spinand_cmdfunc;
	chip->waitfunc	= ath79_spinand_wait;
	chip->options	|= NAND_CACHEPRG;
	chip->select_chip = ath79_spinand_select_chip;
	chip->init_size = ath79_init_size;

	mtd = devm_kzalloc(&spi_nand->dev, sizeof(struct mtd_info), GFP_KERNEL);
	if (!mtd)
		return -ENOMEM;

	dev_set_drvdata(&spi_nand->dev, mtd);

	mtd->priv		= chip;
	mtd->name		= dev_name(&spi_nand->dev);
	mtd->owner		= THIS_MODULE;

	if (nand_scan(mtd, 1))
		return -ENXIO;

	ath79_spinand_enable_ecc(spi_nand);

	info->init = false;

	return mtd_device_parse_register(mtd, NULL, NULL, NULL, 0);
}

static int ath79_spinand_remove(struct spi_device *spi)
{
	mtd_device_unregister(dev_get_drvdata(&spi->dev));

	return 0;
}

static struct spi_driver ath79_spinand_driver = {
	.driver = {
		.name		= "ath79-spinand",
		.bus		= &spi_bus_type,
		.owner		= THIS_MODULE,
	},
	.probe		= ath79_spinand_probe,
	.remove		= ath79_spinand_remove,
};

module_spi_driver(ath79_spinand_driver);

MODULE_DESCRIPTION("SPI NAND driver for Giga Device");
MODULE_LICENSE("GPL v2");
