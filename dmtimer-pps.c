// SPDX-License-Identifier: GPL-2.0+
// Copyright (C) 2026 MicroStrain by HBK <ext_jonathan.herbst@hbkworld.com>

#include "dmtimer-pps.h"

#include <linux/cdev.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

#include <plat/dmtimer.h>

// supposedly you need to start the timer with this to get it to work properly
#define DM_TIMER_LOAD_MIN 0xfffffffe
#define DM_TIMER_MAX      0xffffffff

// chardev pool
#define DMTIMER_PPS_MAX_SOURCES 16
static dev_t dmtimer_pps_chardevs;
static struct class *dmtimer_pps_class = NULL;

struct dmtimer_pps {
        struct platform_device *pdev;
        struct omap_dm_timer *dmtimer;
        struct platform_device *dmtimer_pdev;
        struct cdev cdev;
        struct device* cdev_dev;
        int timer_id;
        const char *timer_name;
        int irq;
        spinlock_t lock;
        wait_queue_head_t queue;
        u32 new_events;

        u32 frequency;
        volatile u32 last_reload;
        volatile u32 reload;
        volatile u32 match;
        volatile u64 sequence;
        volatile struct dmtimer_pps_event latest_event;
        volatile struct dmtimer_pps_event latest_capture_event;
        volatile struct dmtimer_pps_params params;
};

static int dmtimer_pps_set_params(struct dmtimer_pps *self, struct dmtimer_pps_params* params)
{
        u32 l;

        pr_info("dmtimer pps: set_params mode(%d), period(%u), adjust(%u)\n", params->mode, params->period, params->adjust);

        spin_lock_irq(&self->lock);

        if(params->mode != self->params.mode) {
                l = self->dmtimer->context.tclr;
                l &= ~(OMAP_TIMER_CTRL_GPOCFG | OMAP_TIMER_CTRL_TCM_BOTHEDGES);

                switch(params->mode) {
                case DMTIMER_PPS_MODE_INPUT_CAPTURE_RISING:
                        l |= OMAP_TIMER_CTRL_GPOCFG;
                        l |= OMAP_TIMER_CTRL_TCM_LOWTOHIGH;
                        break;
                case DMTIMER_PPS_MODE_INPUT_CAPTURE_FALLING:
                        l |= OMAP_TIMER_CTRL_GPOCFG;
                        l |= OMAP_TIMER_CTRL_TCM_HIGHTOLOW;
                        break;
                // setup as output by default.
                }
                l |= OMAP_TIMER_CTRL_ST;
                
                __omap_dm_timer_write(self->dmtimer, OMAP_TIMER_CTRL_REG, l, self->dmtimer->posted);

                // Save the context
                self->dmtimer->context.tclr = l;
                self->params.mode = params->mode;
        }

        self->params.period = params->period;
        self->params.adjust = params->adjust;

        spin_unlock_irq(&self->lock);

        return 0;
}

static int dmtimer_pps_start(struct dmtimer_pps *self)
{
        struct clk *dmtimer_clk = NULL;
        u32 l, imask;

        // make sure the timer is stopped
        if (pm_runtime_active(&self->dmtimer_pdev->dev)) {
                omap_dm_timer_stop(self->dmtimer);
        }

        omap_dm_timer_set_source(self->dmtimer, OMAP_TIMER_SRC_SYS_CLK);
        dmtimer_clk = omap_dm_timer_get_fclk(self->dmtimer);
        if(!dmtimer_clk) {
                pr_err("dmtimer-pps: couldn't get dmtimer fclk");
                return -EINVAL;
        }

        pr_info("dmtimer-pps: probed %s, clk(%ld hz)\n", self->timer_name, clk_get_rate(dmtimer_clk));
        
        // setup the control for the interrupt
        self->frequency = clk_get_rate(dmtimer_clk);
        self->params.frequency = self->frequency;
        self->params.period = self->frequency;
        self->params.adjust = 0;

        self->reload = (DM_TIMER_MAX - self->frequency) + 1;
        self->match = (DM_TIMER_MAX - self->frequency / 2) + 1;

        // turn the timer on (not started yet)
        omap_dm_timer_enable(self->dmtimer);
        __omap_dm_timer_write(self->dmtimer, OMAP_TIMER_LOAD_REG, self->reload, self->dmtimer->posted);
        self->dmtimer->context.tldr = self->reload;
        __omap_dm_timer_write(self->dmtimer, OMAP_TIMER_MATCH_REG, self->match, self->dmtimer->posted);
        self->dmtimer->context.tmar = self->match;
        __omap_dm_timer_write(self->dmtimer, OMAP_TIMER_COUNTER_REG, DM_TIMER_LOAD_MIN, self->dmtimer->posted);
        self->dmtimer->context.tcrr = DM_TIMER_LOAD_MIN;

        // setup the configuration and load the timer
        l = __omap_dm_timer_read(self->dmtimer, OMAP_TIMER_CTRL_REG, self->dmtimer->posted);
        l = 0;
        l |= OMAP_TIMER_CTRL_GPOCFG; // set the timer pin as an input
        l |= OMAP_TIMER_CTRL_PT; // toggle the output pin, instead of pulsing it
        l |= OMAP_TIMER_TRIGGER_OVERFLOW_AND_COMPARE << 10; // toggle on overflow and match
        l |= OMAP_TIMER_CTRL_TCM_LOWTOHIGH; // capture on rising edge
        l |= OMAP_TIMER_CTRL_CE; // enable compare mode
        l |= OMAP_TIMER_CTRL_AR; // enable auto reload
        l |= OMAP_TIMER_CTRL_ST; // start the timer
        __omap_dm_timer_write(self->dmtimer, OMAP_TIMER_CTRL_REG, l, self->dmtimer->posted);
        self->dmtimer->context.tclr = l;

        // enable the interrupts
        imask = OMAP_TIMER_INT_CAPTURE | OMAP_TIMER_INT_OVERFLOW;
        __omap_dm_timer_int_enable(self->dmtimer, imask);
        self->dmtimer->context.tier = imask;
        self->dmtimer->context.twer = imask;

        return 0;
}

static irqreturn_t dmtimer_pps_interrupt(int irq, void* data) {
        struct dmtimer_pps *self = data;
        unsigned int irq_status;
        unsigned int count;
        unsigned int capture;
        struct system_time_snapshot snap;
        unsigned long flags;
        u32 last_period;
        u32 period;

        if (unlikely(!self->dmtimer)) {
                panic("%s no dmtimer in interrupt", self->timer_name);
        }
        if (unlikely(pm_runtime_suspended(&self->dmtimer->pdev->dev))) {
                panic("%s suspended in interrupt", self->timer_name);
        }

        // get all the irq flags so we can handle them accordingly
        // this comes before taking the snapshot so we never have a count from before the overflow for overflow interrupt
        irq_status = omap_dm_timer_read_status(self->dmtimer);

        // get a system time snapshot and the couter value close to each other.
        ktime_get_snapshot(&snap);
        count = omap_dm_timer_read_counter(self->dmtimer);
        
        last_period = (DM_TIMER_MAX - self->last_reload) + 1;
        period = (DM_TIMER_MAX - self->reload) + 1;

        if(irq_status & OMAP_TIMER_INT_OVERFLOW) {
                spin_lock_irqsave(&self->lock, flags);
                self->sequence += last_period;
                self->last_reload = self->reload;
                self->reload = (DM_TIMER_MAX - (self->params.period + self->params.adjust)) + 1;
                self->match = (DM_TIMER_MAX - (period / 2)) + 1;
                self->params.adjust = 0;
                self->new_events += 1;

                self->latest_event.type = DMTIMER_PPS_TYPE_OVERFLOW;
                self->latest_event.event_seq = self->sequence;
                self->latest_event.irq_seq = self->sequence + (count - self->last_reload);
                self->latest_event.irq_realtime = ktime_to_ns(snap.real);
                spin_unlock_irqrestore(&self->lock, flags);

                __omap_dm_timer_write(self->dmtimer, OMAP_TIMER_LOAD_REG, self->reload, self->dmtimer->posted);
                __omap_dm_timer_write(self->dmtimer, OMAP_TIMER_MATCH_REG, self->match, self->dmtimer->posted);

                wake_up_interruptible_all(&self->queue);
        }
        if(irq_status & OMAP_TIMER_INT_CAPTURE) {
                capture = __omap_dm_timer_read(self->dmtimer, OMAP_TIMER_CAPTURE_REG, self->dmtimer->posted);
                
                spin_lock_irqsave(&self->lock, flags);
                self->new_events += 1;
                self->latest_capture_event.type = DMTIMER_PPS_TYPE_CAPTURE;
                self->latest_capture_event.irq_realtime = ktime_to_ns(snap.real);
                // figure out what the sequences are
                if(irq_status & OMAP_TIMER_INT_OVERFLOW) {
                        // irq happened with overflow already handled
                        // the overflow handler has figured out the irq sequence for us
                        self->latest_capture_event.irq_seq = self->latest_event.irq_seq;
                        if(capture > count) {
                                // capture happened before the reload
                                self->latest_capture_event.event_seq = self->sequence - ((DM_TIMER_MAX - capture) + 1);
                        } else {
                                // capture happened after the reload
                                self->latest_capture_event.event_seq = self->sequence + (capture - self->last_reload);
                        }
                } else {
                        // irq happened without overflow already handled
                        self->latest_capture_event.event_seq = self->sequence + (capture - self->last_reload);
                        if(capture > count) {
                                // irq count happened after a reload
                                self->latest_capture_event.irq_seq = self->sequence + last_period + (count - self->reload);
                        } else {
                                // irq count happened before a reload
                                self->latest_capture_event.irq_seq = self->sequence + (count - self->last_reload);
                        }
                }
                spin_unlock_irqrestore(&self->lock, flags);

                wake_up_interruptible_all(&self->queue);
        }

        __omap_dm_timer_write_status(self->dmtimer, irq_status);

        return IRQ_HANDLED;

}

static int dmtimer_pps_cdev_open(struct inode *inode, struct file *file)
{
        struct dmtimer_pps *self = container_of(inode->i_cdev,
                                struct dmtimer_pps, cdev);
        file->private_data = self;
        kobject_get(&self->cdev_dev->kobj);
        return 0;
}

static int dmtimer_pps_cdev_release(struct inode *inode, struct file *file)
{
        struct dmtimer_pps *self = container_of(inode->i_cdev,
                                struct dmtimer_pps, cdev);
        kobject_put(&self->cdev_dev->kobj);
        return 0;
}

static long dmtimer_pps_cdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
        struct dmtimer_pps *self = file->private_data;
        void __user *uarg = (void __user *) arg;
        struct dmtimer_pps_params params;
        // don't leak kernel memory
        memset(&params, 0, sizeof(params));

        switch(cmd) {
        case DMTIMER_PPS_GETPARAMS:
                spin_lock_irq(&self->lock);
                params = self->params;
                spin_unlock_irq(&self->lock);

                if(0 != copy_to_user(uarg, &params, sizeof(params))) {
                        return -EFAULT;
                }
                break;

        case DMTIMER_PPS_SETPARAMS:
                if(0 != copy_from_user(&params, uarg, sizeof(params))) {
                        return -EFAULT;
                }

                dmtimer_pps_set_params(self, &params);
                break;

        default:
                return -ENOTTY;
        }

        return 0;
}

static ssize_t dmtimer_pps_cdev_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
        struct dmtimer_pps *self = file->private_data;
        struct dmtimer_pps_event event;
        struct dmtimer_pps_event capture_event;

        if(count >= sizeof(event)) {
                spin_lock_irq(&self->lock);
                event = self->latest_event;
                capture_event = self->latest_capture_event;
                self->new_events = 0;
                spin_unlock_irq(&self->lock);

                if(0 != copy_to_user(buf, &event, sizeof(event))) {
                        return -EFAULT;
                }

                if(count >= (sizeof(event) + sizeof(capture_event))) {
                        if(0 == copy_to_user(buf + sizeof(event), &capture_event, sizeof(capture_event))) {
                                return sizeof(event) + sizeof(capture_event);
                        } else {
                                return -EFAULT;
                        }
                } else {
                        return sizeof(event);
                }
        } else {
                return -EINVAL;
        }
}

unsigned int dmtimer_pps_cdev_poll(struct file *file, struct poll_table_struct *wait)
{
        struct dmtimer_pps *self = file->private_data;

        poll_wait(file, &self->queue, wait);

        if(self->new_events) {
                return POLLIN | POLLRDNORM;
        } else {
                return 0;
        }

}

static const struct file_operations dmtimer_pps_cdev_fops = {
        .owner          = THIS_MODULE,
        .open           = dmtimer_pps_cdev_open,
        .release        = dmtimer_pps_cdev_release,
        .unlocked_ioctl = dmtimer_pps_cdev_ioctl,
        .read           = dmtimer_pps_cdev_read,
        .llseek         = no_llseek,
        .poll           = dmtimer_pps_cdev_poll
};

static int dmtimer_pps_probe(struct platform_device *pdev) {
        struct device_node *dn = pdev->dev.of_node;
        struct device_node *timer_dn = NULL;
        struct dmtimer_pps *self = NULL;
        dev_t chardev;

        self = devm_kzalloc(&pdev->dev, sizeof(struct dmtimer_pps), GFP_KERNEL);
        if (!self) {
                return -ENOMEM;
        }

        self->pdev = pdev;

        timer_dn = of_parse_phandle(dn, "ti,timers", 0);
        if(!timer_dn) {
                pr_err("dmtimer_pps: failed to find the timer\n");
                return -EINVAL;
        }

        self->dmtimer_pdev = of_find_device_by_node(timer_dn);
        if(!self->dmtimer_pdev) {
                pr_err("dmtimer_pps: couldn't find the timer pdev\n");
                goto probe_fail;
        }

        self->dmtimer = omap_dm_timer_request_by_node(timer_dn);
        if(!self->dmtimer) {
                pr_err("dmtimer_pps: couldn't find the dmtimer from the platform\n");
                goto probe_fail;
        }

        if(0 != of_property_read_string_index(timer_dn, "ti,hwmods", 0, &self->timer_name)) {
                pr_err("dmtimer_pps: the timer obj doesn't have \"ti,hwmods\"\n");
                goto probe_fail;
        }
        if(sscanf(self->timer_name, "timer%d", &self->timer_id) < 1) {
                pr_err("dmtimer_pps: unable to parse the timer_name %s\n", self->timer_name);
                goto probe_fail;
        }

        init_waitqueue_head(&self->queue);
        spin_lock_init(&self->lock);

        // make the character device
        chardev = MKDEV(MAJOR(dmtimer_pps_chardevs), self->timer_id);
        cdev_init(&self->cdev, &dmtimer_pps_cdev_fops);
        self->cdev.owner = THIS_MODULE;
        if(0 != cdev_add(&self->cdev, chardev, 1)) {
                pr_err("dmtimer_pps: unable to add the cdev\n");
                goto probe_fail;
        }
        self->cdev_dev = device_create(dmtimer_pps_class, NULL, chardev, self, "dmtimer%d", self->timer_id);
        if (IS_ERR(self->cdev_dev)) {
                cdev_del(&self->cdev);
                goto probe_fail;
        }

        // register the irq
        self->irq = omap_dm_timer_get_irq(self->dmtimer);
        if(devm_request_irq(&pdev->dev, self->irq, dmtimer_pps_interrupt, IRQF_TIMER, self->timer_name, self) < 0) {
                dev_err(&pdev->dev, "failed to request the irq for %s\n", self->timer_name);
                goto probe_fail;
        }

        if(0 == dmtimer_pps_start(self)) {
                platform_set_drvdata(pdev, self);
                return 0;
        }

probe_fail:
        if(!(IS_ERR(self->cdev_dev))) {
                device_destroy(dmtimer_pps_class, chardev);
                cdev_del(&self->cdev);
        }

        // we failed the initialization, free what we need to
        if(self->dmtimer) {
                omap_dm_timer_free(self->dmtimer);
        }

        return -EINVAL;
}

static int dmtimer_pps_remove(struct platform_device *pdev) {
        struct dmtimer_pps* self = platform_get_drvdata(pdev);

        device_destroy(dmtimer_pps_class, MKDEV(MAJOR(dmtimer_pps_chardevs), self->timer_id));
        cdev_del(&self->cdev);

        // handle removing the timer
        if (pm_runtime_active(&self->dmtimer_pdev->dev)) {
                omap_dm_timer_stop(self->dmtimer);
                omap_dm_timer_set_int_disable(self->dmtimer, OMAP_TIMER_INT_CAPTURE | OMAP_TIMER_INT_OVERFLOW | OMAP_TIMER_INT_MATCH);
        }
        omap_dm_timer_free(self->dmtimer);

    return 0;
}

static const struct of_device_id dmtimer_pps_of_match[] = {
        {.compatible = "dmtimer-pps"},
        {}
};
MODULE_DEVICE_TABLE(of, dmtimer_pps_of_match);

static struct platform_driver dmtimer_pps_driver = {
        .driver = {
                .name = "dmtimer-pps",
                .of_match_table = dmtimer_pps_of_match,
        },
        .probe = dmtimer_pps_probe,
        .remove = dmtimer_pps_remove,
};

static int __init dmtimer_pps_init(void)
{
        int err;
        err = alloc_chrdev_region(&dmtimer_pps_chardevs, 0, DMTIMER_PPS_MAX_SOURCES, "dmtimerpps");
        if (err < 0) {
                pr_err("failed to allocate char device region\n");
                return err;
        }

        dmtimer_pps_class = class_create(THIS_MODULE, "dmtimerpps");
        if (IS_ERR(dmtimer_pps_class)) {
                pr_err("failed to create a sysfs class for dmtimer_pps\n");
                unregister_chrdev_region(dmtimer_pps_chardevs, DMTIMER_PPS_MAX_SOURCES);
                return -ENOMEM;
        }

        platform_driver_register(&dmtimer_pps_driver);

        return 0;
}
module_init(dmtimer_pps_init);

static void __exit dmtimer_pps_exit(void)
{
        platform_driver_unregister(&dmtimer_pps_driver);
        class_destroy(dmtimer_pps_class);
        unregister_chrdev_region(dmtimer_pps_chardevs, DMTIMER_PPS_MAX_SOURCES);
}
module_exit(dmtimer_pps_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jonathan Herbst <ext_jonathan.herbst@hbkworld.com>");
MODULE_DESCRIPTION("DMTimer PPS Driver for Linux 4.9");