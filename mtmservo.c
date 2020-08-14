// SPDX-License-Identifier: GPL-2.0
/**
 * mtmservo stepper motor driver
 *
 * Copyright 2020 Pawel Skrzypiec <pawel.skrzypiec@agh.edu.pl>
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <linux/gpio/consumer.h>

struct mtmservo {
    struct platform_device *dev;
    struct workqueue_struct *workqueue;
    struct work_struct work;
    struct gpio_descs *ems;
    struct gpio_desc *det;
    unsigned long pos;
    atomic_t freq;
    atomic_t dst_pos;
    atomic_t calibration;
};

static void mtmservo_set_active_electromagnet(struct mtmservo *mtmservo, const int id)
{
    int i;

    for (i = 0; i < mtmservo->ems->ndescs; ++i)
        gpiod_set_value(mtmservo->ems->desc[i], (i == id));
}

static void mtmservo_stepping_work(struct work_struct *work)
{
    struct mtmservo *mtmservo = container_of(work, struct mtmservo, work);
    unsigned int period = 1000 / atomic_read(&mtmservo->freq);
    int dst_pos = atomic_read(&mtmservo->dst_pos);

    while (mtmservo->pos != dst_pos) {
        if (mtmservo->pos < dst_pos)
            ++mtmservo->pos;
        else
            --mtmservo->pos;

        mtmservo_set_active_electromagnet(mtmservo, mtmservo->pos % mtmservo->ems->ndescs);
        msleep(period);
    }
}

static void mtmservo_calibration_work(struct work_struct *work)
{
    struct mtmservo *mtmservo = container_of(work, struct mtmservo, work);
    unsigned int period = 1000 / atomic_read(&mtmservo->freq);

    while (!gpiod_get_value(mtmservo->det)) {
        --mtmservo->pos;
        mtmservo_set_active_electromagnet(mtmservo, mtmservo->pos % mtmservo->ems->ndescs);
        msleep(period);
    }

    mtmservo->pos = 0;
    atomic_set(&mtmservo->dst_pos, 0);
    atomic_set(&mtmservo->calibration, 0);
    dev_info(&mtmservo->dev->dev, "calibrated\n");
}

static ssize_t mtmservo_freq_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct mtmservo *mtmservo = (struct mtmservo *)dev->driver_data;

    return sprintf(buf, "%d\n", atomic_read(&mtmservo->freq));
}

static ssize_t mtmservo_freq_store(struct device *dev, struct device_attribute *attr,
    const char *buf, size_t size)
{
    struct mtmservo *mtmservo = (struct mtmservo *)dev->driver_data;
    int err, val;

    err = kstrtoint(buf, 0, &val);
    if (err)
        dev_err(dev, "failed to set freq\n");
    else if (val == 0 || val > 1000)
        dev_err(dev, "freq should be in range 1-1000\n");
    else
        atomic_set(&mtmservo->freq, val);
    return size;
}

static DEVICE_ATTR(frequency, S_IWUSR | S_IRUSR, mtmservo_freq_show, mtmservo_freq_store);

static ssize_t mtmservo_pos_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct mtmservo *mtmservo = (struct mtmservo *)dev->driver_data;

    return sprintf(buf, "%ld\n", mtmservo->pos);
}

static DEVICE_ATTR(position, S_IRUSR, mtmservo_pos_show, NULL);

static ssize_t mtmservo_dst_pos_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct mtmservo *mtmservo = (struct mtmservo *)dev->driver_data;

    return sprintf(buf, "%d\n", atomic_read(&mtmservo->dst_pos));
}

static ssize_t mtmservo_dst_pos_store(struct device *dev, struct device_attribute *attr,
    const char *buf, size_t size)
{
    struct mtmservo *mtmservo = (struct mtmservo *)dev->driver_data;
    int err, val;

    err = kstrtoint(buf, 0, &val);
    if (err) {
        dev_err(dev, "failed to set pos\n");
    }
    else {
        if (mtmservo->pos != val) {
            atomic_set(&mtmservo->dst_pos, val);
            INIT_WORK(&mtmservo->work, mtmservo_stepping_work);
            queue_work(mtmservo->workqueue, &mtmservo->work);
        }
    }
    return size;
}

static DEVICE_ATTR(dst_position, S_IWUSR | S_IRUSR, mtmservo_dst_pos_show, mtmservo_dst_pos_store);

static ssize_t mtmservo_cal_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct mtmservo *mtmservo = (struct mtmservo *)dev->driver_data;

    return sprintf(buf, "%d\n", atomic_read(&mtmservo->calibration));
}

static ssize_t mtmservo_cal_store(struct device *dev, struct device_attribute *attr,
    const char *buf, size_t size)
{
    struct mtmservo *mtmservo = (struct mtmservo *)dev->driver_data;
    int err, val;

    err = kstrtoint(buf, 0, &val);
    if (err) {
        dev_err(dev, "failed to set pos\n");
    }
    else {
        atomic_set(&mtmservo->calibration, 1);
        INIT_WORK(&mtmservo->work, mtmservo_calibration_work);
        queue_work(mtmservo->workqueue, &mtmservo->work);
    }
    return size;
}

static DEVICE_ATTR(calibration, S_IWUSR | S_IRUSR, mtmservo_cal_show, mtmservo_cal_store);

static ssize_t mtmservo_det_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct mtmservo *mtmservo = (struct mtmservo *)dev->driver_data;
    int val;

    val = gpiod_get_value(mtmservo->det);
    return sprintf(buf, "%d\n", val);
}

static DEVICE_ATTR(detector, S_IRUSR, mtmservo_det_show, NULL);

static int mtmservo_probe(struct platform_device *dev)
{
    int err;
    struct mtmservo *mtmservo;

    mtmservo = devm_kzalloc(&dev->dev, sizeof(struct mtmservo), GFP_KERNEL);
    if (!mtmservo) {
        dev_err(&dev->dev, "failed to allocate struct mtmservo\n");
        err = -ENOMEM;
        goto out_ret_err;
    }

    mtmservo->dev = dev;
    platform_set_drvdata(dev, mtmservo);

    mtmservo->ems = gpiod_get_array(&dev->dev, "ems", GPIOD_OUT_LOW);
    if (IS_ERR(mtmservo->ems)) {
        dev_err(&dev->dev, "failed to allocate ems\n");
        err = PTR_ERR(mtmservo->ems);
        goto out_ret_err;
    }

    mtmservo->det = gpiod_get(&dev->dev, "det", GPIOD_IN);
    if (IS_ERR(mtmservo->det)) {
        dev_err(&dev->dev, "failed to allocate det\n");
        err = PTR_ERR(mtmservo->det);
        goto out_put_ems;
    }

    mtmservo->workqueue = create_singlethread_workqueue("mtmservo_workqueue");

    if (!mtmservo->workqueue) {
        dev_err(&dev->dev, "failed to allocate workqueue\n");
        err = -ENOMEM;
        goto out_put_det;
    }

    err = device_create_file(&dev->dev, &dev_attr_frequency);
    if (err) {
        dev_err(&dev->dev, "failed to create frequency attr file\n");
        goto out_destroy_workqueue;
    }

    err = device_create_file(&dev->dev, &dev_attr_position);
    if (err) {
        dev_err(&dev->dev, "failed to create position attr file\n");
        goto out_remove_freq_attr_file;
    }

    err = device_create_file(&dev->dev, &dev_attr_dst_position);
    if (err) {
        dev_err(&dev->dev, "failed to create dst_position attr file\n");
        goto out_remove_pos_attr_file;
    }

    err = device_create_file(&dev->dev, &dev_attr_calibration);
    if (err) {
        dev_err(&dev->dev, "failed to create calibration attr file\n");
        goto out_remove_dst_pos_attr_file;
    }

    err = device_create_file(&dev->dev, &dev_attr_detector);
    if (err) {
        dev_err(&dev->dev, "failed to create detector attr file\n");
        goto out_remove_calibration_attr_file;
    }

    atomic_set(&mtmservo->freq, 100);
    atomic_set(&mtmservo->calibration, 1);
    INIT_WORK(&mtmservo->work, mtmservo_calibration_work);
    queue_work(mtmservo->workqueue, &mtmservo->work);

    dev_info(&dev->dev, "probed\n");
    return 0;

out_remove_calibration_attr_file:
    device_remove_file(&dev->dev, &dev_attr_calibration);
out_remove_dst_pos_attr_file:
    device_remove_file(&dev->dev, &dev_attr_dst_position);
out_remove_pos_attr_file:
    device_remove_file(&dev->dev, &dev_attr_position);
out_remove_freq_attr_file:
    device_remove_file(&dev->dev, &dev_attr_frequency);
out_destroy_workqueue:
    flush_workqueue(mtmservo->workqueue);
    destroy_workqueue(mtmservo->workqueue);
out_put_det:
    gpiod_put(mtmservo->det);
out_put_ems:
    gpiod_put_array(mtmservo->ems);
out_ret_err:
    return err;
}

static int mtmservo_remove(struct platform_device *dev)
{
    struct mtmservo *mtmservo = platform_get_drvdata(dev);

    device_remove_file(&dev->dev, &dev_attr_detector);
    device_remove_file(&dev->dev, &dev_attr_calibration);
    device_remove_file(&dev->dev, &dev_attr_dst_position);
    device_remove_file(&dev->dev, &dev_attr_position);
    device_remove_file(&dev->dev, &dev_attr_frequency);
    flush_workqueue(mtmservo->workqueue);
    destroy_workqueue(mtmservo->workqueue);
    gpiod_put(mtmservo->det);
    gpiod_put_array(mtmservo->ems);

    dev_info(&dev->dev, "removed\n");
    return 0;
}

static const struct platform_device_id mtmservo_id[] = {
    { .name = "mtmservo" },
    { }
};
MODULE_DEVICE_TABLE(platform, mtmservo_id);

static struct platform_driver mtmservo_driver = {
    .id_table = mtmservo_id,
    .probe = mtmservo_probe,
    .remove = mtmservo_remove,
    .driver = {
        .name = "mtmservo",
        .owner = THIS_MODULE,
    },
};
module_platform_driver(mtmservo_driver);

MODULE_DESCRIPTION("Simple Raspberry Pi 3 platform driver for the stepper motor control");
MODULE_AUTHOR("Pawel Skrzypiec <pawel.skrzypiec@agh.edu.pl>");
MODULE_LICENSE("GPL v2");
