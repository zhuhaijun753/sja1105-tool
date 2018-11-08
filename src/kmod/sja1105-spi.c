/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2016-2018, NXP Semiconductors
 * Copyright (c) 2018, Sensor-Technik Wiedemann GmbH
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/spi/spi.h>
#include <linux/netdevice.h>
#include <lib/include/static-config.h>
#include <lib/include/gtable.h>
#include <lib/include/spi.h>
#include <common.h>
#include "sja1105.h"

#define SPI_TRANSFER_SIZE_MAX  (SIZE_SPI_MSG_HEADER + SIZE_SPI_MSG_MAXLEN)

static int sja1105_spi_transfer(const struct sja1105_spi_setup *spi_setup,
                                const void *tx, void *rx, int size)
{
	struct sja1105_spi_private *priv = container_of(spi_setup,
	                         struct sja1105_spi_private, spi_setup);
	struct spi_device *spi = priv->spi_dev;
	struct spi_transfer transfer = {
		.tx_buf = tx,
		.rx_buf = rx,
		.len = size,
	};
	struct spi_message msg;
	int rc;

	if (size > SPI_TRANSFER_SIZE_MAX) {
		dev_err(&spi->dev, "Spi message too long: is=%i, max=%i\n",
		        size, SPI_TRANSFER_SIZE_MAX);
		return -EMSGSIZE;
	}

	spi_message_init(&msg);

	spi_message_add_tail(&transfer, &msg);

	rc = spi_sync(spi, &msg);
	if (rc) {
		dev_err(&spi->dev, "Spi transfer failed: rc = %d\n", rc);
		return rc;
	}

	return rc;
}

static void sja1105_spi_message_access(void  *buf,
                                       struct sja1105_spi_message *msg,
                                       int    write)
{
	int (*pack_or_unpack)(void*, uint64_t*, int, int, int);
	int size = SIZE_SPI_MSG_HEADER;

	if (write == 0) {
		pack_or_unpack = gtable_unpack;
		memset(msg, 0, sizeof(*msg));
	} else {
		pack_or_unpack = gtable_pack;
		memset(buf, 0, size);
	}
	pack_or_unpack(buf, &msg->access,     31, 31, size);
	pack_or_unpack(buf, &msg->read_count, 30, 25, size);
	pack_or_unpack(buf, &msg->address,    24,  4, size);
}
#define sja1105_spi_message_unpack(buf, msg) \
	sja1105_spi_message_access(buf, msg, 0)
#define sja1105_spi_message_pack(buf, msg) \
	sja1105_spi_message_access(buf, msg, 1)

/* If read_or_write is:
 *     * SPI_WRITE: creates and sends an SPI write message at absolute
 *                  address reg_addr, taking size_bytes from *packed_buf
 *     * SPI_READ: creates and sends an SPI read message from absolute
 *                 address reg_addr, writing size_bytes into *packed_buf
 *
 * This function should only be called if it is priorly known that
 * size_bytes is smaller than SIZE_SPI_MSG_MAXLEN. Larger packed buffers
 * are chunked in smaller pieces by sja1105_spi_send_long_packed_buf below.
 */
inline int
sja1105_spi_send_packed_buf(struct sja1105_spi_setup *spi_setup,
                            enum sja1105_spi_access_mode read_or_write,
                            uint64_t reg_addr, void *packed_buf,
                            uint64_t size_bytes)
{
	const int MSG_LEN = size_bytes + SIZE_SPI_MSG_HEADER;
	struct sja1105_spi_message msg;
	uint8_t tx_buf[MSG_LEN];
	uint8_t rx_buf[MSG_LEN];
	int rc;

	memset(rx_buf, 0, MSG_LEN);

	msg.access     = read_or_write;
	msg.read_count = (read_or_write == SPI_READ) ? (size_bytes / 4) : 0;
	msg.address    = reg_addr;
	sja1105_spi_message_pack(tx_buf, &msg);

	if (read_or_write == SPI_READ) {
		memset(tx_buf + SIZE_SPI_MSG_HEADER, 0, size_bytes);
	} else if (read_or_write == SPI_WRITE) {
		memcpy(tx_buf + SIZE_SPI_MSG_HEADER, /* dest */
		       packed_buf,                   /* src */
		       size_bytes);                  /* size */
	} else {
		loge("read_or_write must be SPI_READ or SPI_WRITE");
		rc = -EINVAL;
		goto out;
	}

	rc = sja1105_spi_transfer(spi_setup, tx_buf, rx_buf, MSG_LEN);
	if (rc < 0) {
		loge("sja1105_spi_transfer failed");
		goto out;
	}
	if (read_or_write == SPI_READ) {
		memcpy(packed_buf,                   /* dest */
		       rx_buf + SIZE_SPI_MSG_HEADER, /* src */
		       size_bytes);                  /* size */
	}
out:
	return rc;
}

/* If read_or_write is:
 *     * SPI_WRITE: creates and sends an SPI write message at absolute
 *                  address reg_addr, taking size_bytes from *value
 *     * SPI_READ: creates and sends an SPI read message from absolute
 *                 address reg_addr, writing size_bytes into *value
 *
 * The uint64_t *value is unpacked, meaning that it's stored in the native
 * CPU endianness and directly usable by software running on the core.
 *
 * This is a wrapper around sja1105_spi_send_packed_buf().
 *
 */
inline int
sja1105_spi_send_int(struct sja1105_spi_setup *spi_setup,
                     enum sja1105_spi_access_mode read_or_write,
                     uint64_t reg_addr, uint64_t *value, uint64_t size_bytes)
{
	uint8_t packed_buf[size_bytes];
	int rc;

	if (read_or_write == SPI_WRITE)
		gtable_pack(packed_buf, value, 8 * size_bytes - 1, 0,
		            size_bytes);

	rc = sja1105_spi_send_packed_buf(spi_setup, read_or_write, reg_addr,
	                                 packed_buf, size_bytes);
	if (read_or_write == SPI_READ)
		gtable_unpack(packed_buf, value, 8 * size_bytes - 1, 0,
		              size_bytes);

	return rc;
}

/*
 * Should be used if a packed_buf larger than SIZE_SPI_MSG_MAXLEN must be
 * sent/received. Splitting the buffer into chunks and assembling those
 * into SPI messages is done automatically by this function.
 */
int sja1105_spi_send_long_packed_buf(struct sja1105_spi_setup *spi_setup,
                                     enum sja1105_spi_access_mode read_or_write,
                                     uint64_t base_addr, char *packed_buf,
                                     uint64_t buf_len)
{
	struct chunk {
		char    *buf_ptr;
		int      len;
		uint64_t spi_address;
	} chunk;
	int distance_to_end;
	int rc = 0;

	/* Initialize chunk */
	chunk.buf_ptr = packed_buf;
	chunk.spi_address = base_addr;
	chunk.len = min((int)buf_len, SIZE_SPI_MSG_MAXLEN);

	while (chunk.len) {
		rc = sja1105_spi_send_packed_buf(spi_setup, read_or_write,
		                                 chunk.spi_address,
		                                 chunk.buf_ptr,
		                                 chunk.len);
		if (rc < 0) {
			loge("spi_send_packed_buf returned %d", rc);
			goto out_send_failed;
		}
		chunk.buf_ptr += chunk.len;
		chunk.spi_address += chunk.len / 4;
		distance_to_end = (int) ((packed_buf + buf_len) -
		                          chunk.buf_ptr);
		chunk.len = min(distance_to_end, SIZE_SPI_MSG_MAXLEN);
	}
out_send_failed:
	return rc;
}

