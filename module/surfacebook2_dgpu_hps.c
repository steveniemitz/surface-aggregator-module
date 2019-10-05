#include <linux/acpi.h>
#include <linux/gpio.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>

#include <linux/uaccess.h>


#define SB2_SHPS_DSM_REVISION	1
#define SB2_SHPS_DSM_GPU_STATE	0x05

static const guid_t SB2_SHPS_DSM_UUID =
	GUID_INIT(0x5515a847, 0xed55, 0x4b27, 0x83, 0x52, 0xcd,
	          0x32, 0x0e, 0x10, 0x36, 0x0a);

#define SB2_PARAM_PERM		(S_IRUGO | S_IWUSR)


static const struct acpi_gpio_params gpio_base_presence_int = { 0, 0, false };
static const struct acpi_gpio_params gpio_base_presence     = { 1, 0, false };
static const struct acpi_gpio_params gpio_dgpu_power_int    = { 2, 0, false };
static const struct acpi_gpio_params gpio_dgpu_power        = { 3, 0, false };
static const struct acpi_gpio_params gpio_dgpu_presence_int = { 4, 0, false };
static const struct acpi_gpio_params gpio_dgpu_presence     = { 5, 0, false };

static const struct acpi_gpio_mapping sb2_mshw0153_acpi_gpios[] = {
	{ "base_presence-int-gpio", &gpio_base_presence_int, 1 },
	{ "base_presence-gpio",     &gpio_base_presence,     1 },
	{ "dgpu_power-int-gpio",    &gpio_dgpu_power_int,    1 },
	{ "dgpu_power-gpio",        &gpio_dgpu_power,        1 },
	{ "dgpu_presence-int-gpio", &gpio_dgpu_presence_int, 1 },
	{ "dgpu_presence-gpio",     &gpio_dgpu_presence,     1 },
	{ },
};


enum sb2_dgpu_power {
	SB2_DGPU_POWER_OFF      = 0,
	SB2_DGPU_POWER_ON       = 1,

	__SB2_DGPU_POWER__START = 0,
	__SB2_DGPU_POWER__END   = 1,
};

enum sb2_param_dgpu_power {
	SB2_PARAM_DGPU_POWER_AS_IS    = -1,
	SB2_PARAM_DGPU_POWER_OFF      = SB2_DGPU_POWER_OFF,
	SB2_PARAM_DGPU_POWER_ON       = SB2_DGPU_POWER_ON,

	__SB2_PARAM_DGPU_POWER__START = -1,
	__SB2_PARAM_DGPU_POWER__END   = 1,
};

static const char* sb2_dgpu_power_str(enum sb2_dgpu_power power) {
	if (power == SB2_DGPU_POWER_OFF) {
		return "off";
	} else if (power == SB2_DGPU_POWER_ON) {
		return "on";
	} else {
		return "<invalid>";
	}
}


struct sb2_shps_driver_data {
	struct mutex dgpu_power_lock;
	enum sb2_dgpu_power dgpu_power;
};


static int __sb2_shps_dgpu_set_power(struct platform_device *pdev, enum sb2_dgpu_power power)
{
	struct sb2_shps_driver_data *drvdata = platform_get_drvdata(pdev);
	acpi_handle handle = ACPI_HANDLE(&pdev->dev);
	union acpi_object *result;
	union acpi_object param;

	param.type = ACPI_TYPE_INTEGER;
	param.integer.value = power == SB2_DGPU_POWER_ON;

	result = acpi_evaluate_dsm_typed(handle, &SB2_SHPS_DSM_UUID, SB2_SHPS_DSM_REVISION,
	                                 SB2_SHPS_DSM_GPU_STATE, &param, ACPI_TYPE_BUFFER);

	if (IS_ERR_OR_NULL(result)) {
		return result ? PTR_ERR(result) : -EFAULT;
	}

	if (result->buffer.length != 1 || result->buffer.pointer[0] != 0) {
		return -EIO;
	}

	drvdata->dgpu_power = power;

	printk(KERN_INFO "sb2_shps: dGPU power state set to \'%s\'\n", sb2_dgpu_power_str(power));

	ACPI_FREE(result);
	return 0;
}

static int sb2_shps_dgpu_set_power(struct platform_device *pdev, enum sb2_dgpu_power power)
{
	struct sb2_shps_driver_data *drvdata = platform_get_drvdata(pdev);
	int status = 0;

	if (power < __SB2_DGPU_POWER__START || power > __SB2_DGPU_POWER__END) {
		return -EINVAL;
	}

	mutex_lock(&drvdata->dgpu_power_lock);
	if (power != drvdata->dgpu_power) {
		status = __sb2_shps_dgpu_set_power(pdev, power);
	}
	mutex_unlock(&drvdata->dgpu_power_lock);

	return status;
}

static int sb2_shps_dgpu_force_power(struct platform_device *pdev, enum sb2_dgpu_power power)
{
	struct sb2_shps_driver_data *drvdata = platform_get_drvdata(pdev);
	int status;

	if (power < __SB2_DGPU_POWER__START || power > __SB2_DGPU_POWER__END) {
		return -EINVAL;
	}

	mutex_lock(&drvdata->dgpu_power_lock);
	status = __sb2_shps_dgpu_set_power(pdev, power);
	mutex_unlock(&drvdata->dgpu_power_lock);

	return status;
}


static int param_dgpu_power_set(const char *val, const struct kernel_param *kp)
{
	int power = SB2_PARAM_DGPU_POWER_OFF;
	int status;

	status = kstrtoint(val, 0, &power);
	if (status) {
		return status;
	}

	if (power < __SB2_PARAM_DGPU_POWER__START || power > __SB2_PARAM_DGPU_POWER__END) {
		return -EINVAL;
	}

	return param_set_int(val, kp);
}

static const struct kernel_param_ops param_dgpu_power_ops = {
	.set = param_dgpu_power_set,
	.get = param_get_int,
};

static int param_dgpu_power_init = SB2_PARAM_DGPU_POWER_OFF;
static int param_dgpu_power_exit = SB2_PARAM_DGPU_POWER_OFF;

module_param_cb(dgpu_power_init, &param_dgpu_power_ops, &param_dgpu_power_init, SB2_PARAM_PERM);
module_param_cb(dgpu_power_exit, &param_dgpu_power_ops, &param_dgpu_power_exit, SB2_PARAM_PERM);

MODULE_PARM_DESC(dgpu_power_init, "dGPU power state to be set on init (-1 as-is, 0: off [default], 1: on)");
MODULE_PARM_DESC(dgpu_power_exit, "dGPU power state to be set on exit (-1 as-is, 0: off [default], 1: on)");


static ssize_t dgpu_power_show(struct device *dev, struct device_attribute *attr, char *data)
{
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	struct sb2_shps_driver_data *drvdata = platform_get_drvdata(pdev);

	return sprintf(data, "%s\n", sb2_dgpu_power_str(drvdata->dgpu_power));
}

static ssize_t dgpu_power_store(struct device *dev, struct device_attribute *attr,
                                const char *data, size_t count)
{
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	bool power = false;
	int status;

	status = kstrtobool(data, &power);
	if (status) {
		return status;
	}

	if (power) {
		status = sb2_shps_dgpu_set_power(pdev, SB2_DGPU_POWER_ON);
	} else {
		status = sb2_shps_dgpu_set_power(pdev, SB2_DGPU_POWER_OFF);
	}

	return status < 0 ? status : count;
}

const static DEVICE_ATTR_RW(dgpu_power);


static int sb2_shps_resume(struct device *dev)
{
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	struct sb2_shps_driver_data *drvdata = platform_get_drvdata(pdev);

	return sb2_shps_dgpu_force_power(pdev, drvdata->dgpu_power);
}

static int sb2_shps_probe(struct platform_device *pdev)
{
	struct sb2_shps_driver_data *drvdata;
	struct acpi_device *shps_dev = ACPI_COMPANION(&pdev->dev);
	int status = 0;

	if (gpiod_count(&pdev->dev, NULL) < 0) {
		return -ENODEV;
	}

	status = acpi_dev_add_driver_gpios(shps_dev, sb2_mshw0153_acpi_gpios);
	if (status) {
		return status;
	}

	drvdata = kzalloc(sizeof(struct sb2_shps_driver_data), GFP_KERNEL);
	if (!drvdata) {
		status = -ENOMEM;
		goto err_alloc_drvdata;
	}

	mutex_init(&drvdata->dgpu_power_lock);
	drvdata->dgpu_power = SB2_DGPU_POWER_OFF;
	platform_set_drvdata(pdev, drvdata);

	if (param_dgpu_power_init != SB2_PARAM_DGPU_POWER_AS_IS) {
		status = sb2_shps_dgpu_force_power(pdev, param_dgpu_power_init);
		if (status) {
			goto err_set_power;
		}
	}

	status = sysfs_create_file(&pdev->dev.kobj, &dev_attr_dgpu_power.attr);
	if (status) {
		goto err_sysfs;
	}

	return 0;

err_sysfs:
	sb2_shps_dgpu_force_power(pdev, SB2_DGPU_POWER_OFF);
err_set_power:
	platform_set_drvdata(pdev, NULL);
	kfree(drvdata);
err_alloc_drvdata:
	acpi_dev_remove_driver_gpios(shps_dev);
	return status;
}

static int sb2_shps_remove(struct platform_device *pdev)
{
	struct sb2_shps_driver_data *drvdata = platform_get_drvdata(pdev);
	struct acpi_device *shps_dev = ACPI_COMPANION(&pdev->dev);

	sysfs_remove_file(&pdev->dev.kobj, &dev_attr_dgpu_power.attr);

	if (param_dgpu_power_exit != SB2_PARAM_DGPU_POWER_AS_IS) {
		sb2_shps_dgpu_set_power(pdev, param_dgpu_power_exit);
	}
	acpi_dev_remove_driver_gpios(shps_dev);

	platform_set_drvdata(pdev, NULL);
	kfree(drvdata);

	return 0;
}


static SIMPLE_DEV_PM_OPS(sb2_shps_pm_ops, NULL, sb2_shps_resume);

static const struct acpi_device_id sb2_shps_acpi_match[] = {
	{ "MSHW0153", 0 },
	{ },
};
MODULE_DEVICE_TABLE(acpi, sb2_shps_acpi_match);

static struct platform_driver sb2_shps_driver = {
	.probe = sb2_shps_probe,
	.remove = sb2_shps_remove,
	.driver = {
		.name = "sb2_shps",
		.acpi_match_table = ACPI_PTR(sb2_shps_acpi_match),
		.pm = &sb2_shps_pm_ops,
	},
};
module_platform_driver(sb2_shps_driver);

MODULE_AUTHOR("Maximilian Luz <luzmaximilian@gmail.com>");
MODULE_DESCRIPTION("Surface Book 2 Hot-Plug System Driver");
MODULE_LICENSE("GPL v2");
