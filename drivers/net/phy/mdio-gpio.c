/*
 * GPIO based MDIO bitbang driver.
 * Supports OpenFirmware.
 *
 * Copyright (c) 2008 CSE Semaphore Belgium.
 *  by Laurent Pinchart <laurentp@cse-semaphore.com>
 *
 * Copyright (C) 2008, Paulius Zaleckas <paulius.zaleckas@teltonika.lt>
 *
 * Based on earlier work by
 *
 * Copyright (c) 2003 Intracom S.A.
 *  by Pantelis Antoniou <panto@intracom.gr>
 *
 * 2005 (c) MontaVista Software, Inc.
 * Vitaly Bordug <vbordug@ru.mvista.com>
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2. This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/mdio-bitbang.h>
#include <linux/mdio-gpio.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>

#include <linux/of_gpio.h>
#include <linux/of_mdio.h>

struct mdio_gpio_info {
	struct mdiobb_ctrl ctrl;
	struct gpio_desc *mdc, *mdio, *mdo;
};

static int mdio_gpio_get_data(struct device *dev,
			      struct mdio_gpio_info *bitbang)
{
	bitbang->mdc = devm_gpiod_get_index(dev, NULL, MDIO_GPIO_MDC,
					    GPIOD_OUT_LOW);
	if (IS_ERR(bitbang->mdc))
		return PTR_ERR(bitbang->mdc);

	bitbang->mdio = devm_gpiod_get_index(dev, NULL, MDIO_GPIO_MDIO,
					     GPIOD_IN);
	if (IS_ERR(bitbang->mdio))
		return PTR_ERR(bitbang->mdio);

	bitbang->mdo = devm_gpiod_get_index_optional(dev, NULL, MDIO_GPIO_MDO,
						     GPIOD_OUT_LOW);
	return PTR_ERR_OR_ZERO(bitbang->mdo);
}

static void mdio_dir(struct mdiobb_ctrl *ctrl, int dir)
{
	struct mdio_gpio_info *bitbang =
		container_of(ctrl, struct mdio_gpio_info, ctrl);

	if (bitbang->mdo) {
		/* Separate output pin. Always set its value to high
		 * when changing direction. If direction is input,
		 * assume the pin serves as pull-up. If direction is
		 * output, the default value is high.
		 */
		gpiod_set_value(bitbang->mdo, 1);
		return;
	}

	if (dir)
		gpiod_direction_output(bitbang->mdio, 1);
	else
		gpiod_direction_input(bitbang->mdio);
}

static int mdio_get(struct mdiobb_ctrl *ctrl)
{
	struct mdio_gpio_info *bitbang =
		container_of(ctrl, struct mdio_gpio_info, ctrl);

	return gpiod_get_value(bitbang->mdio);
}

static void mdio_set(struct mdiobb_ctrl *ctrl, int what)
{
	struct mdio_gpio_info *bitbang =
		container_of(ctrl, struct mdio_gpio_info, ctrl);

	if (bitbang->mdo)
		gpiod_set_value(bitbang->mdo, what);
	else
		gpiod_set_value(bitbang->mdio, what);
}

static void mdc_set(struct mdiobb_ctrl *ctrl, int what)
{
	struct mdio_gpio_info *bitbang =
		container_of(ctrl, struct mdio_gpio_info, ctrl);

	gpiod_set_value(bitbang->mdc, what);
}

static const struct mdiobb_ops mdio_gpio_ops = {
	.owner = THIS_MODULE,
	.set_mdc = mdc_set,
	.set_mdio_dir = mdio_dir,
	.set_mdio_data = mdio_set,
	.get_mdio_data = mdio_get,
};

static struct mii_bus *mdio_gpio_bus_init(struct device *dev,
					  struct mdio_gpio_info *bitbang,
					  int bus_id)
{
	struct mii_bus *new_bus;

	bitbang->ctrl.ops = &mdio_gpio_ops;

	new_bus = alloc_mdio_bitbang(&bitbang->ctrl);
	if (!new_bus)
		return NULL;

	new_bus->name = "GPIO Bitbanged MDIO";
	new_bus->parent = dev;

	if (bus_id != -1)
		snprintf(new_bus->id, MII_BUS_ID_SIZE, "gpio-%x", bus_id);
	else
		strncpy(new_bus->id, "gpio", MII_BUS_ID_SIZE);

	dev_set_drvdata(dev, new_bus);

	return new_bus;
}

static void mdio_gpio_bus_deinit(struct device *dev)
{
	struct mii_bus *bus = dev_get_drvdata(dev);

	free_mdio_bitbang(bus);
}

static void mdio_gpio_bus_destroy(struct device *dev)
{
	struct mii_bus *bus = dev_get_drvdata(dev);

	mdiobus_unregister(bus);
	mdio_gpio_bus_deinit(dev);
}

static int mdio_gpio_probe(struct platform_device *pdev)
{
	struct mdio_gpio_info *bitbang;
	struct mii_bus *new_bus;
	int ret, bus_id;

	bitbang = devm_kzalloc(&pdev->dev, sizeof(*bitbang), GFP_KERNEL);
	if (!bitbang)
		return -ENOMEM;

	ret = mdio_gpio_get_data(&pdev->dev, bitbang);
	if (ret)
		return ret;

	if (pdev->dev.of_node) {
		bus_id = of_alias_get_id(pdev->dev.of_node, "mdio-gpio");
		if (bus_id < 0) {
			dev_warn(&pdev->dev, "failed to get alias id\n");
			bus_id = 0;
		}
	} else {
		bus_id = pdev->id;
	}

	new_bus = mdio_gpio_bus_init(&pdev->dev, bitbang, bus_id);
	if (!new_bus)
		return -ENODEV;

	if (pdev->dev.of_node)
		ret = of_mdiobus_register(new_bus, pdev->dev.of_node);
	else
		ret = mdiobus_register(new_bus);

	if (ret)
		mdio_gpio_bus_deinit(&pdev->dev);

	return ret;
}

static int mdio_gpio_remove(struct platform_device *pdev)
{
	mdio_gpio_bus_destroy(&pdev->dev);

	return 0;
}

static const struct of_device_id mdio_gpio_of_match[] = {
	{ .compatible = "virtual,mdio-gpio", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mdio_gpio_of_match);

static struct platform_driver mdio_gpio_driver = {
	.probe = mdio_gpio_probe,
	.remove = mdio_gpio_remove,
	.driver		= {
		.name	= "mdio-gpio",
		.of_match_table = mdio_gpio_of_match,
	},
};

module_platform_driver(mdio_gpio_driver);

MODULE_ALIAS("platform:mdio-gpio");
MODULE_AUTHOR("Laurent Pinchart, Paulius Zaleckas");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Generic driver for MDIO bus emulation using GPIO");
