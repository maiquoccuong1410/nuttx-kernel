/****************************************************************************
 * drivers/sensors/max31855.c
 * Character driver for the Maxim MAX31855 Thermocouple-to-Digital Converter
 *
 *   Copyright (C) 2015 Alan Carvalho de Assis. All rights reserved.
 *   Author: Alan Carvalho de Assis <acassis@extern.io>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/* NOTE: Some Maxim MAX31855 chips have an issue it report value 25% lower
 * of real temperature, for more info read this thread:
 * http://www.eevblog.com/forum/projects/max31855-temperature-error/
*/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdlib.h>
#include <fixedmath.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/kmalloc.h>
#include <nuttx/fs/fs.h>
#include <nuttx/spi/spi.h>
#include <nuttx/sensors/max31855.h>

#if defined(CONFIG_SPI) && defined(CONFIG_MAX31855)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Private
 ****************************************************************************/

#define MAX31855_FAULT         (1 << 16)
#define MAX31855_SHORT_VCC     (1 << 2)
#define MAX31855_SHORT_GND     (1 << 1)
#define MAX31855_OPEN_CIRCUIT  (1 << 0)
#define MAX31855_TEMP_COUPLE   0xFFFFC000
#define MAX31855_TEMP_JUNCTION 0xFFF0

struct max31855_dev_s
{
  FAR struct spi_dev_s *spi;           /* Saved SPI driver instance */
  int16_t temp;
};


/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Character driver methods */

static int     max31855_open(FAR struct file *filep);
static int     max31855_close(FAR struct file *filep);
static ssize_t max31855_read(FAR struct file *, FAR char *, size_t);
static ssize_t max31855_write(FAR struct file *filep, FAR const char *buffer,
                              size_t buflen);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct file_operations g_max31855fops =
{
  max31855_open,
  max31855_close,
  max31855_read,
  max31855_write,
  NULL,
  NULL
#ifndef CONFIG_DISABLE_POLL
  , NULL
#endif
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/
/****************************************************************************
 * Name: max31855_open
 *
 * Description:
 *   This function is called whenever the MAX31855 device is opened.
 *
 ****************************************************************************/

static int max31855_open(FAR struct file *filep)
{
  return OK;
}

/****************************************************************************
 * Name: max31855_close
 *
 * Description:
 *   This routine is called when the MAX31855 device is closed.
 *
 ****************************************************************************/

static int max31855_close(FAR struct file *filep)
{
  return OK;
}

/****************************************************************************
 * Name: max31855_read
 ****************************************************************************/

static ssize_t max31855_read(FAR struct file *filep, FAR char *buffer, size_t buflen)
{
  FAR struct inode          *inode = filep->f_inode;
  FAR struct max31855_dev_s *priv  = inode->i_private;
  FAR uint16_t              *temp  = (FAR uint16_t *) buffer;
  int                       ret    = 2;
  int32_t                   regmsb;
  int32_t                   regval;

  /* Check for issues */

  if (!buffer)
    {
      sndbg("Buffer is null\n");
      return -EINVAL;
    }

  if (buflen != 2)
    {
      sndbg("You can't read something other than 16 bits (2 bytes)\n");
      return -EINVAL;
    }

  /* Enable MAX31855's chip select */

  SPI_SELECT(priv->spi, SPIDEV_TEMPERATURE, true);

  /* Read temperature */

  SPI_RECVBLOCK(priv->spi, &regmsb, 4);

  /* Disable MAX31855's chip select */

  SPI_SELECT(priv->spi, SPIDEV_TEMPERATURE, false);

  regval  = (regmsb & 0xFF000000) >> 24;
  regval |= (regmsb & 0xFF0000) >> 8;
  regval |= (regmsb & 0xFF00) << 8;
  regval |= (regmsb & 0xFF) << 24;

  sndbg("Read from MAX31855 = 0x%08X\n", regval);

  /* If negative, fix signal bits */

  if (regval & 0x80000000)
    {
      *temp = 0xc000 | (regval >> 18);
    }
  else
    {
      *temp = (regval >> 18);
    }

  /* Detect any fault */

  if (regval & MAX31855_FAULT)
    {
      sndbg("Error: A fault was detected by MAX31855:\n");

      if (regval & MAX31855_SHORT_VCC)
        {
          sndbg("The thermocouple input is shorted to VCC!\n");
        }

      if (regval & MAX31855_SHORT_GND)
        {
          sndbg("The thermocouple input is shorted to GND!\n");
        }

      if (regval & MAX31855_OPEN_CIRCUIT)
        {
          sndbg("The thermocouple input is not connected!\n");
        }

      ret = -EINVAL;
    }

  /* Return two bytes, the temperature is fixed point Q12.2, then divide by 4
   * in your application in other to get real temperature in Celsius degrees.
   */

  return ret;
}

/****************************************************************************
 * Name: max31855_write
 ****************************************************************************/

static ssize_t max31855_write(FAR struct file *filep, FAR const char *buffer,
                              size_t buflen)
{
  return -ENOSYS;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: max31855_register
 *
 * Description:
 *   Register the MAX31855 character device as 'devpath'
 *
 * Input Parameters:
 *   devpath - The full path to the driver to register. E.g., "/dev/temp0"
 *   i2c - An instance of the I2C interface to use to communicate wit
 *   MAX31855 addr - The I2C address of the MAX31855.  The base I2C address
 *   of the MAX31855 is 0x48.  Bits 0-3 can be controlled to get 8 unique
 *   addresses from 0x48 through 0x4f.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int max31855_register(FAR const char *devpath, FAR struct spi_dev_s *spi)
{
  FAR struct max31855_dev_s *priv;
  int ret;

  /* Sanity check */

  DEBUGASSERT(spi != NULL);

  /* Initialize the MAX31855 device structure */

  priv = (FAR struct max31855_dev_s *)kmm_malloc(sizeof(struct max31855_dev_s));
  if (priv == NULL)
    {
      sndbg("Failed to allocate instance\n");
      return -ENOMEM;
    }

  priv->spi        = spi;
  priv->temp       = 0;

#ifdef CONFIG_SPI_OWNBUS
  /* Configure SPI for the MAX31855 */

  SPI_SETMODE(priv->spi, SPIDEV_MODE1);
  SPI_SETBITS(priv->spi, 8);
  SPI_SETFREQUENCY(priv->spi, MAX31855_SPI_MAXFREQ);
#endif

  /* Register the character driver */

  ret = register_driver(devpath, &g_max31855fops, 0666, priv);
  if (ret < 0)
    {
      sndbg("Failed to register driver: %d\n", ret);
      kmm_free(priv);
    }

  return ret;
}
#endif /* CONFIG_SPI && CONFIG_MAX31855 */
