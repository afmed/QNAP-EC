/*
 * Copyright (C) 2021 Stonyx
 * https://www.stonyx.com/
 *
 * This driver is free software. You can redistribute it and/or modify it under the terms of the
 * GNU General Public License Version 3 (or at your option any later version) as published by The
 * Free Software Foundation.
 *
 * This driver is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * the GNU General Public License for more details.
 *
 * If you did not received a copy of the GNU General Public License along with this script see
 * http://www.gnu.org/copyleft/gpl.html or write to The Free Software Foundation, 675 Mass Ave,
 * Cambridge, MA 02139, USA.
 */

#include <linux/fs.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include "qnap-ec-ioctl.h"

// Define the printk prefix
#undef pr_fmt
#define pr_fmt(fmt) "%s @ %s: " fmt, "qnap-ec", __FUNCTION__

// Define module details
MODULE_DESCRIPTION("QNAP EC Driver");
MODULE_VERSION("1.1.0");
MODULE_AUTHOR("Stonyx - https://www.stonyx.com/");
MODULE_LICENSE("GPL");
MODULE_PARM_DESC(skip_check, "Skip check for QNAP IT8528 E.C. chip");
MODULE_PARM_DESC(sim_pwm_enable, "Simulate pwmX_enable sysfs attribute");

// Define maximum number of possible channels
// Note: number of channels has to be multiples of 8 and less than 256 and is based on the switch
//       statements in the ec_sys_get_fan_status, ec_sys_get_fan_speed, ec_sys_get_fan_pwm, and
//       ec_sys_get_temperature functions in the libuLinux_hal.so library as decompiled by IDA and
//       rounded up to the nearest multiple of 32 to allow for future additions of channels in
//       those functions
#define QNAP_EC_NUMBER_OF_FAN_CHANNELS 64
#define QNAP_EC_NUMBER_OF_PWM_CHANNELS QNAP_EC_NUMBER_OF_FAN_CHANNELS
#define QNAP_EC_NUMBER_OF_TEMP_CHANNELS 64

// Define the devices structure
// Note: in order to use the container_of macro in the qnap_ec_misc_dev_open and 
//       qnap_ec_misc_dev_ioctl functions we need to make the misc_device member not a pointer
//       and in order to use the platform_device_alloc function (see note in the qnap_ec_init
//       function) we need to make the plat_device member a pointer
struct qnap_ec_devices {
  struct mutex misc_device_mutex;
  bool open_misc_device;
  struct miscdevice misc_device;
  struct platform_device* plat_device;
};

// Define the I/O control data structure
struct qnap_ec_data {
  struct mutex mutex;
  struct qnap_ec_devices* devices;
  struct qnap_ec_ioctl_command ioctl_command;
  uint8_t fan_channel_checked_field[QNAP_EC_NUMBER_OF_FAN_CHANNELS / 8];
  uint8_t fan_channel_valid_field[QNAP_EC_NUMBER_OF_FAN_CHANNELS / 8];
  uint8_t pwm_channel_checked_field[QNAP_EC_NUMBER_OF_PWM_CHANNELS / 8];
  uint8_t pwm_channel_valid_field[QNAP_EC_NUMBER_OF_PWM_CHANNELS / 8];
  uint8_t temp_channel_checked_field[QNAP_EC_NUMBER_OF_TEMP_CHANNELS / 8];
  uint8_t temp_channel_valid_field[QNAP_EC_NUMBER_OF_TEMP_CHANNELS / 8];
};

// Declare functions
static int __init qnap_ec_init(void);
static int __init qnap_ec_is_chip_present(void);
static int qnap_ec_probe(struct platform_device* platform_dev);
static umode_t qnap_ec_hwmon_is_visible(const void* const_data, enum hwmon_sensor_types type,
                                        u32 attribute, int channel);
static int qnap_ec_hwmon_read(struct device* dev, enum hwmon_sensor_types type, u32 attribute,
                              int channel, long* value);
static int qnap_ec_hwmon_write(struct device* dev, enum hwmon_sensor_types type, u32 attribute,
                               int channel, long value);
static bool qnap_ec_is_fan_channel_valid(struct qnap_ec_data* data, uint8_t channel);
static bool qnap_ec_is_pwm_channel_valid(struct qnap_ec_data* data, uint8_t channel);
static bool qnap_ec_is_temp_channel_valid(struct qnap_ec_data* data, uint8_t channel);
static int qnap_ec_is_pwm_channel_valid_read_pwm_values(struct qnap_ec_data* data, uint8_t channel,
                                                        uint8_t initial_pwm_values[],
                                                        uint8_t changed_pwm_values[]);
static int qnap_ec_call_helper(bool log_helper_error);
static int qnap_ec_misc_device_open(struct inode* inode, struct file* file);
static long int qnap_ec_misc_device_ioctl(struct file* file, unsigned int command,
                                          unsigned long argument);
static int qnap_ec_misc_device_release(struct inode* inode, struct file* file);
static void __exit qnap_ec_exit(void);

// Specifiy the initialization and exit functions
module_init(qnap_ec_init);
module_exit(qnap_ec_exit);

// Define the module parameters
static bool qnap_ec_val_pwm_channels = true;
static bool qnap_ec_sim_pwm_enable = false;
static bool qnap_ec_skip_check = false;
module_param_named(val_pwm_channels, qnap_ec_val_pwm_channels, bool, 0);
module_param_named(sim_pwm_enable, qnap_ec_sim_pwm_enable, bool, 0);
module_param_named(skip_check, qnap_ec_skip_check, bool, 0);

// Declare the platform driver structure pointer
static struct platform_driver* qnap_ec_plat_driver;

// Declare the devices structure pointer
static struct qnap_ec_devices* qnap_ec_devices;

// Function called to initialize the driver
static int __init qnap_ec_init(void)
{
  // Define static constant data consisting of the miscellaneous device file operations structure
  static const struct file_operations misc_device_file_ops = {
    .owner = THIS_MODULE,
    .open = &qnap_ec_misc_device_open,
    .unlocked_ioctl = &qnap_ec_misc_device_ioctl,
    .release = &qnap_ec_misc_device_release
  };

  // Declare needed variables
  int error;

  // Check if the embedded controll chip isn't present
  error = qnap_ec_is_chip_present();
  if (error)
    return error;

  // Allocate memory for the platform driver structure and populate various fields
  qnap_ec_plat_driver = kzalloc(sizeof(struct platform_driver), GFP_KERNEL);
  if (qnap_ec_plat_driver == NULL)
    return -ENOMEM;
  qnap_ec_plat_driver->driver.owner = THIS_MODULE;
  qnap_ec_plat_driver->driver.name = "qnap-ec";
  qnap_ec_plat_driver->probe = &qnap_ec_probe;

  // Register the driver
  error = platform_driver_register(qnap_ec_plat_driver);
  if (error)
  {
    // Free the platform driver structure memory
    kfree(qnap_ec_plat_driver);

    return error;
  }

  // Allocate memory for the devices structure
  qnap_ec_devices = kzalloc(sizeof(struct qnap_ec_devices), GFP_KERNEL);
  if (qnap_ec_devices == NULL)
  {
    // Unregister the driver
    platform_driver_unregister(qnap_ec_plat_driver);

    // Free the platform driver structure memory
    kfree(qnap_ec_plat_driver);

    return -ENOMEM;
  }

  // Initialize the miscellaneous device mutex
  mutex_init(&qnap_ec_devices->misc_device_mutex);

  // Populate various miscellaneous device structure fields
  qnap_ec_devices->misc_device.name = "qnap-ec";
  qnap_ec_devices->misc_device.minor = MISC_DYNAMIC_MINOR;
  qnap_ec_devices->misc_device.fops = &misc_device_file_ops;

  // Register the miscellaneous device
  // Note: we need to register the miscellaneous device before registering the platform device so
  //       that the miscellaneous device is available for use by the various functions called by
  //       the probe function which is called when the platform device is registered
  error = misc_register(&qnap_ec_devices->misc_device);
  if (error)
  {
    // Free the devices structure memory
    kfree(qnap_ec_devices);

    // Unregister the platform driver
    platform_driver_unregister(qnap_ec_plat_driver);

    // Free the platform driver structure memory
    kfree(qnap_ec_plat_driver);

    return error;
  }

  // Allocate memory for the platform device structure and populate various fields
  // Note: we are using the platform_device_alloc function in combination with the
  //       platform_device_add function instead of the platform_device_register function because
  //       this approach is recommended for legacy type drivers that use hardware probing, all
  //       other hwmon drivers use this approach, and it provides a device release function for us
  qnap_ec_devices->plat_device = platform_device_alloc("qnap-ec", 0);
  if (qnap_ec_devices->plat_device == NULL)
  {
    // Unregister the miscellaneous device
    misc_deregister(&qnap_ec_devices->misc_device);

    // Free the devices structure memory
    kfree(qnap_ec_devices);

    // Unregister the driver
    platform_driver_unregister(qnap_ec_plat_driver);

    // Free the platform driver structure memory
    kfree(qnap_ec_plat_driver);

    return -ENOMEM;
  }
  qnap_ec_devices->plat_device->name = "qnap-ec";
  qnap_ec_devices->plat_device->id = PLATFORM_DEVID_NONE;

  // "Register" the platform device
  error = platform_device_add(qnap_ec_devices->plat_device);
  if (error)
  {
    // Free the platform device structure memory
    platform_device_put(qnap_ec_devices->plat_device);

    // Unregister the miscellaneous device
    misc_deregister(&qnap_ec_devices->misc_device);

    // Free the devices structure memory
    kfree(qnap_ec_devices);

    // Unregister the platform driver
    platform_driver_unregister(qnap_ec_plat_driver);

    // Free the platform driver structure memory
    kfree(qnap_ec_plat_driver);

    return error;
  }

  return 0;
}

// Function called to check if the QNAP embedded controller chip is present
static int __init qnap_ec_is_chip_present(void)
{
  // Declare needed variables
  uint8_t byte1;
  uint8_t byte2;

  // Check if we should skip the check
  if (qnap_ec_skip_check)
    return 0;

  // Request access to the input (0x2E) and output (0x2F) ports
  if (request_muxed_region(0x2E, 2, "qnap-ec") == NULL)
    return -EBUSY;

  // Write 0x20 to the input port
  outb(0x20, 0x2E);

  // Read the first identification byte from the output port
  byte1 = inb(0x2F);

  // Write 0x21 to the input port
  outb(0x21, 0x2E);

  // Read the second identification byte from the output port
  byte2 = inb(0x2F);

  // Check if the identification bytes do not match the expected values
  if (byte1 != 0x85 || byte2 != 0x28)
  {
    release_region(0x2E, 2);
    return -ENODEV;
  }

  // Release access to the input and output ports
  release_region(0x2E, 2);

  return 0;
}

// Function called to probe this driver
static int qnap_ec_probe(struct platform_device* platform_dev)
{
  // Define static non constant and constant data consisiting of mulitple configuration arrays,
  //   multiple hwmon channel info structures, the hwmon channel info structures array, and the
  //   hwmon chip information structure
  static u32 fan_config[QNAP_EC_NUMBER_OF_FAN_CHANNELS + 1];
  static u32 pwm_config[QNAP_EC_NUMBER_OF_PWM_CHANNELS + 1];
  static u32 temp_config[QNAP_EC_NUMBER_OF_TEMP_CHANNELS + 1];
  static const struct hwmon_channel_info fan_channel_info = {
    .type = hwmon_fan,
    .config = fan_config
  };
  static const struct hwmon_channel_info pwm_channel_info = {
    .type = hwmon_pwm,
    .config = pwm_config
  };
  static const struct hwmon_channel_info temp_channel_info = {
    .type = hwmon_temp,
    .config = temp_config
  };
  static const struct hwmon_channel_info* hwmon_channel_info[] = { &fan_channel_info,
    &pwm_channel_info, &temp_channel_info, NULL };
  static const struct hwmon_ops hwmon_ops = {
    .is_visible = &qnap_ec_hwmon_is_visible,
    .read = &qnap_ec_hwmon_read,
    .write = &qnap_ec_hwmon_write
  };
  static const struct hwmon_chip_info hwmon_chip_info = {
    .info = hwmon_channel_info,
    .ops = &hwmon_ops
  };

  // Declare needed variables
  uint8_t i;
  struct qnap_ec_data* data;
  struct device* device;

  // Allocate device managed memory for the data structure
  data = devm_kzalloc(&platform_dev->dev, sizeof(struct qnap_ec_data), GFP_KERNEL);
  if (data == NULL)
    return -ENOMEM;

  // Initialize the data mutex and set the devices pointer
  mutex_init(&data->mutex);
  data->devices = qnap_ec_devices;

  // Set the custom device data to the data structure
  // Note: this needs to be done before registering the hwmon device so that the data is accessible
  //       in the qnap_ec_is_visible function which is called when the hwmon device is registered
  dev_set_drvdata(&platform_dev->dev, data);

  // Populate the fan configuration array
  for (i = 0; i < QNAP_EC_NUMBER_OF_FAN_CHANNELS; ++i)
    fan_config[i] = HWMON_F_INPUT;
  fan_config[i] = 0;

  // Populate the PWM configuration array
  if (qnap_ec_sim_pwm_enable)
    for (i = 0; i < QNAP_EC_NUMBER_OF_PWM_CHANNELS; ++i)
      pwm_config[i] = HWMON_PWM_INPUT | HWMON_PWM_ENABLE;
  else
    for (i = 0; i < QNAP_EC_NUMBER_OF_PWM_CHANNELS; ++i)
      pwm_config[i] = HWMON_PWM_INPUT;
  pwm_config[i] = 0;

  // Populate the temperature configuration array
  for (i = 0; i < QNAP_EC_NUMBER_OF_TEMP_CHANNELS; ++i)
    temp_config[i] = HWMON_T_INPUT;
  temp_config[i] = 0;

  // Register the hwmon device and pass in the data structure
  // Note: hwmon device name cannot contain dashes
  device = devm_hwmon_device_register_with_info(&platform_dev->dev, "qnap_ec", data,
    &hwmon_chip_info, NULL);
  if (device == NULL)
    return -ENOMEM;

  return 0;
}

// Function called to check if a hwmon attribute is visible
static umode_t qnap_ec_hwmon_is_visible(const void* const_data, enum hwmon_sensor_types type,
                                        u32 attribute, int channel)
{
  // Declare and/or define needed variables
  // Note: we are using the dev_get_drvdata function to get access to the data since we need a
  //       pointer to non constant data
  struct qnap_ec_data* data = dev_get_drvdata(&((const struct qnap_ec_data*)const_data)->devices->
    plat_device->dev);

  // Switch based on the sensor type
  // Note: we are using a switch statements to simplify possible future expansion
  switch (type)
  {
    case hwmon_fan:
      // Switch based on the sensor attribute
      switch (attribute)
      {
        case hwmon_fan_input:
          // Check if this fan channel is valid and make the input attribute read only
          if (qnap_ec_is_fan_channel_valid(data, channel))
            return S_IRUGO;

          break;
      }
      break;
    case hwmon_pwm:
      // Switch based on the sensor attribute
      switch (attribute)
      {
        case hwmon_pwm_enable:
          // Check if we are simulating the enable attribute and if this PWM channel is valid and
          //   make the enable attribute read/write
          if (qnap_ec_sim_pwm_enable && qnap_ec_is_pwm_channel_valid(data, channel))
            return S_IRUGO | S_IWUSR;

          break;
        case hwmon_pwm_input:
          // Check if this PWM channel is valid and make the input attribute read/write
          if (qnap_ec_is_pwm_channel_valid(data, channel))
            return S_IRUGO | S_IWUSR;

          break;
      }
      break;
    case hwmon_temp:
      // Switch based on the sensor attribute
      switch (attribute)
      {
        case hwmon_temp_input:
          // Check if this temperature input channel is valid and make it read only
          if (qnap_ec_is_temp_channel_valid(data, channel))
            return S_IRUGO;

          break;
      }
      break;
    // Dummy default cause to silence compiler warnings about not including all enums in switch
    //   statement
    default:
      break;
  }

  return 0;
}

// Function called to read from a hwmon attribute
static int qnap_ec_hwmon_read(struct device* device, enum hwmon_sensor_types type, u32 attribute,
                              int channel, long* value)
{
  // Declare and/or define needed variables
  struct qnap_ec_data* data = dev_get_drvdata(device);

  // Switch based on the sensor type
  // Note: we are using a switch statements to simplify possible future expansion
  switch (type)
  {
    case hwmon_fan:
      // Switch based on the sensor attribute
      switch (attribute)
      {
        case hwmon_fan_input:
          // Check if this fan channel is invalid
          if (!qnap_ec_is_fan_channel_valid(data, channel))
            return -EOPNOTSUPP;

          // Get the data mutex lock
          mutex_lock(&data->mutex);

          // Set the I/O control command structure fields for calling the ec_sys_get_fan_speed
          //   function in the libuLinux_hal library via the helper program
          // Note: "sizeof(((struct qnap_ec_ioctl_command*)0)->function_name)" statement is based
          //       on the FIELD_SIZEOF macro which was removed from the kernel
          data->ioctl_command.function_type = int8_func_uint8_uint32pointer;
          strncpy(data->ioctl_command.function_name, "ec_sys_get_fan_speed",
            sizeof(((struct qnap_ec_ioctl_command*)0)->function_name) - 1);
          data->ioctl_command.argument1_uint8 = channel;
          data->ioctl_command.argument2_uint32 = 0;

          break;
        default:
          return -EOPNOTSUPP;
      }
      break;
    case hwmon_pwm:
      // Switch based on the sensor attribute
      switch (attribute)
      {
        case hwmon_pwm_enable:
          // Check if we are not simulating the enable attribute or this PWM channel is invalid
          if (!qnap_ec_sim_pwm_enable || !qnap_ec_is_pwm_channel_valid(data, channel))
            return -EOPNOTSUPP;

          break;
        case hwmon_pwm_input:
          // Check if this PWM channel is invalid
          if (!qnap_ec_is_pwm_channel_valid(data, channel))
            return -EOPNOTSUPP;

          // Get the data mutex lock
          mutex_lock(&data->mutex);

          // Set the I/O control command structure fields for calling the ec_sys_get_fan_pwm
          //   function in the libuLinux_hal library via the helper program
          // Note: "sizeof(((struct qnap_ec_ioctl_command*)0)->function_name)" statement is based
          //       on the FIELD_SIZEOF macro which was removed from the kernel
          data->ioctl_command.function_type = int8_func_uint8_uint32pointer;
          strncpy(data->ioctl_command.function_name, "ec_sys_get_fan_pwm",
            sizeof(((struct qnap_ec_ioctl_command*)0)->function_name) - 1);
          data->ioctl_command.argument1_uint8 = channel;
          data->ioctl_command.argument2_uint32 = 0;

          break;
        default:
          return -EOPNOTSUPP;
      }
      break;
    case hwmon_temp:
      // Switch based on the sensor attribute
      switch (attribute)
      {
        case hwmon_temp_input:
          // Check if this temperature channel is invalid
          if (!qnap_ec_is_temp_channel_valid(data, channel))
            return -EOPNOTSUPP;

          // Get the data mutex lock
          mutex_lock(&data->mutex);

          // Set the I/O control command structure fields for calling the ec_sys_get_temperature
          //   function in the libuLinux_hal library via the helper program
          // Note: "sizeof(((struct qnap_ec_ioctl_command*)0)->function_name)" statement is based
          //       on the FIELD_SIZEOF macro which was removed from the kernel
          // Note: we are using an int64 field in place of a double field since floating point
          //       math is not possible in kernel space
          data->ioctl_command.function_type = int8_func_uint8_doublepointer;
          strncpy(data->ioctl_command.function_name, "ec_sys_get_temperature",
            sizeof(((struct qnap_ec_ioctl_command*)0)->function_name) - 1);
          data->ioctl_command.argument1_uint8 = channel;
          data->ioctl_command.argument2_int64 = 0;

          break;
        default:
          return -EOPNOTSUPP;
      }
      break;
    default:
      return -EOPNOTSUPP;
  }

  // Set the open device flag to allow return communication by the helper program
  data->devices->open_misc_device = true;

  // Call the helper program
  if (qnap_ec_call_helper(true) != 0)
  {
    // Clear the open device flag
    data->devices->open_misc_device = false;

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return -ENODATA;
  }

  // Clear the open device flag
  data->devices->open_misc_device = false;

  // Check if the called function returned any errors
  if (data->ioctl_command.return_value_int8 != 0)
  {
    // Log the error
    printk(KERN_ERR "libuLinux_hal library %s function called by qnap-ec helper program returned "
      "a non zero value (%i)", data->ioctl_command.function_name,
      data->ioctl_command.return_value_int8);

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return -ENODATA;
  }

  // Switch based on the sensor type
  // Note: we are using a switch statements to match the switch statement above with the exception
  //       of error cases
  switch (type)
  {
    case hwmon_fan:
      // Switch based on the sensor attribute
      switch (attribute)
      {
        case hwmon_fan_input:
          // Set the value to the returned fan speed value
          *value = data->ioctl_command.argument2_uint32;

          break;
      }
      break;
    case hwmon_pwm:
      // Switch based on the sensor attribute
      switch (attribute)
      {
        case hwmon_pwm_enable:
          break;
        case hwmon_pwm_input:
          // Set the value to the returned fan PWM value
          *value = data->ioctl_command.argument2_uint32;

          break;
      }
      break;
    case hwmon_temp:
      // Switch based on the sensor attribute
      switch (attribute)
      {
        case hwmon_temp_input:
          // Set the value to the returned temperature value
          // Note: we are using an int64 field instead of a double field because floating point
          //       math is not possible in kernel space and because an int64 value can hold a 19
          //       digit integer while a double value can hold a 16 digit integer without loosing
          //       precision we can multiple the double value by 1000 to move three digits after
          //       the decimal point to before the decimal point and still fit the value in an
          //       int64 value and preserve three digits after the decimal point however because
          //       we need to return a millidegree value there is no need to divide by 1000
          *value = data->ioctl_command.argument2_int64;

          break;
      }
      break;
    // Dummy default cause to silence compiler warnings about not including all enums in switch
    //   statement
    default:
      break;
  }

  // Release the data mutex lock
  mutex_unlock(&data->mutex);

  return 0;
}

// Function called to write to a hwmon atrribute
static int qnap_ec_hwmon_write(struct device* device, enum hwmon_sensor_types type, u32 attribute,
                               int channel, long value)
{
  // Declare and/or define needed variables
  struct qnap_ec_data* data = dev_get_drvdata(device);

  // Switch based on the sensor type
  // Note: we are using a switch statement to simplify possible future expansion
  switch (type)
  {
    case hwmon_pwm:
      // Switch based on the sensor attribute
      switch (attribute)
      {
        case hwmon_pwm_enable:
          // Check if we are not simulating the pwm enable attribute or this PWM channel is invalid
          if (!qnap_ec_sim_pwm_enable || !qnap_ec_is_pwm_channel_valid(data, channel))
            return -EOPNOTSUPP;

          break;
        case hwmon_pwm_input:
          // Check if this PWM channel is invalid
          if (!qnap_ec_is_pwm_channel_valid(data, channel))
            return -EOPNOTSUPP;

          // Get the data mutex lock
          mutex_lock(&data->mutex);

          // Set the I/O control command structure fields for calling the ec_sys_set_fan_speed
          //   function in the libuLinux_hal library via the helper program
          // Note: "sizeof(((struct qnap_ec_ioctl_command*)0)->function_name)" statement is based
          //       on the FIELD_SIZEOF macro which was removed from the kernel
          data->ioctl_command.function_type = int8_func_uint8_uint8;
          strncpy(data->ioctl_command.function_name, "ec_sys_set_fan_speed",
            sizeof(((struct qnap_ec_ioctl_command*)0)->function_name) - 1);
          data->ioctl_command.argument1_uint8 = channel;
          data->ioctl_command.argument2_uint8 = value;

          break;
        default:
          return -EOPNOTSUPP;
      }
      break;
    default:
      return -EOPNOTSUPP;
  }

  // Set the open device flag to allow return communication by the helper program
  data->devices->open_misc_device = true;

  // Call the helper program
  if (qnap_ec_call_helper(true) != 0)
  {
    // Clear the open device flag
    data->devices->open_misc_device = false;

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    // Note: the "bad exchange" error code seems to be the closest code to describe this error
    //       condition (ie: the exchange of information between the kernel module and the helper
    //       program went bad)
    return -EBADE;
  }

  // Clear the open device flag
  data->devices->open_misc_device = false;

  // Check if the called function returned any errors
  if (data->ioctl_command.return_value_int8 != 0)
  {
    // Log the error
    printk(KERN_ERR "libuLinux_hal library %s function called by qnap-ec helper program returned "
      "a non zero value (%i)", data->ioctl_command.function_name,
      data->ioctl_command.return_value_int8);

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    // Note: the "bad exchange" error code seems to be the closest code to describe this error
    //       condition (ie: the exchange of information between the kernel module and the helper
    //       program went bad)
    return -EBADE;
  }

  // Release the data mutex lock
  mutex_unlock(&data->mutex);

  return 0;
}

// Function called to check if the fan channel number is valid
static bool qnap_ec_is_fan_channel_valid(struct qnap_ec_data* data, uint8_t channel)
{
  // Note: based on testing the logic to determining if a fan channel is valid is:
  //       - if channel is 10 or 11 then channel is invalid
  //       - call ec_sys_get_fan_status function in the libuLinux_hal library
  //       - if the function return value is non zero then the channel is invalid
  //       - if the returned fan status value is non zero then the channel is invalid
  //       - call ec_sys_get_fan_speed function in the libuLinux_hal library
  //       - if the function return value is non zero then the channel is invalid
  //       - if the returned fan speed value is 65535 then the channel is invalid
  //       - call ec_sys_get_fan_pwm function in the libuLinux_hal library
  //       - if the function return value is non zero then the channel is invalid
  //       - if the returned fan PWM value is greater than 255 then the channel is invalid

  // Check if this channel has already been checked
  if (((data->fan_channel_checked_field[channel / 8] >> (channel % 8)) & 0x01) == 1)
  {
    // Check if this channel is invalid
    if (((data->fan_channel_valid_field[channel / 8] >> (channel % 8)) & 0x01) == 0)
    {
      return false;
    }

    return true;
  }

  // Get the data mutex lock
  mutex_lock(&data->mutex);

  // Mark this channel as checked now so that we don't have to before each return statement
  data->fan_channel_checked_field[channel / 8] |= (0x01 << (channel % 8));

  // Set the I/O control command structure fields for calling the ec_sys_get_fan_status function
  //   in the libuLinux_hal library via the helper program
  // Note: "sizeof(((struct qnap_ec_ioctl_command*)0)->function_name)" statement is based on the
  //       FIELD_SIZEOF macro which was removed from the kernel
  data->ioctl_command.function_type = int8_func_uint8_uint32pointer;
  strncpy(data->ioctl_command.function_name, "ec_sys_get_fan_status",
    sizeof(((struct qnap_ec_ioctl_command*)0)->function_name) - 1);
  data->ioctl_command.argument1_uint8 = channel;
  data->ioctl_command.argument2_uint32 = 0;

  // Set the open device flag to allow return communication by the helper program
  data->devices->open_misc_device = true;

  // Call the helper program
  if (qnap_ec_call_helper(false) != 0)
  {
    // Clear the open device flag
    data->devices->open_misc_device = false;

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return false;
  }

  // Clear the open device flag
  data->devices->open_misc_device = false;

  // Check if the called function return value is non zero
  if (data->ioctl_command.return_value_int8 != 0)
  {
    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return false;
  }

  // Check if the returned status value is non zero
  if (data->ioctl_command.argument2_uint32 != 0)
  {
    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return false;
  }

  // Set the I/O control command structure fields for calling the ec_sys_get_fan_speed function
  //   in the libuLinux_hal library via the helper program
  // Note: "sizeof(((struct qnap_ec_ioctl_command*)0)->function_name)" statement is based on the
  //       FIELD_SIZEOF macro which was removed from the kernel
  data->ioctl_command.function_type = int8_func_uint8_uint32pointer;
  strncpy(data->ioctl_command.function_name, "ec_sys_get_fan_speed",
    sizeof(((struct qnap_ec_ioctl_command*)0)->function_name) - 1);
  data->ioctl_command.argument1_uint8 = channel;
  data->ioctl_command.argument2_uint32 = 0;

  // Set the open device flag to allow return communication by the helper program
  data->devices->open_misc_device = true;

  // Call the helper program
  if (qnap_ec_call_helper(false) != 0)
  {
    // Clear the open device flag
    data->devices->open_misc_device = false;

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return false;
  }

  // Clear the open device flag
  data->devices->open_misc_device = false;

  // Check if the called function return value is non zero
  if (data->ioctl_command.return_value_int8 != 0)
  {
    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return false;
  }

  // Check if the returned fan speed value is 65535
  if (data->ioctl_command.argument2_uint32 == 65535)
  {
    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return false;
  }

  // Set the I/O control command structure fields for calling the ec_sys_get_fan_pwm function
  //   in the libuLinux_hal library via the helper program
  // Note: "sizeof(((struct qnap_ec_ioctl_command*)0)->function_name)" statement is based on the
  //       FIELD_SIZEOF macro which was removed from the kernel
  data->ioctl_command.function_type = int8_func_uint8_uint32pointer;
  strncpy(data->ioctl_command.function_name, "ec_sys_get_fan_pwm",
    sizeof(((struct qnap_ec_ioctl_command*)0)->function_name) - 1);
  data->ioctl_command.argument1_uint8 = channel;
  data->ioctl_command.argument2_uint32 = 0;

  // Set the open device flag to allow return communication by the helper program
  data->devices->open_misc_device = true;

  // Call the helper program
  if (qnap_ec_call_helper(false) != 0)
  {
    // Clear the open device flag
    data->devices->open_misc_device = false;

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return false;
  }

  // Clear the open device flag
  data->devices->open_misc_device = false;

  // Check if the called function return value is non zero
  if (data->ioctl_command.return_value_int8 != 0)
  {
    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return false;
  }

  // Check if the returned fan PWM value is invalid
  if (data->ioctl_command.argument2_uint32 > 255)
  {
    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return false;
  }

  // Mark this channel as valid
  data->fan_channel_valid_field[channel / 8] |= (0x01 << (channel % 8));

  // Release the data mutex lock
  mutex_unlock(&data->mutex);

  return true;
}

// Function called to check if the PWM channel number is valid
static bool qnap_ec_is_pwm_channel_valid(struct qnap_ec_data* data, uint8_t channel)
{
  // Note: based on testing the logic to determine if a PWM channel is valid is:

  // Declare and/or define needed variables
  uint8_t i;
  bool valid_channel_marked = false;
  uint8_t initial_pwm_values[QNAP_EC_NUMBER_OF_PWM_CHANNELS];
  uint8_t changed_pwm_values[QNAP_EC_NUMBER_OF_PWM_CHANNELS];

  // Check if this channel has already been checked
  if (((data->pwm_channel_checked_field[channel / 8] >> (channel % 8)) & 0x01) == 1)
  {
    // Check if this channel is invalid
    if (((data->pwm_channel_valid_field[channel / 8] >> (channel % 8)) & 0x01) == 0)
    {
      return false;
    }

    return true;
  }

  // Get the data mutex lock
  mutex_lock(&data->mutex);

  // Read the intial PWM values
  if (qnap_ec_is_pwm_channel_valid_read_pwm_values(data, channel, initial_pwm_values, NULL) != 0)
  {
    // Mark this channel as checked
    data->pwm_channel_checked_field[channel / 8] |= (0x01 << (channel % 8));

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return false;
  }

  // Set the I/O control command structure fields for calling the ec_sys_set_fan_speed function in
  //   the libuLinux_hal library via the helper program in order to raise or lower the fan PWM
  //   value by 5 (~ 2%)
  // Note: "sizeof(((struct qnap_ec_ioctl_command*)0)->function_name)" statement is based on the
  //       FIELD_SIZEOF macro which was removed from the kernel
  data->ioctl_command.function_type = int8_func_uint8_uint8;
  strncpy(data->ioctl_command.function_name, "ec_sys_set_fan_speed",
    sizeof(((struct qnap_ec_ioctl_command*)0)->function_name) - 1);
  data->ioctl_command.argument1_uint8 = channel;
  if (initial_pwm_values[channel] <= 250)
    data->ioctl_command.argument2_uint8 = initial_pwm_values[channel] + 5;
  else
    data->ioctl_command.argument2_uint8 = initial_pwm_values[channel] - 5;

  // Set the open device flag to allow return communication by the helper program
  data->devices->open_misc_device = true;

  // Call the helper program
  if (qnap_ec_call_helper(false) != 0)
  {
    // Clear the open device flag
    data->devices->open_misc_device = false;

    // Mark this channel as checked
    data->pwm_channel_checked_field[channel / 8] |= (0x01 << (channel % 8));

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return false;
  }

  // Clear the open device flag
  data->devices->open_misc_device = false;

  // Check if the called function returned any errors
  if (data->ioctl_command.return_value_int8 != 0)
  {
    // Mark this channel as checked
    data->pwm_channel_checked_field[channel / 8] |= (0x01 << (channel % 8));

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return false;
  }

  // Read the changed PWM values
  if (qnap_ec_is_pwm_channel_valid_read_pwm_values(data, channel, initial_pwm_values,
      changed_pwm_values) != 0)
  {
    // Mark this channel as checked
    data->pwm_channel_checked_field[channel / 8] |= (0x01 << (channel % 8));

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return false;
  }

  // Set the I/O control command structure fields for calling the ec_sys_set_fan_speed function in
  //   the libuLinux_hal library via the helper program in order to reset the fan PWM value to the
  //   initial fan PWM value
  // Note: "sizeof(((struct qnap_ec_ioctl_command*)0)->function_name)" statement is based on the
  //       FIELD_SIZEOF macro which was removed from the kernel
  data->ioctl_command.function_type = int8_func_uint8_uint8;
  strncpy(data->ioctl_command.function_name, "ec_sys_set_fan_speed",
    sizeof(((struct qnap_ec_ioctl_command*)0)->function_name) - 1);
  data->ioctl_command.argument1_uint8 = channel;
  data->ioctl_command.argument2_uint8 = initial_pwm_values[channel];

  // Set the open device flag to allow return communication by the helper program
  data->devices->open_misc_device = true;

  // Call the helper program
  if (qnap_ec_call_helper(false) != 0)
  {
    // Clear the open device flag
    data->devices->open_misc_device = false;

    // Mark this channel as checked
    data->pwm_channel_checked_field[channel / 8] |= (0x01 << (channel % 8));

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return false;
  }

  // Clear the open device flag
  data->devices->open_misc_device = false;

  // Check if the called function returned any errors
  if (data->ioctl_command.return_value_int8 != 0)
  {
    // Mark this channel as checked
    data->pwm_channel_checked_field[channel / 8] |= (0x01 << (channel % 8));

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return false;
  }

  // Loop through all the channels
  for (i = 0; i < QNAP_EC_NUMBER_OF_PWM_CHANNELS; ++i)
  {
    // Check if this channel has already been checked
    if (((data->pwm_channel_checked_field[i / 8] >> (i % 8)) & 0x01) == 1)
      continue;

    // Check if this channel does not have the same intial fan PWM value as the initial fan PWM
    //   value of the channel this function was called for
    if (initial_pwm_values[i] != initial_pwm_values[channel])
      continue;

    // Check if this channel does not have the same changed fan PWM value as the changed fan PWM
    //   value of the channel this function was called for
    if (changed_pwm_values[i] != changed_pwm_values[channel])
      continue;

    // Mark this channel as checked
    data->pwm_channel_checked_field[i / 8] |= (0x01 << (i % 8));

    // Check if a valid channel has not yet been marked in this group
    if (!valid_channel_marked)
    {
      // Mark this channel as valid
      data->pwm_channel_valid_field[i / 8] |= (0x01 << (i % 8));

      // Set the marked flag
      valid_channel_marked = true;
    }
  }

  // Release the data mutex lock
  mutex_unlock(&data->mutex);

  // Check if this channel is invalid
  if (((data->pwm_channel_valid_field[channel / 8] >> (channel % 8)) & 0x01) == 0)
    return false;

  return true;
}

// Function called by the qnap_ec_is_pwm_channel_valid function to read the fan PWM values
static int qnap_ec_is_pwm_channel_valid_read_pwm_values(struct qnap_ec_data* data, uint8_t channel,
                                                        uint8_t initial_pwm_values[],
                                                        uint8_t changed_pwm_values[])
{
  // Declare needed variables
  uint8_t i;
  uint8_t j;

  // Loop through all the channels starting at the channel this function was called for
  for (i = 0, j = channel; i < QNAP_EC_NUMBER_OF_PWM_CHANNELS; ++i, j = (j + 1) % 64)
  {
    // Check if this channel has already been checked
    if (((data->pwm_channel_checked_field[j / 8] >> (j % 8)) & 0x01) == 1)
      continue;

    // Check if the changed PWM values pointer is not NULL (ie: this is the second read) and if
    //   this channel does not have the same initial fan PWM value as the initial fan PWM value of
    //   the channel that this function was called for
    if (changed_pwm_values != NULL && initial_pwm_values[j] != initial_pwm_values[channel])
      continue;

    // Set the I/O control command structure fields for calling the ec_sys_get_fan_pwm
    //   function in the libuLinux_hal library via the helper program
    // Note: "sizeof(((struct qnap_ec_ioctl_command*)0)->function_name)" statement is based
    //       on the FIELD_SIZEOF macro which was removed from the kernel
    data->ioctl_command.function_type = int8_func_uint8_uint32pointer;
    strncpy(data->ioctl_command.function_name, "ec_sys_get_fan_pwm",
      sizeof(((struct qnap_ec_ioctl_command*)0)->function_name) - 1);
    data->ioctl_command.argument1_uint8 = channel;
    data->ioctl_command.argument2_uint32 = 0;

    // Set the open device flag to allow return communication by the helper program
    data->devices->open_misc_device = true;

    // Call the helper program
    if (qnap_ec_call_helper(false) != 0)
    {
      // Clear the open device flag
      data->devices->open_misc_device = false;

      // Check if this is the channel that this function was called for and we should just
      //   return instead of continuing in the loop
      if (j == channel)
        return -ENODATA;

      // Mark this channel as checked (and invalid by default)
      data->pwm_channel_checked_field[j / 8] |= (0x01 << (j % 8));
      
      continue;
    }

    // Clear the open device flag
    data->devices->open_misc_device = false;

    // Check if the called function returned any errors
    if (data->ioctl_command.return_value_int8 != 0)
    {
      // Check if this is the channel that this function was called for and we should just
      //   return instead of continuing in the loop
      if (j == channel)
        return -ENODATA;

      // Mark this channel as checked (and invalid by default)
      data->pwm_channel_checked_field[j / 8] |= (0x01 << (j % 8));

      continue;
    }

    // Check if the changed PWM values pointer is NULL (ie: this is the first read) and set the
    //   appropriate array's value for this channel
    if (changed_pwm_values == NULL)
      initial_pwm_values[j] = data->ioctl_command.argument2_uint32;
    else
      changed_pwm_values[j] = data->ioctl_command.argument2_uint32;
  }

  return 0;
}

// Function called to check if the temperature channel number is valid
static bool qnap_ec_is_temp_channel_valid(struct qnap_ec_data* data, uint8_t channel)
{
  // Note: based on testing the logic to determining if a temperature channel is valid is:
  //       - if channel is 10 or 11 then channel is invalid
  //       - call ec_sys_get_temperature function in the libuLinux_hal library
  //       - if the function return value is non zero then the channel is invalid
  //       - if the returned temperature value is negative then the channel is invalid

  // Check if this channel has already been checked
  if (((data->temp_channel_checked_field[channel / 8] >> (channel % 8)) & 0x01) == 1)
  {
    // Check if this channel is invalid
    if (((data->temp_channel_valid_field[channel / 8] >> (channel % 8)) & 0x01) == 0)
    {
      return false;
    }

    return true;
  }

  // Get the data mutex lock
  mutex_lock(&data->mutex);

  // Mark this channel as checked now so that we don't have to before each return statement
  data->temp_channel_checked_field[channel / 8] |= (0x01 << (channel % 8));

  // Set the I/O control command structure fields for calling the ec_sys_get_temperature function
  //   in the libuLinux_hal library via the helper program
  // Note: "sizeof(((struct qnap_ec_ioctl_command*)0)->function_name)" statement is based on the
  //       FIELD_SIZEOF macro which was removed from the kernel
  // Note: we are using an int64 field in place of a double field since floating point math is not
  //       possible in kernel space
  data->ioctl_command.function_type = int8_func_uint8_doublepointer;
  strncpy(data->ioctl_command.function_name, "ec_sys_get_temperature",
    sizeof(((struct qnap_ec_ioctl_command*)0)->function_name) - 1);
  data->ioctl_command.argument1_uint8 = channel;
  data->ioctl_command.argument2_int64 = 0;

  // Set the open device flag to allow return communication by the helper program
  data->devices->open_misc_device = true;

  // Call the helper program
  if (qnap_ec_call_helper(false) != 0)
  {
    // Clear the open device flag
    data->devices->open_misc_device = false;

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return false;
  }

  // Clear the open device flag
  data->devices->open_misc_device = false;

  // Check if the called function return value is non zero
  if (data->ioctl_command.return_value_int8 != 0)
  {
    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return false;
  }

  // Check if the returned temperature value is negative
  if (data->ioctl_command.argument2_int64 < 0)
  {
    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return false;
  }

  // Mark this channel as valid
  data->temp_channel_valid_field[channel / 8] |= (0x01 << (channel % 8));

  // Release the data mutex lock
  mutex_unlock(&data->mutex);

  return true;
}

// Function called to call the user space helper program
// Note: the return value is the call_usermodehelper function's error code if an error code was
//       returned or if successful the return value is the user space helper program's error code
//       if an error code was returned or if successful the return value is zero
static int qnap_ec_call_helper(bool log_helper_error)
{
  // Declare and/or define needed variables
  uint8_t i = 0;
  int return_value;
#ifdef PACKAGE
  char* paths[] = { "/usr/sbin/qnap-ec", "/usr/bin/qnap-ec", "/sbin/qnap-ec", "/bin/qnap-ec" };
#else
  char* paths[] = { "/usr/local/sbin/qnap-ec", "/usr/local/bin/qnap-ec", "/usr/sbin/qnap-ec",
    "/usr/bin/qnap-ec", "/sbin/qnap-ec", "/bin/qnap-ec" };
#endif

  // Loop through the paths while the first 8 bits of the return value contain any error codes
  do
  {
    // Call the user space helper program
    return_value = call_usermodehelper(paths[i], (char*[]){ paths[i], NULL }, NULL, UMH_WAIT_PROC);
  } while ((return_value & 0xFF) != 0 && ++i < sizeof(paths) / sizeof(char*));

  // Check if the first 8 bits of the return value contain any error codes
  if ((return_value & 0xFF) != 0)
  {
    // Log the error
#ifdef PACKAGE
    printk(KERN_ERR "qnap-ec helper program not found at the expected path (%s) or any of the "
      "fall back paths (%s, %s, %s)", paths[0], paths[1], paths[2], paths[3]);
#else
    printk(KERN_ERR "qnap-ec helper program not found at the expected path (%s) or any of the "
      "fall back paths (%s, %s, %s, %s, %s)", paths[0], paths[1], paths[2], paths[3], paths[4],
      paths[5]);
#endif

    // Return the call_usermodehelper function's error code
    return return_value & 0xFF;
  }

  // Check if the user space helper program's return value stored in the second 8 bits of the
  //   return value contain any error codes
  if (((return_value >> 8) & 0xFF) != 0)
  {
    // Log the error
    // Note: the sign (+/-) of the user space helper program's error code is not returned by the
    //       call_usermodehelper function
    if (log_helper_error)
    {
      printk(KERN_ERR "qnap-ec helper program exited with a non zero exit code (+/-%i)",
        ((return_value >> 8) & 0xFF));
    }

    // Return the user space helper program's error code
    return (return_value >> 8) & 0xFF;
  }

  return 0;
}

// Function called when the miscellaneous device is openeded
static int qnap_ec_misc_device_open(struct inode* inode, struct file* file)
{
  // Declare and/or define needed variables
  struct qnap_ec_devices* devices = container_of(file->private_data, struct qnap_ec_devices,
    misc_device);

  // Check if the open device flag is not set which means we are not expecting any communications
  if (devices->open_misc_device == false)
  {
    return -EBUSY;
  }

  // Try to lock the miscellaneous device mutex if it's currently unlocked
  // Note: if the mutex is currently locked it means we are already communicating and this is an
  //       unexpected communication
  if (mutex_trylock(&devices->misc_device_mutex) == 0)
  {
    return -EBUSY;
  }

  return 0;
}

// Function called when the miscellaneous device receives a I/O control command
static long int qnap_ec_misc_device_ioctl(struct file* file, unsigned int command,
                                          unsigned long argument)
{
  // Declare and/or define needed variables
  // Note: the following statement is a combination of the following two statements:
  //       struct qnap_ec_devices* devices = container_of(file->private_data,
  //         struct qnap_ec_devices, misc_device);
  //       struct qnap_ec_data* data = dev_get_drvdata(&devices->plat_device->dev);
  struct qnap_ec_data* data = dev_get_drvdata(&container_of(file->private_data,
    struct qnap_ec_devices, misc_device)->plat_device->dev);

  // Swtich based on the command
  switch (command)
  {
    case QNAP_EC_IOCTL_CALL:
      // Make sure we can write the data to user space
      if (access_ok(argument, sizeof(struct qnap_ec_ioctl_command)) == 0)
      {
        return -EFAULT;
      }

      // Copy the I/O control command data from the data structure to the user space
      if (copy_to_user((void*)argument, &data->ioctl_command,
        sizeof(struct qnap_ec_ioctl_command)) != 0)
      {
        return -EFAULT;
      }

      break;
    case QNAP_EC_IOCTL_RETURN:
      // Make sure we can read the data from user space
      if (access_ok(argument, sizeof(struct qnap_ec_ioctl_command)) == 0)
      {
        return -EFAULT;
      }

      // Copy the I/O control command data from the user space to the data structure
      if (copy_from_user(&data->ioctl_command, (void*)argument,
        sizeof(struct qnap_ec_ioctl_command)) != 0)
      {
        return -EFAULT;
      }

      break;
    default:
      return -EINVAL;
  }

  return 0;
}

// Function called when the miscellaneous device is released
static int qnap_ec_misc_device_release(struct inode* inode, struct file* file)
{
  // Declare and/or define needed variables
  struct qnap_ec_devices* devices = container_of(file->private_data, struct qnap_ec_devices,
    misc_device);

  // Release the miscellaneous device mutex lock
  mutex_unlock(&devices->misc_device_mutex);

  return 0;
}

// Function called to exit the driver
static void __exit qnap_ec_exit(void)
{
  // Unregister the platform device and free the platform device structure memory
  // Note: we are using the platform_device_unregister function instead of the platform_device_put
  //       function used in the qnap_ec_init function because all other hwmon drivers take this
  //       approach
  platform_device_unregister(qnap_ec_devices->plat_device);

  // Unregister the miscellaneous device
  misc_deregister(&qnap_ec_devices->misc_device);

  // Free the devices structure memory
  kfree(qnap_ec_devices);

  // Unregister the platform driver
  platform_driver_unregister(qnap_ec_plat_driver);

  // Free the platform driver structure memory
  kfree(qnap_ec_plat_driver);
}