#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/libata.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <scsi/scsi_host.h>

struct pata_gpio {
	struct gpio_descs *led_gpios;
	struct gpio_descs *databus_gpios;
	struct gpio_desc *reset_gpio;
	struct gpio_descs *cs_gpios;
	struct gpio_descs *address_gpios;
	struct gpio_desc *strobe_write_gpio;
	struct gpio_desc *strobe_read_gpio;
};

static int pata_gpio_set_register(struct pata_gpio *pata, unsigned long reg)
{
	int err; 
	unsigned long cs_state = 0b01;

	err = gpiod_set_array_value_cansleep(pata->cs_gpios->ndescs,
										 pata->cs_gpios->desc,
										 pata->cs_gpios->info,
										 &cs_state);
	if (err)
		return err; 

	return gpiod_set_array_value_cansleep(pata->address_gpios->ndescs,
										 pata->address_gpios->desc,
										 pata->address_gpios->info,
										 &reg);
}

static int pata_gpio_read16(struct pata_gpio *pata, u8 reg, u16 *result)
{
	u8 i;
	int err; 
	unsigned long value = 0;

	for (i = 0; i < pata->databus_gpios->ndescs; i++) {
		err = gpiod_direction_input(pata->databus_gpios->desc[i]);
		if (err)
			return err; 
	}

	err = pata_gpio_set_register(pata, reg);
	if (err)
		return err; 

	gpiod_set_value_cansleep(pata->strobe_read_gpio, 1);
	usleep_range(1, 100);

	err = gpiod_get_array_value_cansleep(pata->databus_gpios->ndescs,
				   pata->databus_gpios->desc,
				   pata->databus_gpios->info,
				   &value);

	gpiod_set_value_cansleep(pata->strobe_read_gpio, 0);

	if (!err) 
		*result = value;

	return err; 
}

static int pata_gpio_write16(struct pata_gpio *pata, u8 reg, unsigned long value)
{
	u8 i;
	int err; 

	err = pata_gpio_set_register(pata, reg);
	if (err)
		return err; 

	for (i = 0; i < pata->databus_gpios->ndescs; i++) {
		err = gpiod_direction_output(pata->databus_gpios->desc[i], (value >> i) & 0x01);
		if (err)
			return err; 
	}

	gpiod_set_value_cansleep(pata->strobe_write_gpio, 1);
	usleep_range(1, 100);
	gpiod_set_value_cansleep(pata->strobe_write_gpio, 0);
	usleep_range(1, 100);

	for (i = 0; i < pata->databus_gpios->ndescs; i++) {
		err = gpiod_direction_input(pata->databus_gpios->desc[i]);
		if (err)
			return err; 
	}

	return 0; 
}

#define SECTOR_SIZE 512
#define CF_BASE_REG 0
#define CF_DATA     CF_BASE_REG + 0 // IDE Data Port
#define CF_ERROR    CF_BASE_REG + 1 // Error code (read)
#define CF_FEATURE  CF_BASE_REG + 1 // Feature (write)
#define CF_NUMSECT  CF_BASE_REG + 2 // Numbers of sectors to transfer
#define CF_ADDR0    CF_BASE_REG + 3 // Sector address LBA 0 (0:7)
#define CF_ADDR1    CF_BASE_REG + 4 // Sector address LBA 1 (8:15)
#define CF_ADDR2    CF_BASE_REG + 5 // Sector address LBA 2 (16:23)
#define CF_ADDR3    CF_BASE_REG + 6 // Sector address LBA 3 (24:27)
#define CF_STATUS   CF_BASE_REG + 7 // Status (read)
#define CF_COMMAND  CF_BASE_REG + 7 // Command (write)
#define CF_ADDR3_ADDITIONAL 0xE0 // Drive 0 aka Master, LBA addressing, bit 5 and 7 at 1 (as per standard)

int cf_identify(struct pata_gpio *pata, u8* data)
{
	/*
		https://wiki.osdev.org/ATA_PIO_Mode:
		To use the IDENTIFY command, select a target drive by sending 0xA0 for the master drive, or 0xB0 for the slave, 
		to the "drive select" IO port. On the Primary bus, this would be port 0x1F6. Then set the Sectorcount, LBAlo, LBAmid, 
		and LBAhi IO ports to 0 (port 0x1F2 to 0x1F5). Then send the IDENTIFY command (0xEC) to the Command IO port (0x1F7). 
		Then read the Status port (0x1F7) again. If the value read is 0, the drive does not exist. For any other value: 
		poll the Status port (0x1F7) until bit 7 (BSY, value = 0x80) clears. Because of some ATAPI drives that do not follow spec, 
		at this point you need to check the LBAmid and LBAhi ports (0x1F4 and 0x1F5) to see if they are non-zero. 
		If so, the drive is not ATA, and you should stop polling. 
		Otherwise, continue polling one of the Status ports until bit 3 (DRQ, value = 8) sets, or until bit 0 (ERR, value = 1) sets.
		At that point, if ERR is clear, the data is ready to read from the Data port (0x1F0). Read 256 16-bit values, and store them.
	*/

	int i; 
	u16 result; 
	
	pata_gpio_write16(pata, CF_ADDR3, 0xA0);

	pata_gpio_write16(pata, CF_NUMSECT, 0x00);
	pata_gpio_write16(pata, CF_ADDR0, 0x00);
	pata_gpio_write16(pata, CF_ADDR1, 0x00);
	pata_gpio_write16(pata, CF_ADDR2, 0x00);

	pata_gpio_write16(pata, CF_COMMAND, 0xEC);

	pata_gpio_read16(pata, CF_STATUS, &result);
	if (result != 0x00)
	{
		usleep_range(1000000, 1000000);

		for (i = 0; i < SECTOR_SIZE; i+=2)
		{
			pata_gpio_read16(pata, CF_DATA, &result);
			*data++ = (result >> 8) & 0xFF;  
			*data++ = result & 0xFF;
		}

		return 1;
	}
	else
	{
		//printf("No device detected!\n");
		return 0; 
	}
}


// static int pata_gpio_softreset(struct ata_link *link, unsigned int *classes,
// 			 unsigned long deadline)
// {
// 	struct ata_port *ap = link->ap;
// 	unsigned int devmask = 0;
// 	int rc;
// 	u8 err;

// 	/* determine if device 0 is present */
// 	if (pata_s3c_devchk(ap, 0))
// 		devmask |= (1 << 0);

// 	/* select device 0 again */
// 	pata_s3c_dev_select(ap, 0);

// 	/* issue bus reset */
// 	rc = pata_s3c_bus_softreset(ap, deadline);
// 	/* if link is occupied, -ENODEV too is an error */
// 	if (rc && rc != -ENODEV) {
// 		ata_link_err(link, "SRST failed (errno=%d)\n", rc);
// 		return rc;
// 	}

// 	/* determine by signature whether we have ATA or ATAPI devices */
// 	classes[0] = ata_sff_dev_classify(&ap->link.device[0],
// 					  devmask & (1 << 0), &err);

// 	return 0;
// }

// static struct scsi_host_template pata_gpio_sht = {
// 	ATA_PIO_SHT("pata-gpio"),
// };

// static struct ata_port_operations pata_gpio_port_ops = {
// 	.inherits				= &ata_sff_port_ops,
// 	.sff_check_status		= pata_gpio_check_status,
// 	.sff_check_altstatus	= pata_gpio_check_altstatus,
// 	.sff_tf_load			= pata_gpio_tf_load,
// 	.sff_tf_read			= pata_gpio_tf_read,
// 	.sff_data_xfer			= pata_gpio_data_xfer,
// 	.sff_exec_command		= pata_gpio_exec_command,
// 	.sff_dev_select			= pata_gpio_dev_select,
// 	.sff_set_devctl			= pata_gpio_set_devctl,
// 	.softreset				= pata_gpio_softreset,
// 	.set_piomode 			= pata_gpio_set_piomode,
// };

static int claim_gpios(struct gpio_descs **target, unsigned count, const char *name, enum gpiod_flags flags, struct device *dev)
{
	struct gpio_descs *gpios; 

	gpios = devm_gpiod_get_array(dev, name, flags);
	if (!gpios) {
		return -ENOMEM;
	}
	if (gpios->ndescs != count) {
		return -EINVAL;
	}

	*target = gpios;
	return 0;
}

static int pata_gpio_probe(struct platform_device *pdev)
{
	int nb, i, err;
	struct device *dev = &pdev->dev;
	struct pata_gpio *pata;
	struct ata_host *host;
	struct ata_port *ap;
	u8 data[512];

	pata = devm_kzalloc(dev, sizeof(struct pata_gpio), GFP_KERNEL);
	if (!pata)
		return -ENOMEM;

	err = claim_gpios(&pata->led_gpios, 4, "led", GPIOD_OUT_LOW, dev);
	if (err) {
		dev_err(dev, "Failed to request led gpios: %d\n", err);
		return err;
	}

	err = claim_gpios(&pata->databus_gpios, 16, "databus", GPIOD_IN, dev);
	if (err) {
		dev_err(dev, "Failed to request databus gpios: %d\n", err);
		return err;
	}

	err = claim_gpios(&pata->cs_gpios, 2, "cs", GPIOD_OUT_LOW, dev);
	if (err) {
		dev_err(dev, "Failed to request cs gpios: %d\n", err);
		return err;
	}

	err = claim_gpios(&pata->address_gpios, 3, "address", GPIOD_OUT_LOW, dev);
	if (err) {
		dev_err(dev, "Failed to request address gpios: %d\n", err);
		return err;
	}

	pata->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (!pata->reset_gpio)
		return -ENOMEM;

	if (IS_ERR(pata->reset_gpio))
		return PTR_ERR(pata->reset_gpio);

	pata->strobe_read_gpio = devm_gpiod_get(dev, "strobe-read", GPIOD_OUT_LOW);
	if (!pata->strobe_read_gpio)
		return -ENOMEM;

	if (IS_ERR(pata->strobe_read_gpio)) 
		return PTR_ERR(pata->strobe_read_gpio);

	pata->strobe_write_gpio = devm_gpiod_get(dev, "strobe-write", GPIOD_OUT_LOW);
	if (!pata->strobe_write_gpio) 
		return -ENOMEM;
	
	if (IS_ERR(pata->strobe_write_gpio))
		return PTR_ERR(pata->strobe_write_gpio);


	// reset
	gpiod_set_value_cansleep(pata->reset_gpio, 1);
	usleep_range(10, 1000); 
	gpiod_set_value_cansleep(pata->reset_gpio, 0);
	usleep_range(10, 1000); 

	usleep_range(1000000, 1000000); 


/*
			gpiod_set_value_cansleep(led, 1);
						usleep_range(1000000, 10000000); 

 */ 

	/*host = ata_host_alloc(&pdev->dev, 1);
	if (!host) {
		dev_err(dev, "failed to allocate ide host\n");
		return -ENOMEM;
	}

	host->private_data = pata;

	ap = host->ports[0];
	ap->ops = &pata_s3c_port_ops;
	ap->pio_mask = ATA_PIO0;
	ap->flags |= ATA_FLAG_PIO_POLLING; */

	for (i = 0; i < 8; ++i)
	{
		u16 result;
		if (pata_gpio_read16(pata, i, &result))	{
			dev_err(dev, "failed to read register %d\n", i);
		} else {
			dev_info(dev, "register %d: 0x%04X\n", i, result);
		}
	}


	cf_identify(pata, data);

	print_hex_dump(KERN_INFO, "identify: ", DUMP_PREFIX_NONE,
		    16, 2,
		    data, 512, 1);

	//return ata_host_activate(host, -1, null, 0, &pata_gpio_sht);
	return 0;
}

static const struct of_device_id pata_gpio_dt_ids[] = {
	{ .compatible = "pata-gpio" },
	{}
};
MODULE_DEVICE_TABLE(of, pata_gpio_dt_ids);


static struct platform_driver pata_gpio_driver = {
	.driver = {
		.name	= "pata-gpio",
		.of_match_table = of_match_ptr(pata_gpio_dt_ids),
	},
	.probe		= pata_gpio_probe,
	/*.remove		= ata_platform_remove_one,*/
};
module_platform_driver(pata_gpio_driver);

MODULE_DESCRIPTION("PATA driver using generic bitbanged GPIO");
MODULE_AUTHOR("Tobias Schramm, Tobias Maedel");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");