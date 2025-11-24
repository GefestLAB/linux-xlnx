// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/pwm.h>

#define REG_CTRL    0x00
#define REG_PERIOD  0x04
#define REG_DUTY    0x08
#define REG_PRESC   0x0C
#define CTRL_EN     BIT(0)
#define CTRL_INV    BIT(1)

struct myaxi_pwm {
    struct pwm_chip chip;
    void __iomem *base;
    struct clk *clk;
    u64 clk_rate;
    spinlock_t lock;
};

static inline struct myaxi_pwm *to_my(struct pwm_chip *c)
{
    return container_of(c, struct myaxi_pwm, chip);
}

static int myaxi_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
                           const struct pwm_state *state)
{
    struct myaxi_pwm *m = to_my(chip);
    unsigned long flags;
    u32 ctrl = 0, presc = 0;
    u64 ticks_p, ticks_d;

    if (!m->clk_rate)
        m->clk_rate = clk_get_rate(m->clk);

    /* конвертация ns -> тики (без предделителя для простоты) */
    if (state->period < 2)              /* защита от нулей */
        return -EINVAL;

    ticks_p = div_u64(m->clk_rate * (u64)state->period, 1000000000ULL);
    if (ticks_p < 2)
        ticks_p = 2;

    ticks_d = div_u64(m->clk_rate * (u64)state->duty_cycle, 1000000000ULL);
    if (ticks_d > ticks_p)
        ticks_d = ticks_p;

    if (state->polarity == PWM_POLARITY_INVERSED)
        ctrl |= CTRL_INV;
    if (state->enabled)
        ctrl |= CTRL_EN;

    spin_lock_irqsave(&m->lock, flags);
    /* порядок: выключить, записать regs, включить */
    writel(0,           m->base + REG_CTRL);
    writel(presc,       m->base + REG_PRESC);
    writel((u32)ticks_p, m->base + REG_PERIOD);
    writel((u32)ticks_d, m->base + REG_DUTY);
    writel(ctrl,        m->base + REG_CTRL);
    spin_unlock_irqrestore(&m->lock, flags);

    return 0;
}

static int myaxi_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
                                struct pwm_state *state)
{
    struct myaxi_pwm *m = to_my(chip);
    u32 ctrl = readl(m->base + REG_CTRL);
    u32 per  = readl(m->base + REG_PERIOD);
    u32 dut  = readl(m->base + REG_DUTY);
    u64 rate = m->clk_rate ? m->clk_rate : clk_get_rate(m->clk);

    state->enabled  = ctrl & CTRL_EN;
    state->polarity = (ctrl & CTRL_INV) ? PWM_POLARITY_INVERSED : PWM_POLARITY_NORMAL;
    state->period   = div_u64((u64)per * 1000000000ULL, rate);
    state->duty_cycle = div_u64((u64)dut * 1000000000ULL, rate);

    return 0;
}

static const struct pwm_ops myaxi_pwm_ops = {
    .apply     = myaxi_pwm_apply,
    .get_state = myaxi_pwm_get_state,
    .owner     = THIS_MODULE,
};

static int myaxi_pwm_probe(struct platform_device *pdev)
{
    struct myaxi_pwm *m;
    struct resource *res;
    int ret;

    m = devm_kzalloc(&pdev->dev, sizeof(*m), GFP_KERNEL);
    if (!m) return -ENOMEM;

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    m->base = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(m->base)) return PTR_ERR(m->base);

    m->clk = devm_clk_get(&pdev->dev, NULL);  /* "s_axi_aclk" опционально через names */
    if (!IS_ERR(m->clk)) {
        ret = clk_prepare_enable(m->clk);
        if (ret) return ret;
        m->clk_rate = clk_get_rate(m->clk);
    }

    spin_lock_init(&m->lock);

    m->chip.dev = &pdev->dev;
    m->chip.ops = &myaxi_pwm_ops;
    m->chip.npwm = 1;
    m->chip.of_xlate = of_pwm_xlate_with_flags;  /* #pwm-cells=3 */
    m->chip.of_pwm_n_cells = 3;

    ret = pwmchip_add(&m->chip);
    if (ret) {
        clk_disable_unprepare(m->clk);
        return ret;
    }

    platform_set_drvdata(pdev, m);
    dev_info(&pdev->dev, "GLAB AXI PWM ready, clk=%llu Hz\n", m->clk_rate);
    return 0;
}

static int myaxi_pwm_remove(struct platform_device *pdev)
{
    struct myaxi_pwm *m = platform_get_drvdata(pdev);
    pwmchip_remove(&m->chip);
    if (!IS_ERR(m->clk))
        clk_disable_unprepare(m->clk);
    return 0;
}

static const struct of_device_id myaxi_pwm_of[] = {
    { .compatible = "glab,axi-pwm-1.0" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, myaxi_pwm_of);

static struct platform_driver myaxi_pwm_driver = {
    .driver = {
        .name = "pwm-myaxi",
        .of_match_table = myaxi_pwm_of,
    },
    .probe  = myaxi_pwm_probe,
    .remove = myaxi_pwm_remove,
};
module_platform_driver(myaxi_pwm_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GLAB AXI PWM controller");
MODULE_AUTHOR("you");
