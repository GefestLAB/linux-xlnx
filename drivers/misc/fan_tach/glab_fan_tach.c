// glab_fan_tach.c
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/mutex.h>

#define REG_CONTROL   0x00
#define REG_GATE_MS   0x04
#define REG_PULSES    0x08
#define REG_STATUS    0x0C
#define REG_PPR       0x10

struct glab_fan_tach {
    void __iomem *base;
    u32 gate_ms;
    u32 ppr;
    struct mutex lock;
};

static ssize_t rpm_show(struct device *dev,
                        struct device_attribute *attr, char *buf)
{
    struct glab_fan_tach *ft = dev_get_drvdata(dev);
    u32 pulses;
    u32 status;
    u32 rpm;

    mutex_lock(&ft->lock);

    /* читаем PULSES и STATUS */
    pulses = readl(ft->base + REG_PULSES);
    status = readl(ft->base + REG_STATUS);

    /* можно сбрасывать data_valid чтением/записью при необходимости */

    mutex_unlock(&ft->lock);

    if (ft->gate_ms == 0 || ft->ppr == 0)
        return sysfs_emit(buf, "0\n");

    /* RPM = pulses * (60000 / gate_ms) / ppr */
    rpm = pulses * (60000 / ft->gate_ms) / ft->ppr;

    return sysfs_emit(buf, "%u\n", rpm);
}

static DEVICE_ATTR_RO(rpm);

static struct attribute *glab_fan_attrs[] = {
    &dev_attr_rpm.attr,
    NULL,
};

static const struct attribute_group glab_fan_attr_group = {
    .attrs = glab_fan_attrs,
};

static int glab_fan_tach_probe(struct platform_device *pdev)
{
    struct glab_fan_tach *ft;
    struct resource *res;
    u32 val;
    int ret;

    ft = devm_kzalloc(&pdev->dev, sizeof(*ft), GFP_KERNEL);
    if (!ft)
        return -ENOMEM;

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    ft->base = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(ft->base))
        return PTR_ERR(ft->base);

    mutex_init(&ft->lock);

    /* читаем параметры из DT */
    if (of_property_read_u32(pdev->dev.of_node, "gate-ms", &ft->gate_ms))
        ft->gate_ms = 100;

    if (of_property_read_u32(pdev->dev.of_node, "pulses-per-rev", &ft->ppr))
        ft->ppr = 2;

    /* записываем их в железо */
    writel(ft->gate_ms, ft->base + REG_GATE_MS);
    writel(ft->ppr,     ft->base + REG_PPR);

    /* включаем модуль */
    val = 0x1; // enable
    writel(val, ft->base + REG_CONTROL);

    platform_set_drvdata(pdev, ft);

    ret = sysfs_create_group(&pdev->dev.kobj, &glab_fan_attr_group);
    if (ret)
        return ret;

    dev_info(&pdev->dev, "gLab fan tach driver probed\n");
    return 0;
}

static int glab_fan_tach_remove(struct platform_device *pdev)
{
    sysfs_remove_group(&pdev->dev.kobj, &glab_fan_attr_group);
    return 0;
}

static const struct of_device_id glab_fan_tach_of_match[] = {
    { .compatible = "glab,fan-tach" },
    { }
};
MODULE_DEVICE_TABLE(of, glab_fan_tach_of_match);

static struct platform_driver glab_fan_tach_driver = {
    .probe  = glab_fan_tach_probe,
    .remove = glab_fan_tach_remove,
    .driver = {
        .name           = "glab_fan_tach",
        .of_match_table = glab_fan_tach_of_match,
    },
};

module_platform_driver(glab_fan_tach_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Artem & gLab");
MODULE_DESCRIPTION("Simple fan tachometer driver for Zynq AXI fan IP");
