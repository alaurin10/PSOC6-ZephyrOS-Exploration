/*
 * Serial NOR flash on the PSoC 6 (CAT1A) SMIF, through the Zephyr flash API.
 *
 * At boot the PDL asks the memory for its SFDP tables (JESD216) and fills in
 * the memory description below: read/program/erase commands, address width,
 * page and sector sizes, timings. Reads and programs use the PDL's blocking
 * calls (a page program takes well under a millisecond). Sector erases take
 * hundreds of milliseconds, so the driver sends the erase command itself and
 * sleeps while it polls the busy bit, instead of the PDL's busy-wait.
 *
 * If the commands the memory reports need four data lines but its
 * non-volatile Quad Enable bit is off, the driver only sets it when the
 * devicetree allows it (quad-enable); otherwise it falls back to one line.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT infineon_cat1a_smif_nor

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <cy_smif.h>
#include <cy_smif_memslot.h>

LOG_MODULE_REGISTER(flash_psoc6_smif, CONFIG_FLASH_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
	     "the driver supports one memory on the one SMIF block");

#define SMIF_BASE          ((SMIF_Type *)DT_INST_REG_ADDR(0))
#define SMIF_TIMEOUT_US    10000U   /* PDL blocking FIFO transfers */
#define QE_WRITE_TIMEOUT_US 1000000U /* non-volatile register write (S25FL512S: <= 500 ms) */
#define ERASE_POLL_MS      5
#define ERASE_MARGIN_MS    100
#define MAX_HYBRID_REGIONS 16
#define CMD_JEDEC_ID       0x9FU
#define CMD_READ_STATUS    0x05U
#define CMD_READ_SFDP      0x5AU
#define NO_MODE            0xFFFFFFFFU
#define BOOT_BUSY_TIMEOUT_MS 5000

/* ---- Memory description, filled from SFDP by Cy_SMIF_MemInit() ------------------- */

static cy_stc_smif_mem_cmd_t cmd_read;
static cy_stc_smif_mem_cmd_t cmd_write_en;
static cy_stc_smif_mem_cmd_t cmd_write_dis;
static cy_stc_smif_mem_cmd_t cmd_erase;
static cy_stc_smif_mem_cmd_t cmd_chip_erase;
static cy_stc_smif_mem_cmd_t cmd_program;
static cy_stc_smif_mem_cmd_t cmd_read_sts_qe;
static cy_stc_smif_mem_cmd_t cmd_read_sts_wip;
static cy_stc_smif_mem_cmd_t cmd_write_sts_qe;
static cy_stc_smif_mem_cmd_t cmd_read_latency;
static cy_stc_smif_mem_cmd_t cmd_write_latency;

/* SFDP itself is always read the same way: 0x5A, one line, 8 dummy cycles. */
static cy_stc_smif_mem_cmd_t cmd_read_sfdp = {
	.command = CMD_READ_SFDP,
	.cmdWidth = CY_SMIF_WIDTH_SINGLE,
	.addrWidth = CY_SMIF_WIDTH_SINGLE,
	.mode = NO_MODE,
	.modeWidth = CY_SMIF_WIDTH_SINGLE,
	.dummyCycles = 8U,
	.dataWidth = CY_SMIF_WIDTH_SINGLE,
};

static cy_stc_smif_hybrid_region_info_t region_info[MAX_HYBRID_REGIONS];
static cy_stc_smif_hybrid_region_info_t *region_ptrs[MAX_HYBRID_REGIONS];

/* Placeholders until SFDP replaces them. */
static cy_stc_smif_mem_device_cfg_t dev_cfg = {
	.numOfAddrBytes = 3U,
	.memSize = 0x100U,
	.readCmd = &cmd_read,
	.writeEnCmd = &cmd_write_en,
	.writeDisCmd = &cmd_write_dis,
	.eraseCmd = &cmd_erase,
	.eraseSize = 0x1000U,
	.chipEraseCmd = &cmd_chip_erase,
	.programCmd = &cmd_program,
	.programSize = 0x100U,
	.readStsRegWipCmd = &cmd_read_sts_wip,
	.readStsRegQeCmd = &cmd_read_sts_qe,
	.writeStsRegQeCmd = &cmd_write_sts_qe,
	.readSfdpCmd = &cmd_read_sfdp,
	.eraseTime = 1U,
	.chipEraseTime = 16U,
	.programTime = 8U,
	.hybridRegionCount = 0U,
	.hybridRegionInfo = region_ptrs,
	.readLatencyCmd = &cmd_read_latency,
	.writeLatencyCmd = &cmd_write_latency,
};

/* Normal (command) mode only: the memory isn't mapped into the address space. */
static cy_stc_smif_mem_config_t mem_cfg = {
	.slaveSelect = (cy_en_smif_slave_select_t)BIT(DT_INST_PROP(0, slave_select)),
	.flags = CY_SMIF_FLAG_DETECT_SFDP,
	.dataSelect = (cy_en_smif_data_select_t)DT_INST_PROP(0, data_select),
	.deviceCfg = &dev_cfg,
};

static cy_stc_smif_mem_config_t *mem_cfgs[] = {&mem_cfg};

static const cy_stc_smif_block_config_t block_cfg = {
	.memCount = ARRAY_SIZE(mem_cfgs),
	.memConfig = mem_cfgs,
	.majorVersion = CY_SMIF_DRV_VERSION_MAJOR,
	.minorVersion = CY_SMIF_DRV_VERSION_MINOR,
};

/* Same settings as the HAL's QSPI driver on CAT1A. */
static const cy_stc_smif_config_t smif_cfg = {
	.mode = (uint32_t)CY_SMIF_NORMAL,
	.deselectDelay = 7U,
	.rxClockSel = (uint32_t)CY_SMIF_SEL_INV_INTERNAL_CLK,
	.blockEvent = (uint32_t)CY_SMIF_BUS_ERROR,
};

/* ---- Driver ---------------------------------------------------------------------- */

struct smif_nor_config {
	const struct pinctrl_dev_config *pcfg;
	uint32_t size;
	bool quad_enable;
};

struct smif_nor_data {
	struct k_mutex lock;
	cy_stc_smif_context_t ctx;
	struct flash_pages_layout layout;
	uint8_t jedec_id[3];
};

static const struct flash_parameters smif_nor_params = {
	.write_block_size = 1,
	.erase_value = 0xff,
};

static bool in_range(const struct device *dev, off_t offset, size_t len)
{
	const struct smif_nor_config *cfg = dev->config;

	return offset >= 0 && (uint64_t)offset + len <= cfg->size;
}

static int smif_nor_read(const struct device *dev, off_t offset, void *buf, size_t len)
{
	struct smif_nor_data *data = dev->data;
	cy_en_smif_status_t st;

	if (!in_range(dev, offset, len)) {
		return -EINVAL;
	}
	if (len == 0) {
		return 0;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	st = Cy_SMIF_MemRead(SMIF_BASE, &mem_cfg, (uint32_t)offset, buf, len, &data->ctx);
	k_mutex_unlock(&data->lock);

	if (st != CY_SMIF_SUCCESS) {
		LOG_ERR("Read %zu B @ 0x%lx failed (0x%x)", len, (long)offset, st);
		return -EIO;
	}
	return 0;
}

static int smif_nor_write(const struct device *dev, off_t offset, const void *buf, size_t len)
{
	struct smif_nor_data *data = dev->data;
	cy_en_smif_status_t st;

	if (!in_range(dev, offset, len)) {
		return -EINVAL;
	}
	if (len == 0) {
		return 0;
	}

	/* Splits at page boundaries and waits for each page to be programmed. */
	k_mutex_lock(&data->lock, K_FOREVER);
	st = Cy_SMIF_MemWrite(SMIF_BASE, &mem_cfg, (uint32_t)offset, buf, len, &data->ctx);
	k_mutex_unlock(&data->lock);

	if (st != CY_SMIF_SUCCESS) {
		LOG_ERR("Write %zu B @ 0x%lx failed (0x%x)", len, (long)offset, st);
		return -EIO;
	}
	return 0;
}

static void pack_address(uint32_t addr, uint8_t *out, uint32_t n_bytes)
{
	for (uint32_t i = 0; i < n_bytes; i++) {
		out[i] = (uint8_t)(addr >> (8U * (n_bytes - 1U - i)));
	}
}

/* Sleep between polls of the busy bit, so other threads run during an erase. */
static int wait_ready(struct smif_nor_data *data, uint32_t timeout_ms)
{
	int64_t end = k_uptime_get() + timeout_ms;

	while (Cy_SMIF_MemIsBusy(SMIF_BASE, &mem_cfg, &data->ctx)) {
		if (k_uptime_get() > end) {
			return -ETIMEDOUT;
		}
		k_msleep(ERASE_POLL_MS);
	}
	return 0;
}

static int smif_nor_erase(const struct device *dev, off_t offset, size_t size)
{
	struct smif_nor_data *data = dev->data;
	uint32_t sector = dev_cfg.eraseSize;
	uint32_t timeout_ms = dev_cfg.eraseTime * 2U + ERASE_MARGIN_MS;
	int ret = 0;

	if (!in_range(dev, offset, size) || (offset % sector) != 0 || (size % sector) != 0) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	for (uint32_t addr = (uint32_t)offset; ret == 0 && addr < (uint32_t)offset + size;
	     addr += sector) {
		uint8_t addr_bytes[4];

		pack_address(addr, addr_bytes, dev_cfg.numOfAddrBytes);
		if (Cy_SMIF_MemCmdWriteEnable(SMIF_BASE, &mem_cfg, &data->ctx) != CY_SMIF_SUCCESS ||
		    Cy_SMIF_MemCmdSectorErase(SMIF_BASE, &mem_cfg, addr_bytes, &data->ctx) !=
			    CY_SMIF_SUCCESS) {
			ret = -EIO;
		} else {
			ret = wait_ready(data, timeout_ms);
		}
		if (ret != 0) {
			LOG_ERR("Erase @ 0x%x failed (%d)", addr, ret);
		}
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

static const struct flash_parameters *smif_nor_get_parameters(const struct device *dev)
{
	ARG_UNUSED(dev);
	return &smif_nor_params;
}

static int smif_nor_get_size(const struct device *dev, uint64_t *size)
{
	const struct smif_nor_config *cfg = dev->config;

	*size = cfg->size;
	return 0;
}

#if defined(CONFIG_FLASH_PAGE_LAYOUT)
static void smif_nor_page_layout(const struct device *dev, const struct flash_pages_layout **layout,
				 size_t *layout_size)
{
	struct smif_nor_data *data = dev->data;

	*layout = &data->layout;
	*layout_size = 1;
}
#endif

#if defined(CONFIG_FLASH_JESD216_API)
static int smif_nor_read_jedec_id(const struct device *dev, uint8_t *id)
{
	struct smif_nor_data *data = dev->data;

	memcpy(id, data->jedec_id, sizeof(data->jedec_id));
	return 0;
}
#endif

static DEVICE_API(flash, smif_nor_api) = {
	.read = smif_nor_read,
	.write = smif_nor_write,
	.erase = smif_nor_erase,
	.get_parameters = smif_nor_get_parameters,
	.get_size = smif_nor_get_size,
#if defined(CONFIG_FLASH_PAGE_LAYOUT)
	.page_layout = smif_nor_page_layout,
#endif
#if defined(CONFIG_FLASH_JESD216_API)
	.read_jedec_id = smif_nor_read_jedec_id,
#endif
};

static cy_en_smif_status_t read_jedec_id(struct smif_nor_data *data)
{
	cy_en_smif_status_t st;

	st = Cy_SMIF_TransmitCommand(SMIF_BASE, CMD_JEDEC_ID, CY_SMIF_WIDTH_SINGLE, NULL,
				     CY_SMIF_CMD_WITHOUT_PARAM, CY_SMIF_WIDTH_SINGLE,
				     mem_cfg.slaveSelect, CY_SMIF_TX_NOT_LAST_BYTE, &data->ctx);
	if (st == CY_SMIF_SUCCESS) {
		st = Cy_SMIF_ReceiveDataBlocking(SMIF_BASE, data->jedec_id,
						 sizeof(data->jedec_id), CY_SMIF_WIDTH_SINGLE,
						 &data->ctx);
	}
	return st;
}

/*
 * A reset in the middle of an erase or program doesn't stop it: the memory
 * stays busy for up to a few seconds and ignores most commands meanwhile.
 * Wait for its write-in-progress bit (status register 1, bit 0) to clear.
 */
static int wait_idle_at_boot(struct smif_nor_data *data)
{
	int64_t start = k_uptime_get();
	uint8_t status;

	while (true) {
		if (Cy_SMIF_TransmitCommand(SMIF_BASE, CMD_READ_STATUS, CY_SMIF_WIDTH_SINGLE, NULL,
					    CY_SMIF_CMD_WITHOUT_PARAM, CY_SMIF_WIDTH_SINGLE,
					    mem_cfg.slaveSelect, CY_SMIF_TX_NOT_LAST_BYTE,
					    &data->ctx) != CY_SMIF_SUCCESS ||
		    Cy_SMIF_ReceiveDataBlocking(SMIF_BASE, &status, 1, CY_SMIF_WIDTH_SINGLE,
						&data->ctx) != CY_SMIF_SUCCESS) {
			return -EIO;
		}
		if ((status & BIT(0)) == 0) {
			break;
		}
		if (k_uptime_get() - start > BOOT_BUSY_TIMEOUT_MS) {
			return -ETIMEDOUT;
		}
		k_msleep(ERASE_POLL_MS);
	}
	if (k_uptime_get() - start > ERASE_POLL_MS) {
		LOG_INF("Waited %lld ms for an operation from before the reset to finish",
			k_uptime_get() - start);
	}
	return 0;
}

static bool is_quad(const cy_stc_smif_mem_cmd_t *cmd)
{
	return cmd->addrWidth == CY_SMIF_WIDTH_QUAD || cmd->modeWidth == CY_SMIF_WIDTH_QUAD ||
	       cmd->dataWidth == CY_SMIF_WIDTH_QUAD;
}

/* Replace quad read/program commands with their one-line equivalents. */
static void use_single_line(void)
{
	bool four = dev_cfg.numOfAddrBytes == 4U;

	if (is_quad(&cmd_read)) {
		cmd_read = (cy_stc_smif_mem_cmd_t){
			.command = four ? 0x0CU : 0x0BU, /* (4-byte) fast read */
			.cmdWidth = CY_SMIF_WIDTH_SINGLE,
			.addrWidth = CY_SMIF_WIDTH_SINGLE,
			.mode = NO_MODE,
			.modeWidth = CY_SMIF_WIDTH_SINGLE,
			.dummyCycles = 8U,
			.dataWidth = CY_SMIF_WIDTH_SINGLE,
		};
	}
	if (is_quad(&cmd_program)) {
		cmd_program = (cy_stc_smif_mem_cmd_t){
			.command = four ? 0x12U : 0x02U, /* (4-byte) page program */
			.cmdWidth = CY_SMIF_WIDTH_SINGLE,
			.addrWidth = CY_SMIF_WIDTH_SINGLE,
			.mode = NO_MODE,
			.modeWidth = CY_SMIF_WIDTH_SINGLE,
			.dummyCycles = 0U,
			.dataWidth = CY_SMIF_WIDTH_SINGLE,
		};
	}
}

static int setup_quad(const struct smif_nor_config *cfg, struct smif_nor_data *data)
{
	bool enabled = false;

	if (!is_quad(&cmd_read) && !is_quad(&cmd_program)) {
		return 0;
	}
	if (Cy_SMIF_MemIsQuadEnabled(SMIF_BASE, &mem_cfg, &enabled, &data->ctx) !=
	    CY_SMIF_SUCCESS) {
		return -EIO;
	}
	if (!enabled && cfg->quad_enable) {
		LOG_INF("Setting the memory's Quad Enable bit");
		enabled = Cy_SMIF_MemEnableQuadMode(SMIF_BASE, &mem_cfg, QE_WRITE_TIMEOUT_US,
						    &data->ctx) == CY_SMIF_SUCCESS;
	}
	if (!enabled) {
		LOG_INF("Quad Enable is off: using one data line");
		use_single_line();
	}
	return 0;
}

static const char *width_name(cy_en_smif_txfr_width_t w)
{
	switch (w) {
	case CY_SMIF_WIDTH_SINGLE:
		return "1";
	case CY_SMIF_WIDTH_DUAL:
		return "2";
	case CY_SMIF_WIDTH_QUAD:
		return "4";
	default:
		return "8";
	}
}

static void log_detected(const struct smif_nor_data *data)
{
	LOG_INF("JEDEC ID %02x %02x %02x; SFDP: %u MB, %u-byte addresses, %u KB sectors "
		"(erase <= %u ms), %u B pages",
		data->jedec_id[0], data->jedec_id[1], data->jedec_id[2],
		dev_cfg.memSize / (1024U * 1024U), dev_cfg.numOfAddrBytes,
		dev_cfg.eraseSize / 1024U, dev_cfg.eraseTime, dev_cfg.programSize);
	LOG_INF("Read 0x%02x (%s-%s-%s, %u dummy cycles), program 0x%02x (%s-%s-%s), "
		"erase 0x%02x",
		cmd_read.command, width_name(cmd_read.cmdWidth), width_name(cmd_read.addrWidth),
		width_name(cmd_read.dataWidth), cmd_read.dummyCycles, cmd_program.command,
		width_name(cmd_program.cmdWidth), width_name(cmd_program.addrWidth),
		width_name(cmd_program.dataWidth), cmd_erase.command);
}

static int smif_nor_init(const struct device *dev)
{
	const struct smif_nor_config *cfg = dev->config;
	struct smif_nor_data *data = dev->data;
	cy_en_smif_status_t st;
	int ret;

	k_mutex_init(&data->lock);
	for (size_t i = 0; i < ARRAY_SIZE(region_ptrs); i++) {
		region_ptrs[i] = &region_info[i];
	}

	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	st = Cy_SMIF_Init(SMIF_BASE, &smif_cfg, SMIF_TIMEOUT_US, &data->ctx);
	if (st != CY_SMIF_SUCCESS) {
		LOG_ERR("SMIF init failed (0x%x)", st);
		return -EIO;
	}
	Cy_SMIF_SetDataSelect(SMIF_BASE, mem_cfg.slaveSelect, mem_cfg.dataSelect);
	Cy_SMIF_Enable(SMIF_BASE, &data->ctx);

	ret = wait_idle_at_boot(data);
	if (ret < 0) {
		LOG_ERR("The memory doesn't respond or stays busy (%d)", ret);
		return ret;
	}

	st = read_jedec_id(data);
	if (st != CY_SMIF_SUCCESS) {
		LOG_ERR("Reading the JEDEC ID failed (0x%x)", st);
		return -EIO;
	}

	/* Reads the SFDP tables and fills in dev_cfg and the commands. */
	st = Cy_SMIF_MemInit(SMIF_BASE, &block_cfg, &data->ctx);
	if (st != CY_SMIF_SUCCESS) {
		LOG_ERR("No usable SFDP on the memory (JEDEC ID %02x %02x %02x, 0x%x)",
			data->jedec_id[0], data->jedec_id[1], data->jedec_id[2], st);
		return -EIO;
	}
	if (dev_cfg.hybridRegionCount != 0U) {
		LOG_ERR("Memories with mixed sector sizes aren't supported");
		return -ENOTSUP;
	}
	if (cfg->size > dev_cfg.memSize || (cfg->size % dev_cfg.eraseSize) != 0U) {
		LOG_ERR("flash-size 0x%x doesn't fit the memory (%u B, %u B sectors)", cfg->size,
			dev_cfg.memSize, dev_cfg.eraseSize);
		return -EINVAL;
	}

	ret = setup_quad(cfg, data);
	if (ret < 0) {
		return ret;
	}

	data->layout.pages_size = dev_cfg.eraseSize;
	data->layout.pages_count = cfg->size / dev_cfg.eraseSize;
	log_detected(data);
	return 0;
}

PINCTRL_DT_INST_DEFINE(0);

static const struct smif_nor_config smif_nor_config0 = {
	.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(0),
	.size = DT_INST_PROP(0, flash_size),
	.quad_enable = DT_INST_PROP(0, quad_enable),
};

static struct smif_nor_data smif_nor_data0;

DEVICE_DT_INST_DEFINE(0, smif_nor_init, NULL, &smif_nor_data0, &smif_nor_config0, POST_KERNEL,
		      CONFIG_FLASH_INIT_PRIORITY, &smif_nor_api);
