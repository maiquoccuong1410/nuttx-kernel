/****************************************************************************
 * arch/arm/src/stm32/stm32_adc.c
 *
 *   Copyright (C) 2011, 2013, 2015 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *           Diego Sanchez <dsanchez@nx-engineering.com>
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <semaphore.h>
#include <errno.h>
#include <assert.h>
#include <debug.h>
#include <unistd.h>

#include <arch/board/board.h>
#include <nuttx/arch.h>
#include <nuttx/analog/adc.h>

#include "up_internal.h"
#include "up_arch.h"

#include "chip.h"
#include "stm32.h"
#include "stm32_dma.h"
#include "stm32_adc.h"

/* ADC "upper half" support must be enabled */

#ifdef CONFIG_ADC

/* Some ADC peripheral must be enabled */

#if defined(CONFIG_STM32_ADC1) || defined(CONFIG_STM32_ADC2) || \
    defined(CONFIG_STM32_ADC3)

/* This implementation is for the STM32 F1, F2, F4 and STM32L15XX only */

#if defined(CONFIG_STM32_STM32F10XX) || defined(CONFIG_STM32_STM32F20XX) || \
    defined(CONFIG_STM32_STM32F40XX) || defined(CONFIG_STM32_STM32L15XX)

/* At the moment there is no proper implementation for timers external
 * trigger in STM32L15XX May be added latter
 */

#if defined(ADC_HAVE_TIMER) && defined(CONFIG_STM32_STM32L15XX)
#  warning "There is no proper implementation for TIMER TRIGGERS at the moment"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/
/* ADC interrupts ***********************************************************/

#ifdef CONFIG_STM32_STM32F10XX
#  define ADC_SR_ALLINTS  (ADC_SR_AWD | ADC_SR_EOC | ADC_SR_JEOC)
#else
#  define ADC_SR_ALLINTS  (ADC_SR_AWD | ADC_SR_EOC | ADC_SR_JEOC | \
                           ADC_SR_OVR)
#endif

#ifdef CONFIG_STM32_STM32F10XX
#  define ADC_CR1_ALLINTS (ADC_CR1_AWDIE | ADC_CR1_EOCIE | ADC_CR1_JEOCIE)
#else
#  define ADC_CR1_ALLINTS (ADC_CR1_AWDIE | ADC_CR1_EOCIE | ADC_CR1_JEOCIE | \
                           ADC_CR1_OVRIE)
#endif

/* ADC Channels/DMA ********************************************************/
/* The maximum number of channels that can be sampled.  If DMA support is
 * not enabled, then only a single channel can be sampled.  Otherwise,
 * data overruns would occur.
 */

#define ADC_MAX_CHANNELS_DMA   16
#define ADC_MAX_CHANNELS_NODMA 1

#ifdef ADC_HAVE_DMA
#  define ADC_MAX_SAMPLES ADC_MAX_CHANNELS_DMA
#else
#  define ADC_MAX_SAMPLES ADC_MAX_CHANNELS_NODMA
#endif

/* DMA channels and interface values differ for the F1 and F4 families */

#if defined(CONFIG_STM32_STM32L15XX)
#  define ADC_CHANNELS_NUMBER  32
#  define ADC_DEFAULT_SAMPLE   0x7
#endif

/* This can be refined or defined in Kconfig */

#ifndef CONFIG_ADC_TOTAL_CHANNELS
#  define ADC_MAX_CHANNELS ADC_MAX_SAMPLES
#else
#  define ADC_MAX_CHANNELS CONFIG_ADC_TOTAL_CHANNELS
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* This structure describes the state of one ADC block */

struct stm32_dev_s
{
  uint8_t irq;          /* Interrupt generated by this ADC block */
  uint8_t nchannels;    /* Number of channels */
  uint8_t cchannels;    /* Number of configured channels */
  uint8_t intf;         /* ADC interface number */
  uint8_t current;      /* Current ADC channel being converted */
#ifdef ADC_HAVE_DMA
  uint8_t dmachan;     /* DMA channel needed by this ADC */
  bool hasdma;         /* True: This channel supports DMA */
#endif
#if defined(CONFIG_STM32_STM32L15XX)
  /* Sample time selection. These bits must be written only when ADON=0 */

  uint8_t sample_rate[ADC_CHANNELS_NUMBER];
#endif
#ifdef ADC_HAVE_TIMER
  uint8_t trigger;      /* Timer trigger channel: 0=CC1, 1=CC2, 2=CC3,
                         * 3=CC4, 4=TRGO */
#endif
  xcpt_t isr;           /* Interrupt handler for this ADC block */
  uint32_t base;        /* Base address of registers unique to this ADC
                         * block */
#ifdef ADC_HAVE_TIMER
  uint32_t tbase;       /* Base address of timer used by this ADC block */
  uint32_t extsel;      /* EXTSEL value used by this ADC block */
  uint32_t pclck;       /* The PCLK frequency that drives this timer */
  uint32_t freq;        /* The desired frequency of conversions */
#endif
#ifdef ADC_HAVE_DMA
  DMA_HANDLE dma;       /* Allocated DMA channel */

  /* DMA transfer buffer */

  uint16_t dmabuffer[ADC_MAX_SAMPLES];
#endif

  /* List of selected ADC channels to sample */

  uint8_t  chanlist[ADC_MAX_SAMPLES];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* ADC Register access */

static uint32_t adc_getreg(struct stm32_dev_s *priv, int offset);
static void adc_putreg(struct stm32_dev_s *priv, int offset, uint32_t value);
#ifdef ADC_HAVE_TIMER
static uint16_t tim_getreg(struct stm32_dev_s *priv, int offset);
static void tim_putreg(struct stm32_dev_s *priv, int offset, uint16_t value);
static void adc_tim_dumpregs(struct stm32_dev_s *priv, FAR const char *msg);
#endif

static void adc_rccreset(struct stm32_dev_s *priv, bool reset);

/* ADC Interrupt Handler */

static int adc_interrupt(FAR struct adc_dev_s *dev);
#if defined(CONFIG_STM32_STM32F10XX) && (defined(CONFIG_STM32_ADC1) || \
    defined(CONFIG_STM32_ADC2))
static int adc12_interrupt(int irq, void *context);
#endif
#if defined(CONFIG_STM32_STM32F10XX) && defined (CONFIG_STM32_ADC3)
static int adc3_interrupt(int irq, void *context);
#endif
#if defined(CONFIG_STM32_STM32F20XX) || defined(CONFIG_STM32_STM32F40XX)
static int adc123_interrupt(int irq, void *context);
#endif
#ifdef CONFIG_STM32_STM32L15XX
static int adc_stm32l_interrupt(int irq, void *context);
#endif

/* ADC Driver Methods */

static void adc_reset(FAR struct adc_dev_s *dev);
static int  adc_setup(FAR struct adc_dev_s *dev);
static void adc_shutdown(FAR struct adc_dev_s *dev);
static void adc_rxint(FAR struct adc_dev_s *dev, bool enable);
static int  adc_ioctl(FAR struct adc_dev_s *dev, int cmd, unsigned long arg);
static void adc_enable(FAR struct stm32_dev_s *priv, bool enable);
static int adc_set_ch(FAR struct adc_dev_s *dev, uint8_t ch);
static int adc_set_ch_idx(FAR struct adc_dev_s *dev, uint8_t idx);
#ifdef CONFIG_STM32_STM32L15XX
  static void adc_power_down_idle(FAR struct stm32_dev_s *priv,
                                  bool pdi_high);
  static void adc_power_down_delay(FAR struct stm32_dev_s *priv,
                                   bool pdd_high);
  static void adc_dels_after_conversion(FAR struct stm32_dev_s *priv,
                                        uint32_t delay);
  static void adc_select_ch_bank(FAR struct stm32_dev_s *priv,
                                 bool chb_selected);
  static int adc_ioc_change_ints(FAR struct adc_dev_s *dev, int cmd,
                                 bool arg);
#endif
#if defined(CONFIG_STM32_STM32L15XX) && ((STM32_CFGR_PLLSRC != 0) || \
    (STM32_SYSCLK_SW != RCC_CFGR_SW_HSI))
  static void adc_enable_hsi(bool enable);
  static void adc_reset_hsi_disable(FAR struct adc_dev_s *dev);
#endif

#ifdef ADC_HAVE_TIMER
static void adc_timstart(FAR struct stm32_dev_s *priv, bool enable);
static int  adc_timinit(FAR struct stm32_dev_s *priv);
#endif

#if defined(CONFIG_STM32_STM32F20XX) || defined(CONFIG_STM32_STM32F40XX) || \
    defined(CONFIG_STM32_STM32L15XX)
static void adc_startconv(FAR struct stm32_dev_s *priv, bool enable);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* ADC interface operations */

static const struct adc_ops_s g_adcops =
{
#if defined(CONFIG_STM32_STM32L15XX) && ((STM32_CFGR_PLLSRC != 0) || \
    (STM32_SYSCLK_SW != RCC_CFGR_SW_HSI))
  .ao_reset       = adc_reset_hsi_disable,
#else
  .ao_reset       = adc_reset,
#endif
  .ao_setup       = adc_setup,
  .ao_shutdown    = adc_shutdown,
  .ao_rxint       = adc_rxint,
  .ao_ioctl       = adc_ioctl,
};

/* ADC1 state */

#ifdef CONFIG_STM32_ADC1
static struct stm32_dev_s g_adcpriv1 =
{
#ifdef CONFIG_STM32_STM32F10XX
  .irq         = STM32_IRQ_ADC12,
  .isr         = adc12_interrupt,
#elif defined(CONFIG_STM32_STM32L15XX)
  .irq         = STM32_IRQ_ADC1,
  .isr         = adc_stm32l_interrupt,
#else
  .irq         = STM32_IRQ_ADC,
  .isr         = adc123_interrupt,
#endif
  .intf        = 1,
#ifndef CONFIG_STM32_STM32L15XX
  .base        = STM32_ADC1_BASE,
#else
  .base        = STM32_ADC_BASE,
#endif

#ifdef ADC1_HAVE_TIMER
  .trigger     = CONFIG_STM32_ADC1_TIMTRIG,
  .tbase       = ADC1_TIMER_BASE,
  .extsel      = ADC1_EXTSEL_VALUE,
  .pclck       = ADC1_TIMER_PCLK_FREQUENCY,
  .freq        = CONFIG_STM32_ADC1_SAMPLE_FREQUENCY,
#endif
#ifdef ADC1_HAVE_DMA
  .dmachan     = ADC1_DMA_CHAN,
  .hasdma      = true,
#endif
};

static struct adc_dev_s g_adcdev1 =
{
  .ad_ops = &g_adcops,
  .ad_priv= &g_adcpriv1,
};
#endif

/* ADC2 state */

#ifdef CONFIG_STM32_ADC2
static struct stm32_dev_s g_adcpriv2 =
{
#ifdef CONFIG_STM32_STM32F10XX
  .irq         = STM32_IRQ_ADC12,
  .isr         = adc12_interrupt,
#else
  .irq         = STM32_IRQ_ADC,
  .isr         = adc123_interrupt,
#endif
  .intf        = 2,
  .base        = STM32_ADC2_BASE,
#ifdef ADC2_HAVE_TIMER
  .trigger     = CONFIG_STM32_ADC2_TIMTRIG,
  .tbase       = ADC2_TIMER_BASE,
  .extsel      = ADC2_EXTSEL_VALUE,
  .pclck       = ADC2_TIMER_PCLK_FREQUENCY,
  .freq        = CONFIG_STM32_ADC2_SAMPLE_FREQUENCY,
#endif
#ifdef ADC2_HAVE_DMA
  .dmachan     = ADC2_DMA_CHAN,
  .hasdma      = true,
#endif
};

static struct adc_dev_s g_adcdev2 =
{
  .ad_ops = &g_adcops,
  .ad_priv= &g_adcpriv2,
};
#endif

/* ADC3 state */

#ifdef CONFIG_STM32_ADC3
static struct stm32_dev_s g_adcpriv3 =
{
#ifdef CONFIG_STM32_STM32F10XX
  .irq         = STM32_IRQ_ADC3,
  .isr         = adc3_interrupt,
#else
  .irq         = STM32_IRQ_ADC,
  .isr         = adc123_interrupt,
#endif
  .intf        = 3,
  .base        = STM32_ADC3_BASE,
#ifdef ADC3_HAVE_TIMER
  .trigger     = CONFIG_STM32_ADC3_TIMTRIG,
  .tbase       = ADC3_TIMER_BASE,
  .extsel      = ADC3_EXTSEL_VALUE,
  .pclck       = ADC3_TIMER_PCLK_FREQUENCY,
  .freq        = CONFIG_STM32_ADC3_SAMPLE_FREQUENCY,
#endif
#ifdef ADC3_HAVE_DMA
  .dmachan     = ADC3_DMA_CHAN,
  .hasdma      = true,
#endif
};

static struct adc_dev_s g_adcdev3 =
{
  .ad_ops = &g_adcops,
  .ad_priv= &g_adcpriv3,
};
#endif

/* ADC4 state */

#ifdef CONFIG_STM32_ADC4
#  error Missing ADC4 implementation
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: adc_getreg
 *
 * Description:
 *   Read the value of an ADC register.
 *
 * Input Parameters:
 *   priv - A reference to the ADC block status
 *   offset - The offset to the register to read
 *
 * Returned Value:
 *
 ****************************************************************************/

static uint32_t adc_getreg(struct stm32_dev_s *priv, int offset)
{
  return getreg32(priv->base + offset);
}

/****************************************************************************
 * Name: adc_getreg
 *
 * Description:
 *   Read the value of an ADC register.
 *
 * Input Parameters:
 *   priv - A reference to the ADC block status
 *   offset - The offset to the register to read
 *
 * Returned Value:
 *
 ****************************************************************************/

static void adc_putreg(struct stm32_dev_s *priv, int offset, uint32_t value)
{
  putreg32(value, priv->base + offset);
}

/****************************************************************************
 * Name: tim_getreg
 *
 * Description:
 *   Read the value of an ADC timer register.
 *
 * Input Parameters:
 *   priv - A reference to the ADC block status
 *   offset - The offset to the register to read
 *
 * Returned Value:
 *   The current contents of the specified register
 *
 ****************************************************************************/

#ifdef ADC_HAVE_TIMER
static uint16_t tim_getreg(struct stm32_dev_s *priv, int offset)
{
  return getreg16(priv->tbase + offset);
}
#endif

/****************************************************************************
 * Name: tim_putreg
 *
 * Description:
 *   Read the value of an ADC timer register.
 *
 * Input Parameters:
 *   priv - A reference to the ADC block status
 *   offset - The offset to the register to read
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifdef ADC_HAVE_TIMER
static void tim_putreg(struct stm32_dev_s *priv, int offset, uint16_t value)
{
  putreg16(value, priv->tbase + offset);
}
#endif

/****************************************************************************
 * Name: adc_tim_dumpregs
 *
 * Description:
 *   Dump all timer registers.
 *
 * Input parameters:
 *   priv - A reference to the ADC block status
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifdef ADC_HAVE_TIMER
static void adc_tim_dumpregs(struct stm32_dev_s *priv, FAR const char *msg)
{
#if defined(CONFIG_DEBUG_ANALOG) && defined(CONFIG_DEBUG_VERBOSE)
  avdbg("%s:\n", msg);
  avdbg("  CR1: %04x CR2:  %04x SMCR:  %04x DIER:  %04x\n",
        tim_getreg(priv, STM32_GTIM_CR1_OFFSET),
        tim_getreg(priv, STM32_GTIM_CR2_OFFSET),
        tim_getreg(priv, STM32_GTIM_SMCR_OFFSET),
        tim_getreg(priv, STM32_GTIM_DIER_OFFSET));
  avdbg("   SR: %04x EGR:  0000 CCMR1: %04x CCMR2: %04x\n",
        tim_getreg(priv, STM32_GTIM_SR_OFFSET),
        tim_getreg(priv, STM32_GTIM_CCMR1_OFFSET),
        tim_getreg(priv, STM32_GTIM_CCMR2_OFFSET));
  avdbg(" CCER: %04x CNT:  %04x PSC:   %04x ARR:   %04x\n",
        tim_getreg(priv, STM32_GTIM_CCER_OFFSET),
        tim_getreg(priv, STM32_GTIM_CNT_OFFSET),
        tim_getreg(priv, STM32_GTIM_PSC_OFFSET),
        tim_getreg(priv, STM32_GTIM_ARR_OFFSET));
  avdbg(" CCR1: %04x CCR2: %04x CCR3:  %04x CCR4:  %04x\n",
        tim_getreg(priv, STM32_GTIM_CCR1_OFFSET),
        tim_getreg(priv, STM32_GTIM_CCR2_OFFSET),
        tim_getreg(priv, STM32_GTIM_CCR3_OFFSET),
        tim_getreg(priv, STM32_GTIM_CCR4_OFFSET));
#  ifndef CONFIG_STM32_STM32L15XX
     if (priv->tbase == STM32_TIM1_BASE || priv->tbase == STM32_TIM8_BASE)
      {
        avdbg("  RCR: %04x BDTR: %04x DCR:   %04x DMAR:  %04x\n",
              tim_getreg(priv, STM32_ATIM_RCR_OFFSET),
              tim_getreg(priv, STM32_ATIM_BDTR_OFFSET),
              tim_getreg(priv, STM32_ATIM_DCR_OFFSET),
              tim_getreg(priv, STM32_ATIM_DMAR_OFFSET));
      }
    else
      {
        avdbg("  DCR: %04x DMAR: %04x\n",
              tim_getreg(priv, STM32_GTIM_DCR_OFFSET),
              tim_getreg(priv, STM32_GTIM_DMAR_OFFSET));
      }
#  endif
#endif
}
#endif

/****************************************************************************
 * Name: adc_timstart
 *
 * Description:
 *   Start (or stop) the timer counter
 *
 * Input Parameters:
 *   priv - A reference to the ADC block status
 *   enable - True: Start conversion
 *
 * Returned Value:
 *
 ****************************************************************************/

#ifdef ADC_HAVE_TIMER
static void adc_timstart(struct stm32_dev_s *priv, bool enable)
{
  uint16_t regval;

  avdbg("enable: %d\n", enable);
  regval  = tim_getreg(priv, STM32_GTIM_CR1_OFFSET);

  if (enable)
    {
      /* Start the counter */

      regval |= ATIM_CR1_CEN;
    }

  else
    {
      /* Disable the counter */

      regval &= ~ATIM_CR1_CEN;
    }

  tim_putreg(priv, STM32_GTIM_CR1_OFFSET, regval);
}
#endif

/****************************************************************************
 * Name: adc_timinit
 *
 * Description:
 *   Initialize the timer that drivers the ADC sampling for this channel using
 *   the pre-calculated timer divider definitions.
 *
 * Input Parameters:
 *   priv - A reference to the ADC block status
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

#ifdef ADC_HAVE_TIMER
static int adc_timinit(FAR struct stm32_dev_s *priv)
{
  uint32_t prescaler;
  uint32_t reload;
  uint32_t regval;
  uint32_t timclk;

  uint16_t cr1;
  uint16_t cr2;
  uint16_t ccmr1;
  uint16_t ccmr2;
  uint16_t ocmode1;
  uint16_t ocmode2;
  uint16_t ccenable;
  uint16_t ccer;
  uint16_t egr;

  avdbg("Initializing timers extsel = 0x%08X\n", priv->extsel);

  /* If the timer base address is zero, then this ADC was not configured to
   * use a timer.
   */

  regval  = adc_getreg(priv, STM32_ADC_CR2_OFFSET);

#ifdef CONFIG_STM32_STM32F10XX
  if (!priv->tbase)
    {
      /* Configure the ADC to use the selected timer and timer channel as
       * the trigger EXTTRIG: External Trigger Conversion mode for regular
       * channels DISABLE
       */

      regval &= ~ADC_CR2_EXTTRIG;
      adc_putreg(priv, STM32_ADC_CR2_OFFSET, regval);
      return OK;
    }
  else
    {
      regval |= ADC_CR2_EXTTRIG;
    }
#endif

  /* EXTSEL selection: These bits select the external event used to trigger
   * the start of conversion of a regular group.  NOTE:
   *
   * - The position with of the EXTSEL field varies from one STM32 MCU
   *   to another.
   * - The width of the EXTSEL field varies from one STM3 MCU to another.
   * - The value in priv->extsel is already shifted into the correct bit
   *   position.
   */

  regval &= ~ADC_CR2_EXTSEL_MASK;
  regval |= priv->extsel;
  adc_putreg(priv, STM32_ADC_CR2_OFFSET, regval);

  /* Configure the timer channel to drive the ADC */

  /* Caculate optimal values for the timer prescaler and for the timer
   * reload register.  If freq is the desired frequency, then
   *
   *   reload = timclk / freq
   *   reload = (pclck / prescaler) / freq
   *
   * There are many solutions to do this, but the best solution will be the
   * one that has the largest reload value and the smallest prescaler value.
   * That is the solution that should give us the most accuracy in the timer
   * control.  Subject to:
   *
   *   0 <= prescaler  <= 65536
   *   1 <= reload <= 65535
   *
   * So ( prescaler = pclck / 65535 / freq ) would be optimal.
   */

  prescaler = (priv->pclck / priv->freq + 65534) / 65535;

  /* We need to decrement the prescaler value by one, but only, the value
   * does not underflow.
   */

  if (prescaler < 1)
    {
      adbg("WARNING: Prescaler underflowed.\n");
      prescaler = 1;
    }

  /* Check for overflow */

  else if (prescaler > 65536)
    {
      adbg("WARNING: Prescaler overflowed.\n");
      prescaler = 65536;
    }

  timclk = priv->pclck / prescaler;

  reload = timclk / priv->freq;
  if (reload < 1)
    {
      adbg("WARNING: Reload value underflowed.\n");
      reload = 1;
    }
  else if (reload > 65535)
    {
      adbg("WARNING: Reload value overflowed.\n");
      reload = 65535;
    }

  /* Set up the timer CR1 register */

  cr1 = tim_getreg(priv, STM32_GTIM_CR1_OFFSET);

  /* Disable the timer until we get it configured */

  adc_timstart(priv, false);

  /* Select the Counter Mode == count up:
   *
   * ATIM_CR1_EDGE: The counter counts up or down depending on the
   *                direction bit(DIR).
   * ATIM_CR1_DIR: 0: count up, 1: count down
   */

  cr1 &= ~(ATIM_CR1_DIR | ATIM_CR1_CMS_MASK);
  cr1 |= ATIM_CR1_EDGE;

  /* Set the clock division to zero for all */

  cr1 &= ~GTIM_CR1_CKD_MASK;
  tim_putreg(priv, STM32_GTIM_CR1_OFFSET, cr1);

  /* Set the reload and prescaler values */

  tim_putreg(priv, STM32_GTIM_PSC_OFFSET, prescaler-1);
  tim_putreg(priv, STM32_GTIM_ARR_OFFSET, reload);

  /* Clear the advanced timers repetition counter in TIM1 */

#ifndef CONFIG_STM32_STM32L15XX
  if (priv->tbase == STM32_TIM1_BASE || priv->tbase == STM32_TIM8_BASE)
    {
      tim_putreg(priv, STM32_ATIM_RCR_OFFSET, 0);
      tim_putreg(priv, STM32_ATIM_BDTR_OFFSET, ATIM_BDTR_MOE); /* Check me */
    }
#endif

  /* TIMx event generation: Bit 0 UG: Update generation */

  tim_putreg(priv, STM32_GTIM_EGR_OFFSET, ATIM_EGR_UG);

  /* Handle channel specific setup */

  ocmode1 = 0;
  ocmode2 = 0;

   switch (priv->trigger)
    {
      case 0: /* TimerX CC1 event */
        {
          ccenable = ATIM_CCER_CC1E;
          ocmode1  = (ATIM_CCMR_CCS_CCOUT << ATIM_CCMR1_CC1S_SHIFT) |
                     (ATIM_CCMR_MODE_PWM1 << ATIM_CCMR1_OC1M_SHIFT) |
                     ATIM_CCMR1_OC1PE;

          /* Set the event CC1 */

          egr      = ATIM_EGR_CC1G;

          /* Set the duty cycle by writing to the CCR register for this channel */

          tim_putreg(priv, STM32_GTIM_CCR1_OFFSET, (uint16_t)(reload >> 1));
        }
        break;

      case 1: /* TimerX CC2 event */
        {
          ccenable = ATIM_CCER_CC2E;
          ocmode1  = (ATIM_CCMR_CCS_CCOUT << ATIM_CCMR1_CC2S_SHIFT) |
                     (ATIM_CCMR_MODE_PWM1 << ATIM_CCMR1_OC2M_SHIFT) |
                     ATIM_CCMR1_OC2PE;

          /* Set the event CC2 */

          egr      = ATIM_EGR_CC2G;

          /* Set the duty cycle by writing to the CCR register for this channel */

          tim_putreg(priv, STM32_GTIM_CCR2_OFFSET, (uint16_t)(reload >> 1));
        }
        break;

      case 2: /* TimerX CC3 event */
        {
          ccenable = ATIM_CCER_CC3E;
          ocmode2  = (ATIM_CCMR_CCS_CCOUT << ATIM_CCMR2_CC3S_SHIFT) |
                     (ATIM_CCMR_MODE_PWM1 << ATIM_CCMR2_OC3M_SHIFT) |
                     ATIM_CCMR2_OC3PE;

          /* Set the event CC3 */

          egr      = ATIM_EGR_CC3G;

          /* Set the duty cycle by writing to the CCR register for this channel */

          tim_putreg(priv, STM32_GTIM_CCR3_OFFSET, (uint16_t)(reload >> 1));
        }
        break;

      case 3: /* TimerX CC4 event */
        {
          ccenable = ATIM_CCER_CC4E;
          ocmode2  = (ATIM_CCMR_CCS_CCOUT << ATIM_CCMR2_CC4S_SHIFT) |
                     (ATIM_CCMR_MODE_PWM1 << ATIM_CCMR2_OC4M_SHIFT) |
                     ATIM_CCMR2_OC4PE;

          /* Set the event CC4 */

          egr      = ATIM_EGR_CC4G;

          /* Set the duty cycle by writing to the CCR register for this channel */

          tim_putreg(priv, STM32_GTIM_CCR4_OFFSET, (uint16_t)(reload >> 1));
        }
        break;

      case 4: /* TimerX TRGO event */
        {
          /* TODO: TRGO support not yet implemented */
          /* Set the event TRGO */

          ccenable = 0;
          egr      = GTIM_EGR_TG;

          /* Set the duty cycle by writing to the CCR register for this channel */

          tim_putreg(priv, STM32_GTIM_CCR4_OFFSET, (uint16_t)(reload >> 1));
        }
        break;

      default:
        adbg("No such trigger: %d\n", priv->trigger);
        return -EINVAL;
    }

  /* Disable the Channel by resetting the CCxE Bit in the CCER register */

  ccer = tim_getreg(priv, STM32_GTIM_CCER_OFFSET);
  ccer &= ~ccenable;
  tim_putreg(priv, STM32_GTIM_CCER_OFFSET, ccer);

  /* Fetch the CR2, CCMR1, and CCMR2 register (already have cr1 and ccer) */

  cr2   = tim_getreg(priv, STM32_GTIM_CR2_OFFSET);
  ccmr1 = tim_getreg(priv, STM32_GTIM_CCMR1_OFFSET);
  ccmr2 = tim_getreg(priv, STM32_GTIM_CCMR2_OFFSET);

  /* Reset the Output Compare Mode Bits and set the select output compare mode */

  ccmr1 &= ~(ATIM_CCMR1_CC1S_MASK | ATIM_CCMR1_OC1M_MASK | ATIM_CCMR1_OC1PE |
             ATIM_CCMR1_CC2S_MASK | ATIM_CCMR1_OC2M_MASK | ATIM_CCMR1_OC2PE);
  ccmr2 &= ~(ATIM_CCMR2_CC3S_MASK | ATIM_CCMR2_OC3M_MASK | ATIM_CCMR2_OC3PE |
             ATIM_CCMR2_CC4S_MASK | ATIM_CCMR2_OC4M_MASK | ATIM_CCMR2_OC4PE);
  ccmr1 |= ocmode1;
  ccmr2 |= ocmode2;

  /* Reset the output polarity level of all channels (selects high polarity)*/

  ccer &= ~(ATIM_CCER_CC1P | ATIM_CCER_CC2P | ATIM_CCER_CC3P | ATIM_CCER_CC4P);

  /* Enable the output state of the selected channel (only) */

  ccer &= ~(ATIM_CCER_CC1E | ATIM_CCER_CC2E | ATIM_CCER_CC3E | ATIM_CCER_CC4E);
  ccer |= ccenable;

#ifndef CONFIG_STM32_STM32L15XX

    if (priv->tbase == STM32_TIM1_BASE || priv->tbase == STM32_TIM8_BASE)
      {
        /* Reset output N polarity level, output N state, output compare state,
         * output compare N idle state.
         */

#  if defined(CONFIG_STM32_STM32F20XX) || defined(CONFIG_STM32_STM32F40XX)
        ccer &= ~(ATIM_CCER_CC1NE | ATIM_CCER_CC1NP | ATIM_CCER_CC2NE | ATIM_CCER_CC2NP |
                  ATIM_CCER_CC3NE | ATIM_CCER_CC3NP | ATIM_CCER_CC4NP);
#  else
        ccer &= ~(ATIM_CCER_CC1NE | ATIM_CCER_CC1NP | ATIM_CCER_CC2NE | ATIM_CCER_CC2NP |
                  ATIM_CCER_CC3NE | ATIM_CCER_CC3NP);
#  endif

        /* Reset the output compare and output compare N IDLE State */

        cr2 &= ~(ATIM_CR2_OIS1 | ATIM_CR2_OIS1N | ATIM_CR2_OIS2 | ATIM_CR2_OIS2N |
                 ATIM_CR2_OIS3 | ATIM_CR2_OIS3N | ATIM_CR2_OIS4);
      }
#  if defined(CONFIG_STM32_STM32F20XX) || defined(CONFIG_STM32_STM32F40XX)
    else
      {
        ccer &= ~(GTIM_CCER_CC1NP | GTIM_CCER_CC2NP | GTIM_CCER_CC3NP);
      }
  #endif

#else

  /* For the STM32L15XX family only these timers can be used: 2-4, 6, 7, 9, 10
   * Reset the output compare and output compare N IDLE State
   */

  if (priv->tbase >= STM32_TIM2_BASE && priv->tbase <= STM32_TIM4_BASE)
    {
      /* Reset output N polarity level, output N state, output compare state,
       * output compare N idle state.
       */
      ccer &= ~(ATIM_CCER_CC1NE | ATIM_CCER_CC1NP | ATIM_CCER_CC2NE | ATIM_CCER_CC2NP |
                ATIM_CCER_CC3NE | ATIM_CCER_CC3NP | ATIM_CCER_CC4NP);
    }
#endif

  /* Save the modified register values */

  tim_putreg(priv, STM32_GTIM_CR2_OFFSET, cr2);
  tim_putreg(priv, STM32_GTIM_CCMR1_OFFSET, ccmr1);
  tim_putreg(priv, STM32_GTIM_CCMR2_OFFSET, ccmr2);
  tim_putreg(priv, STM32_GTIM_CCER_OFFSET, ccer);
  tim_putreg(priv, STM32_GTIM_EGR_OFFSET, egr);

  /* Set the ARR Preload Bit */

  cr1 = tim_getreg(priv, STM32_GTIM_CR1_OFFSET);
  cr1 |= GTIM_CR1_ARPE;
  tim_putreg(priv, STM32_GTIM_CR1_OFFSET, cr1);

  /* Enable the timer counter
   * All but the CEN bit with the default config in CR1
   */

  adc_timstart(priv, true);

  adc_tim_dumpregs(priv, "After starting Timers");

  return OK;
}
#endif

/****************************************************************************
 * Name: adc_startconv
 *
 * Description:
 *   Start (or stop) the ADC conversion process in DMA mode
 *
 * Input Parameters:
 *   priv - A reference to the ADC block status
 *   enable - True: Start conversion
 *
 * Returned Value:
 *
 ****************************************************************************/

#if defined(CONFIG_STM32_STM32F20XX) || defined(CONFIG_STM32_STM32F40XX) || \
    defined(CONFIG_STM32_STM32L15XX)
static void adc_startconv(struct stm32_dev_s *priv, bool enable)
{
  uint32_t regval;

  avdbg("enable: %d\n", enable);

  regval = adc_getreg(priv, STM32_ADC_CR2_OFFSET);

  if (enable)
    {
#ifdef CONFIG_ADC_CONTINUOUS

      /* Set continuous mode */

      regval |= ADC_CR2_CONT;

#endif

      /* Start conversion of regular channels */

      regval |= ADC_CR2_SWSTART;
    }
  else
    {
#ifdef CONFIG_ADC_CONTINUOUS

      /* Disable the continuous conversion */

      regval &= ~ADC_CR2_CONT;

#endif

      /* Disable the conversion of regular channels */

      regval &= ~ADC_CR2_SWSTART;
    }

  adc_putreg(priv, STM32_ADC_CR2_OFFSET, regval);
}
#endif

/****************************************************************************
 * Name: adc_rccreset
 *
 * Description:
 *   Deinitializes the ADCx peripheral registers to their default
 *   reset values. It could set all the ADCs configured.
 *
 * Input Parameters:
 *   regaddr - The register to read
 *   reset - Condition, set or reset
 *
 * Returned Value:
 *
 ****************************************************************************/

static void adc_rccreset(struct stm32_dev_s *priv, bool reset)
{
  irqstate_t flags;
  uint32_t regval;
  uint32_t adcbit;

  /* Pick the appropriate bit in the APB2 reset register */

#ifdef CONFIG_STM32_STM32F10XX
  /* For the STM32 F1, there is an individual bit to reset each ADC. */

  switch (priv->intf)
    {
#ifdef CONFIG_STM32_ADC1
      case 1:
        adcbit = RCC_APB2RSTR_ADC1RST;
        break;
#endif
#ifdef CONFIG_STM32_ADC2
      case 2:
        adcbit = RCC_APB2RSTR_ADC2RST;
        break;
#endif
#ifdef CONFIG_STM32_ADC3
      case 3:
        adcbit = RCC_APB2RSTR_ADC3RST;
        break;
#endif
      default:
        return;
    }

#elif defined(CONFIG_STM32_STM32L15XX)
  adcbit = RCC_APB2RSTR_ADC1RST;

#else
  /* For the STM32 F4, there is one common reset for all ADC block.
   * THIS will probably cause some problems!
   */

  adcbit = RCC_APB2RSTR_ADCRST;
#endif

  /* Disable interrupts.  This is necessary because the APB2RTSR register
   * is used by several different drivers.
   */

  flags = irqsave();

  /* Set or clear the selected bit in the APB2 reset register */

  regval = getreg32(STM32_RCC_APB2RSTR);
  if (reset)
    {
      /* Enable  ADC reset state */

      regval |= adcbit;
    }
  else
    {
      /* Release ADC from reset state */

      regval &= ~adcbit;
    }

  putreg32(regval, STM32_RCC_APB2RSTR);
  irqrestore(flags);
}

/*******************************************************************************
 * Name: adc_power_down_idle
 *
 * Description    : Enables or disables power down during the idle phase.
 *
 * Input Parameters:
 *
 *   priv     - pointer to the adc device structure
 *   pdi_high - true:  The ADC is powered down when waiting for a start event
 *              false: The ADC is powered up when waiting for a start event
 *
 * Returned Value:
 *   None.
 *
 *******************************************************************************/

#ifdef CONFIG_STM32_STM32L15XX
static void adc_power_down_idle(FAR struct stm32_dev_s *priv, bool pdi_high)
{
  uint32_t regval = 0;

  avdbg("PDI: %d\n", (pdi_high ? 1 : 0));

  regval = adc_getreg(priv, STM32_ADC_CR1_OFFSET);

  if (!(STM32_ADC1_CR2 & ADC_CR2_ADON))
    {
      if (pdi_high)
        {
          regval |= ADC_CR1_PDI;
        }
      else
        {
          regval &= ~ADC_CR1_PDI;
        }

      adc_putreg(priv, STM32_ADC_CR1_OFFSET, regval);
    }
}
#endif

/*******************************************************************************
 * Name: adc_power_down_delay
 *
 * Description    : Enables or disables power down during the delay phase.
 *
 * Input Parameters:
 *
 *   priv     - pointer to the adc device structure
 *   pdd_high - true:  The ADC is powered down when waiting for a start event
 *              false: The ADC is powered up when waiting for a start event
 *
 * Returned Value:
 *   None.
 *
 *******************************************************************************/

#ifdef CONFIG_STM32_STM32L15XX
static void adc_power_down_delay(FAR struct stm32_dev_s *priv, bool pdd_high)
{
  uint32_t regval = 0;

  avdbg("PDD: %d\n", (pdd_high ? 1 : 0));

  regval = adc_getreg(priv, STM32_ADC_CR1_OFFSET);

  if (!(STM32_ADC1_CR2 & ADC_CR2_ADON))
    {
      if (pdd_high)
        {
          regval |= ADC_CR1_PDD;
        }
      else
        {
          regval &= ~ADC_CR1_PDD;
        }

      adc_putreg(priv, STM32_ADC_CR1_OFFSET, regval);
    }
}
#endif

/*******************************************************************************
 * Name: adc_dels_after_conversion
 *
 * Description    : Defines the length of the delay which is applied
 *                  after a conversion or a sequence of conversions.
 *
 * Input Parameters:
 *
 *   priv  - pointer to the adc device structure
 *   delay - delay selection (see definition in chip/chip/stm32_adc.h
 *           starting from line 284)
 *
 * Returned Value:
 *
 *******************************************************************************/

#ifdef CONFIG_STM32_STM32L15XX
static void adc_dels_after_conversion(FAR struct stm32_dev_s *priv,
                                      uint32_t delay)
{
  uint32_t regval;

  avdbg("Delay selected: 0x%08X\n", delay);

  regval = adc_getreg(priv, STM32_ADC_CR2_OFFSET);
  regval &= ~ADC_CR2_DELS_MASK;
  regval |= delay;
  adc_putreg(priv, STM32_ADC_CR2_OFFSET, regval);
}
#endif

/*******************************************************************************
 * Name: adc_select_ch_bank
 *
 * Description    : Selects the bank of channels to be converted
 *                  (! Must be modified only when no conversion is on going !)
 *
 * Input Parameters:
 *
 *   priv   - pointer to the adc device structure
 *   enable - true:  bank of channels B selected
 *            false: bank of channels A selected
 *
 * Returned Value:
 *
 *******************************************************************************/

#ifdef CONFIG_STM32_STM32L15XX
static void adc_select_ch_bank(FAR struct stm32_dev_s *priv, bool chb_selected)
{
  uint32_t regval;

  avdbg("Bank of channels selected: %c\n", (chb_selected ? 'B' : 'A'));

  regval = adc_getreg(priv, STM32_ADC_CR2_OFFSET);

  if (chb_selected)
    {
      regval |= ADC_CR2_CFG;
    }
  else
    {
      regval &= ~ADC_CR2_CFG;
    }

  adc_putreg(priv, STM32_ADC_CR2_OFFSET, regval);
}
#endif

/*******************************************************************************
 * Name: adc_enable
 *
 * Description    : Enables or disables the specified ADC peripheral.
 *                  Also, starts a conversion when the ADC is not
 *                  triggered by timers
 *
 * Input Parameters:
 *
 *   enable - true:  enable ADC conversion
 *            false: disable ADC conversion
 *
 * Returned Value:
 *
 *******************************************************************************/

static void adc_enable(FAR struct stm32_dev_s *priv, bool enable)
{
  uint32_t regval;

  avdbg("enable: %d\n", (enable ? 1 : 0));

  /* Not all STM32 parts prove the SR ADONS bit */

#ifdef ADC_SR_ADONS
  regval = adc_getreg(priv, STM32_ADC_SR_OFFSET);

  if (!(regval & ADC_SR_ADONS) && enable)
    {
      regval  = adc_getreg(priv, STM32_ADC_CR2_OFFSET);
      regval |= ADC_CR2_ADON;
    }
  else if ((regval & ADC_SR_ADONS) && !enable)
    {
      regval  = adc_getreg(priv, STM32_ADC_CR2_OFFSET);
      regval &= ~ADC_CR2_ADON;
    }

#else
  regval  = adc_getreg(priv, STM32_ADC_CR2_OFFSET);
  if (enable)
    {
      regval |= ADC_CR2_ADON;
    }
  else
    {
      regval &= ~ADC_CR2_ADON;
    }
#endif

  adc_putreg(priv, STM32_ADC_CR2_OFFSET, regval);
}

/****************************************************************************
 * Name: adc_write_sample_time_registers
 *
 * Description:
 *   Writes previously defined values into ADC_SMPR0, ADC_SMPR1, ADC_SMPR2
 *   and ADC_SMPR3 registers
 *
 * Input Parameters:
 *
 * Returned Value:
 *
 ****************************************************************************/

#ifdef CONFIG_STM32_STM32L15XX
static void adc_write_sample_time_registers(FAR struct adc_dev_s *dev)
{
  FAR struct stm32_dev_s *priv = (FAR struct stm32_dev_s *)dev->ad_priv;
  uint32_t value = 0;
  uint8_t i, shift;

  /* Sampling time individually for each channel
  * 000: 4 cycles
  * 001: 9 cycles
  * 010: 16 cycles
  * 011: 24 cycles
  * 100: 48 cycles
  * 101: 96 cycles
  * 110: 192 cycles
  * 111: 384 cycles    - selected for all channels
  */

  for (i = 0, shift = 0; i < 32; i++)
    {
      value |= priv->sample_rate[i] << (shift * 3);
      switch (i)
        {
          case 9:
            adc_putreg(priv, STM32_ADC_SMPR3_OFFSET, value);
            shift = 0;
            value = 0;
            break;

          case 19:
            adc_putreg(priv, STM32_ADC_SMPR2_OFFSET, value);
            shift = 0;
            value = 0;
            break;

          case 29:
            adc_putreg(priv, STM32_ADC_SMPR1_OFFSET, value);
            shift = 0;
            value = 0;
            break;

          case 31:
            adc_putreg(priv, STM32_ADC_SMPR0_OFFSET, value);
            shift = 0;
            value = 0;
            break;

          default:
            shift++;
            break;
        }
    }
}
#endif

/****************************************************************************
 * Name: adc_dmacovcallback
 *
 * Description:
 *   Callback for DMA.  Called from the DMA transfer complete interrupt after
 *   all channels have been converted and transferred with DMA.
 *
 * Input Parameters:
 *
 *   handle - handle to DMA
 *   isr -
 *   arg - adc device
 *
 * Returned Value:
 *
 ****************************************************************************/

#ifdef ADC_HAVE_DMA
static void adc_dmaconvcallback(DMA_HANDLE handle, uint8_t isr, void *arg)
{
  FAR struct adc_dev_s *dev = (FAR struct adc_dev_s*) arg;
  FAR struct stm32_dev_s *priv = dev->ad_priv;
  uint32_t regval;
  int i;

  for (i = 0; i < priv->nchannels; i++)
    {
      adc_receive(dev, priv->current, priv->dmabuffer[priv->current]);
      priv->current++;
      if (priv->current >= priv->nchannels)
        {
          /* Restart the conversion sequence from the beginning */

          priv->current = 0;
        }
    }

  /* Restart DMA for the next conversion series */

  regval  = adc_getreg(priv, STM32_ADC_CR2_OFFSET);
  regval &= ~ADC_CR2_DMA;
  adc_putreg(priv, STM32_ADC_CR2_OFFSET, regval);

  regval |= ADC_CR2_DMA;
  adc_putreg(priv, STM32_ADC_CR2_OFFSET, regval);
}
#endif

/****************************************************************************
 * Name: adc_reset
 *
 * Description:
 *   Reset the ADC device.  Called early to initialize the hardware. This
 *   is called, before adc_setup() and on error conditions.
 *
 * Input Parameters:
 *
 * Returned Value:
 *
 ****************************************************************************/

static void adc_reset(FAR struct adc_dev_s *dev)
{
  FAR struct stm32_dev_s *priv = (FAR struct stm32_dev_s *)dev->ad_priv;
  irqstate_t flags;
  uint32_t regval;
#ifdef ADC_HAVE_TIMER
  int ret;
#endif

  allvdbg("intf: ADC%d\n", priv->intf);
  flags = irqsave();

  /* In STM32L15XX family HSI used as an independent clock-source for the ADC */

#if defined(CONFIG_STM32_STM32L15XX) && ((STM32_CFGR_PLLSRC != 0) || \
    (STM32_SYSCLK_SW != RCC_CFGR_SW_HSI))

  adc_enable_hsi(true);

#endif

  /* Enable ADC reset state */

  adc_rccreset(priv, true);

  /* Release ADC from reset state */

  adc_rccreset(priv, false);

  /* Initialize the ADC data structures */

  /* Initialize the watchdog high threshold register */

  adc_putreg(priv, STM32_ADC_HTR_OFFSET, 0x00000fff);

  /* Initialize the watchdog low threshold register */

  adc_putreg(priv, STM32_ADC_LTR_OFFSET, 0x00000000);

  /* Initialize the same sample time for each ADC 55.5 cycles
   *
   * During sample cycles channel selection bits must remain unchanged.
   * For F-family only
   *
   *   000:   1.5 cycles
   *   001:   7.5 cycles
   *   010:  13.5 cycles
   *   011:  28.5 cycles
   *   100:  41.5 cycles
   *   101:  55.5 cycles
   *   110:  71.5 cycles
   *   111: 239.5 cycles
   */

#ifndef CONFIG_STM32_STM32L15XX
  adc_putreg(priv, STM32_ADC_SMPR1_OFFSET, 0x00b6db6d);
  adc_putreg(priv, STM32_ADC_SMPR2_OFFSET, 0x00b6db6d);
#else
  adc_write_sample_time_registers(dev);
#endif

  /* ADC CR1 Configuration */

  regval  = adc_getreg(priv, STM32_ADC_CR1_OFFSET);

  /* Set mode configuration (Independent mode) */

#ifdef CONFIG_STM32_STM32F10XX
  regval |= ADC_CR1_IND;
#endif

  /* Initialize the Analog watchdog enable */

  regval |= ADC_CR1_AWDEN;
  regval |= (priv->chanlist[0] << ADC_CR1_AWDCH_SHIFT);

  /* Enable interrupt flags */

  regval |= ADC_CR1_ALLINTS;

#ifdef ADC_HAVE_DMA
  if (priv->hasdma)
    {
      regval |= ADC_CR1_SCAN;
    }
#endif

#if defined(CONFIG_STM32_STM32F20XX) || defined(CONFIG_STM32_STM32F40XX) || \
    defined(CONFIG_STM32_STM32L15XX)

  /* Enable or disable Overrun interrupt */

  regval &= ~ADC_CR1_OVRIE;

  /* Set the resolution of the conversion */

  regval |= ADC_CR1_RES_12BIT;
#endif

  adc_putreg(priv, STM32_ADC_CR1_OFFSET, regval);

#ifdef CONFIG_STM32_STM32L15XX

  /* Disables power down during the delay phase */

  adc_power_down_idle(priv, false);
  adc_power_down_delay(priv, false);
#endif

#ifdef CONFIG_STM32_STM32L15XX

  /* Select the bank of channels A */

  adc_select_ch_bank(priv, false);

  /* Delay until the converted data has been read */

  adc_dels_after_conversion(priv, ADC_CR2_DELS_TILLRD);
#endif

  /* ADC CR2 Configuration */

  regval  = adc_getreg(priv, STM32_ADC_CR2_OFFSET);

  /* Clear CONT, continuous mode disable */

  regval &= ~ADC_CR2_CONT;

  /* Set ALIGN (Right = 0) */

  regval &= ~ADC_CR2_ALIGN;

#if defined(CONFIG_STM32_STM32F20XX) || defined(CONFIG_STM32_STM32F40XX) || \
    defined(CONFIG_STM32_STM32L15XX)
  /* External trigger enable for regular channels */

  regval |= ADC_CR2_EXTEN_NONE;
#endif

#ifdef ADC_HAVE_DMA
  if (priv->hasdma)
    {
      regval |= ADC_CR2_DMA;
    }
#endif

  adc_putreg(priv, STM32_ADC_CR2_OFFSET, regval);

  /* Configuration of the channel conversions */

#if ADC_MAX_SAMPLES == 1
  /* Select on first indexed channel for backward compatibility. */
  adc_set_ch_idx(dev,0);
#else
  adc_set_ch(dev,0);
#endif

  /* ADC CCR configuration */

#if defined(CONFIG_STM32_STM32F20XX) || defined(CONFIG_STM32_STM32F40XX)
  regval  = getreg32(STM32_ADC_CCR);
  regval &= ~(ADC_CCR_MULTI_MASK | ADC_CCR_DELAY_MASK | ADC_CCR_DDS |
              ADC_CCR_DMA_MASK | ADC_CCR_ADCPRE_MASK | ADC_CCR_VBATE |
              ADC_CCR_TSVREFE);
  regval |=  (ADC_CCR_MULTI_NONE | ADC_CCR_DMA_DISABLED |
              ADC_CCR_ADCPRE_DIV2);
  putreg32(regval, STM32_ADC_CCR);

#elif defined(CONFIG_STM32_STM32L15XX)
  regval = getreg32(STM32_ADC_CCR);
  regval &= ~(ADC_CCR_ADCPRE_MASK | ADC_CCR_TSVREFE);
  regval |= ADC_CCR_ADCPRE_DIV2;
  putreg32(regval, STM32_ADC_CCR);
#endif

  /* Set the number of conversions */

#ifdef ADC_HAVE_DMA
  if (priv->hasdma)
    {
      DEBUGASSERT(priv->nchannels <= ADC_MAX_CHANNELS_DMA);
    }
  else
#endif
    {
      DEBUGASSERT(priv->nchannels <= ADC_MAX_CHANNELS_NODMA);
    }

  regval |= (((uint32_t)priv->nchannels - 1) << ADC_SQR1_L_SHIFT);
  adc_putreg(priv, STM32_ADC_SQR1_OFFSET, regval);

  /* Set the channel index of the first conversion */

  priv->current = 0;

#ifdef ADC_HAVE_DMA
  /* Enable DMA */

  if (priv->hasdma)
    {
      uint32_t ccr;

      /* Stop and free DMA if it was started before */

      if (priv->dma != NULL)
        {
          stm32_dmastop(priv->dma);
          stm32_dmafree(priv->dma);
        }

      priv->dma = stm32_dmachannel(priv->dmachan);
      ccr       = DMA_SCR_MSIZE_16BITS | /* Memory size */
                  DMA_SCR_PSIZE_16BITS | /* Peripheral size */
                  DMA_SCR_MINC |         /* Memory increment mode */
                  DMA_SCR_CIRC |         /* Circular buffer */
                  DMA_SCR_DIR_P2M;       /* Read from peripheral */

      stm32_dmasetup(priv->dma,
                     priv->base + STM32_ADC_DR_OFFSET,
                     (uint32_t)priv->dmabuffer,
                     priv->nchannels,
                     ccr);

      stm32_dmastart(priv->dma, adc_dmaconvcallback, dev, false);
    }
#endif

  /* Set ADON to wake up the ADC from Power Down state. */

  adc_enable(priv, true);

#ifdef ADC_HAVE_TIMER
  ret = adc_timinit(priv);
  if (ret!=OK)
   {
      adbg("Error initializing the timers\n");
   }
#elif !defined(CONFIG_ADC_NO_STARTUP_CONV)

#ifdef CONFIG_STM32_STM32F10XX
  /* Set ADON (Again) to start the conversion.  Only if Timers are not
   * configured as triggers
   */

  adc_enable(priv, true);
#else
  adc_startconv(priv, true);
#endif /* CONFIG_STM32_STM32F10XX */

#endif /* ADC_HAVE_TIMER */

  irqrestore(flags);

  avdbg("SR:   0x%08x CR1:  0x%08x CR2:  0x%08x\n",
        adc_getreg(priv, STM32_ADC_SR_OFFSET),
        adc_getreg(priv, STM32_ADC_CR1_OFFSET),
        adc_getreg(priv, STM32_ADC_CR2_OFFSET));
  avdbg("SQR1: 0x%08x SQR2: 0x%08x SQR3: 0x%08x\n",
        adc_getreg(priv, STM32_ADC_SQR1_OFFSET),
        adc_getreg(priv, STM32_ADC_SQR2_OFFSET),
        adc_getreg(priv, STM32_ADC_SQR3_OFFSET));
#ifdef CONFIG_STM32_STM32L15XX
  avdbg("SQR4: 0x%08x SQR5: 0x%08x\n",
        adc_getreg(priv, STM32_ADC_SQR4_OFFSET),
        adc_getreg(priv, STM32_ADC_SQR5_OFFSET));
#endif

#if defined(CONFIG_STM32_STM32F20XX) || defined(CONFIG_STM32_STM32F40XX) || \
    defined(CONFIG_STM32_STM32L15XX)
  avdbg("CCR:  0x%08x\n",
        getreg32(STM32_ADC_CCR));
#endif
}

/****************************************************************************
 * Name: adc_reset_hsi_disable
 *
 * Description:
 *   Reset the ADC device with HSI and ADC shut down. Called early to
 *   initialize the hardware. This is called, before adc_setup() and on
 *   error conditions. In STM32L15XX case sometimes HSI must be shut
 *   down after the first initialization
 *
 * Input Parameters:
 *
 * Returned Value:
 *
 ****************************************************************************/

#if defined(CONFIG_STM32_STM32L15XX) && ((STM32_CFGR_PLLSRC != 0) || \
    (STM32_SYSCLK_SW != RCC_CFGR_SW_HSI))
static void adc_reset_hsi_disable(FAR struct adc_dev_s *dev)
{
    adc_reset(dev);
    adc_shutdown(dev);
}
#endif

/****************************************************************************
 * Name: adc_setup
 *
 * Description:
 *   Configure the ADC. This method is called the first time that the ADC
 *   device is opened.  This will occur when the port is first opened.
 *   This setup includes configuring and attaching ADC interrupts.  Interrupts
 *   are all disabled upon return.
 *
 * Input Parameters:
 *
 * Returned Value:
 *
 ****************************************************************************/

static int adc_setup(FAR struct adc_dev_s *dev)
{
  FAR struct stm32_dev_s *priv = (FAR struct stm32_dev_s *)dev->ad_priv;
  int ret;

  /* Attach the ADC interrupt */

  ret = irq_attach(priv->irq, priv->isr);
  if (ret == OK)
    {
      /* Make sure that the ADC device is in the powered up, reset state */

      adc_reset(dev);

      /* Enable the ADC interrupt */

      avdbg("Enable the ADC interrupt: irq=%d\n", priv->irq);
      up_enable_irq(priv->irq);
    }

  return ret;
}

/****************************************************************************
 * Name: adc_shutdown
 *
 * Description:
 *   Disable the ADC.  This method is called when the ADC device is closed.
 *   This method reverses the operation the setup method.
 *
 * Input Parameters:
 *
 * Returned Value:
 *
 ****************************************************************************/

static void adc_shutdown(FAR struct adc_dev_s *dev)
{
  FAR struct stm32_dev_s *priv = (FAR struct stm32_dev_s *)dev->ad_priv;

#ifdef CONFIG_STM32_STM32L15XX
  adc_enable(priv, false);
#  if (STM32_CFGR_PLLSRC != 0) || (STM32_SYSCLK_SW != RCC_CFGR_SW_HSI)
     adc_enable_hsi(false);
#  endif
#endif

  /* Disable ADC interrupts and detach the ADC interrupt handler */

  up_disable_irq(priv->irq);
  irq_detach(priv->irq);

  /* Disable and reset the ADC module */

  adc_rccreset(priv, true);
}

/****************************************************************************
 * Name: adc_rxint
 *
 * Description:
 *   Call to enable or disable RX interrupts.
 *
 * Input Parameters:
 *
 * Returned Value:
 *
 ****************************************************************************/

static void adc_rxint(FAR struct adc_dev_s *dev, bool enable)
{
  FAR struct stm32_dev_s *priv = (FAR struct stm32_dev_s *)dev->ad_priv;
  uint32_t regval;

  avdbg("intf: %d enable: %d\n", priv->intf, enable);

  regval = adc_getreg(priv, STM32_ADC_CR1_OFFSET);

#ifndef CONFIG_STM32_HAVE_ONLY_EOCIE
  if (enable)
    {
      /* Enable the end-of-conversion ADC and analog watchdog interrupts */

      regval |= ADC_CR1_ALLINTS;
    }
#else
  if (enable)
    {

      /* Clear all interrupts */

      regval &= ~ADC_CR1_ALLINTS;

      /* Enable the end-of-conversion ADC interrupt */

      regval |= ADC_CR1_EOCIE;
    }
#endif
  else
    {
      /* Disable all ADC interrupts */

      regval &= ~ADC_CR1_ALLINTS;
    }

  adc_putreg(priv, STM32_ADC_CR1_OFFSET, regval);
}

/****************************************************************************
 * Name: adc_enable_tvref_register
 *
 * Description:
 *   Enable/disable the temperature sensor and the VREFINT channel.
 *
 * Input Parameters:
 *   dev - pointer to device structure used by the driver
 *   enable - true:  Temperature sensor and V REFINT channel enabled (ch 16 and 17)
 *            false: Temperature sensor and V REFINT channel disabled (ch 16 and 17)
 *
 * Returned Value:
 *   None.
 *
 ****************************************************************************/

#ifdef CONFIG_STM32_STM32L15XX
static void adc_ioc_enable_tvref_register(FAR struct adc_dev_s *dev, bool enable)
{
  uint32_t regval;

  regval = getreg32(STM32_ADC_CCR);

  if (enable)
    {
      regval |= ADC_CCR_TSVREFE;
    }
  else
    {
      regval &= ~ADC_CCR_TSVREFE;
    }

  putreg32(regval, STM32_ADC_CCR);
  allvdbg("STM32_ADC_CCR value: 0x%08X\n", getreg32(STM32_ADC_CCR));
}
#endif

/****************************************************************************
 * Name: adc_ioc_change_sleep_between_opers
 *
 * Description:
 *   Changes PDI and PDD bits to save battery.
 *
 * Input Parameters:
 *   dev - pointer to device structure used by the driver
 *   cmd - command
 *   arg - arguments passed with command
 *
 * Returned Value:
 *
 ****************************************************************************/

#ifdef CONFIG_STM32_STM32L15XX
static int adc_ioc_change_sleep_between_opers(FAR struct adc_dev_s *dev,
                                              int cmd, bool arg)
{
  int ret = OK;
  FAR struct stm32_dev_s *priv = (FAR struct stm32_dev_s *)dev->ad_priv;

  adc_enable(priv, false);

  switch (cmd)
    {
      case IO_ENABLE_DISABLE_PDI:
        adc_power_down_idle(priv, arg);
        break;

      case IO_ENABLE_DISABLE_PDD:
        adc_power_down_delay(priv, arg);
        break;

      case IO_ENABLE_DISABLE_PDD_PDI:
        adc_power_down_idle(priv, arg);
        adc_power_down_delay(priv, arg);
        break;

      default:
        avdbg("unknown cmd: %d\n", cmd);
        break;
    }

  adc_enable(priv, true);

  return ret;
}
#endif

/****************************************************************************
 * Name: adc_ioc_enable_awd_int
 *
 * Description:
 *   Turns ON/OFF ADC analog watch-dog interrupt.
 *
 * Input Parameters:
 *   dev - pointer to device structure used by the driver
 *   arg - true:  Turn ON interrupt
 *         false: Turn OFF interrupt
 *
 * Returned Value:
 *
 ****************************************************************************/

#ifdef CONFIG_STM32_STM32L15XX
static void adc_ioc_enable_awd_int(FAR struct stm32_dev_s *priv, bool enable)
{
  uint32_t regval = 0;

  regval = adc_getreg(priv, STM32_ADC_CR1_OFFSET);
  if (enable)
    {
      regval |= ADC_CR1_AWDIE;
    }
  else
    {
      regval &= ~ADC_CR1_AWDIE;
    }

  adc_putreg(priv, STM32_ADC_CR1_OFFSET, regval);
}
#endif

/****************************************************************************
 * Name: adc_ioc_enable_eoc_int
 *
 * Description:
 *   Turns ON/OFF ADC EOC interrupt.
 *
 * Input Parameters:
 *   dev - pointer to device structure used by the driver
 *   arg - true:  Turn ON interrupt
 *         false: Turn OFF interrupt
 *
 * Returned Value:
 *
 ****************************************************************************/

#ifdef CONFIG_STM32_STM32L15XX
static void adc_ioc_enable_eoc_int(FAR struct stm32_dev_s *priv, bool enable)
{
  uint32_t regval = 0;

  regval = adc_getreg(priv, STM32_ADC_CR1_OFFSET);
  if (enable)
    {
      regval |= ADC_CR1_EOCIE;
    }
  else
    {
      regval &= ~ADC_CR1_EOCIE;
    }

  adc_putreg(priv, STM32_ADC_CR1_OFFSET, regval);
}
#endif

/****************************************************************************
 * Name: adc_ioc_enable_jeoc_int
 *
 * Description:
 *   Turns ON/OFF ADC injected channels interrupt.
 *
 * Input Parameters:
 *   dev - pointer to device structure used by the driver
 *   arg - true:  Turn ON interrupt
 *         false: Turn OFF interrupt
 *
 * Returned Value:
 *
 ****************************************************************************/

#ifdef CONFIG_STM32_STM32L15XX
static void adc_ioc_enable_jeoc_int(FAR struct stm32_dev_s *priv, bool enable)
{
  uint32_t regval = 0;

  regval = adc_getreg(priv, STM32_ADC_CR1_OFFSET);
  if (enable)
    {
      regval |= ADC_CR1_JEOCIE;
    }
  else
    {
      regval &= ~ADC_CR1_JEOCIE;
    }

  adc_putreg(priv, STM32_ADC_CR1_OFFSET, regval);
}
#endif

/****************************************************************************
 * Name: adc_ioc_enable_ovr_int
 *
 * Description:
 *   Turns ON/OFF ADC overrun interrupt.
 *
 * Input Parameters:
 *   dev - pointer to device structure used by the driver
 *   arg - true:  Turn ON interrupt
 *         false: Turn OFF interrupt
 *
 * Returned Value:
 *
 ****************************************************************************/

#ifdef CONFIG_STM32_STM32L15XX
static void adc_ioc_enable_ovr_int(FAR struct stm32_dev_s *priv, bool enable)
{
  uint32_t regval = 0;

  regval = adc_getreg(priv, STM32_ADC_CR1_OFFSET);
  if (enable)
    {
      regval |= ADC_CR1_OVRIE;
    }
  else
    {
      regval &= ~ADC_CR1_OVRIE;
    }

  adc_putreg(priv, STM32_ADC_CR1_OFFSET, regval);
}
#endif

/****************************************************************************
 * Name: adc_ioc_change_ints
 *
 * Description:
 *   Turns ON/OFF ADC interrupts.
 *
 * Input Parameters:
 *   dev - pointer to device structure used by the driver
 *   cmd - command
 *   arg - arguments passed with command
 *
 * Returned Value:
 *
 ****************************************************************************/

#ifdef CONFIG_STM32_STM32L15XX
static int adc_ioc_change_ints(FAR struct adc_dev_s *dev, int cmd, bool arg)
{
  int ret = OK;
  FAR struct stm32_dev_s *priv = (FAR struct stm32_dev_s *)dev->ad_priv;

  switch (cmd)
    {
      case IO_ENABLE_DISABLE_AWDIE:
        adc_ioc_enable_awd_int(priv, arg);
        break;

      case IO_ENABLE_DISABLE_EOCIE:
        adc_ioc_enable_eoc_int(priv, arg);
        break;

      case IO_ENABLE_DISABLE_JEOCIE:
        adc_ioc_enable_jeoc_int(priv, arg);
        break;

      case IO_ENABLE_DISABLE_OVRIE:
        adc_ioc_enable_ovr_int(priv, arg);
        break;

      case IO_ENABLE_DISABLE_ALL_INTS:
        adc_ioc_enable_awd_int(priv, arg);
        adc_ioc_enable_eoc_int(priv, arg);
        adc_ioc_enable_jeoc_int(priv, arg);
        adc_ioc_enable_ovr_int(priv, arg);
        break;

      default:
        avdbg("unknown cmd: %d\n", cmd);
        break;
    }

  return ret;
}
#endif

/****************************************************************************
 * Name: adc_ioc_wait_rcnr_zeroed
 *
 * Description:
 *   For the STM3215XX-family the ADC_SR_RCNR bit must be zeroed,
 *   before next conversion.
 *
 * Input Parameters:
 *   dev - pointer to device structure used by the driver
 *
 * Returned Value:
 *
 ****************************************************************************/

#ifdef CONFIG_STM32_STM32L15XX
static int adc_ioc_wait_rcnr_zeroed(FAR struct stm32_dev_s *priv)
{
  int i = 0;
  uint32_t regval = 0;

  for (i = 0; i < 30000; i++)
    {
      if (!((regval = adc_getreg(priv, STM32_ADC_SR_OFFSET)) & ADC_SR_RCNR))
        {
          return OK;
        }
    }

  return -ENODATA;
}
#endif

/****************************************************************************
 * Name: adc_enable_hsi
 *
 * Description:
 *   Enable/Disable HSI clock
 *
 * Input Parameters:
 *   enable - true  : HSI clock for ADC enabled
 *            false : HSI clock for ADC disabled
 *
 * Returned Value:
 *
 ****************************************************************************/

#if defined(CONFIG_STM32_STM32L15XX) && ((STM32_CFGR_PLLSRC != 0) || \
    (STM32_SYSCLK_SW != RCC_CFGR_SW_HSI))
static void adc_enable_hsi(bool enable)
{
  uint32_t regval = 0;

  if (enable)
    {
      regval  = getreg32(STM32_RCC_CR);
      regval |= RCC_CR_HSION;
      putreg32(regval, STM32_RCC_CR);     /* Enable the HSI */
      while ((getreg32(STM32_RCC_CR) & RCC_CR_HSIRDY) == 0);
    }
  else
    {
      regval  = getreg32(STM32_RCC_CR);
      regval &= ~RCC_CR_HSION;
      putreg32(regval, STM32_RCC_CR);    /* Disable the HSI */
    }
}
#endif

/****************************************************************************
 * Name: adc_set_ch_idx
 *
 * Description:
 *   Set single channel for adc conversion. Channel selected from
 *   configured channel list by index.
 *
 * Input Parameters:
 *   dev - pointer to device structure used by the driver
 *   idx - channel index in configured channel list
 *
 * Returned Value:
 *   int - errno
 *
 ****************************************************************************/

static int adc_set_ch_idx(FAR struct adc_dev_s *dev, uint8_t idx)
{
  FAR struct stm32_dev_s *priv = (FAR struct stm32_dev_s *)dev->ad_priv;
  int32_t regval;

  if (idx < priv->cchannels)
    {
#ifdef CONFIG_STM32_STM32L15XX
      regval = adc_getreg(priv, STM32_ADC_SQR5_OFFSET) & ADC_SQR3_RESERVED;
#else
      regval = adc_getreg(priv, STM32_ADC_SQR3_OFFSET) & ADC_SQR3_RESERVED;
#endif
      regval |= (uint32_t)priv->chanlist[idx];
#ifdef CONFIG_STM32_STM32L15XX
      adc_putreg(priv, STM32_ADC_SQR5_OFFSET, regval);
#else
      adc_putreg(priv, STM32_ADC_SQR3_OFFSET, regval);
#endif
      return 0;
    }
  else
    {
      return -ENODEV;
    }
}

/****************************************************************************
 * Name: adc_set_ch
 *
 * Description:
 *   Sets the ADC channel.
 *
 * Input Parameters:
 *   dev - pointer to device structure used by the driver
 *   ch  - ADC channel number + 1. 0 reserved for all configured channels
 *
 * Returned Value:
 *   int - errno
 *
 ****************************************************************************/

static int adc_set_ch(FAR struct adc_dev_s *dev, uint8_t ch)
{
  FAR struct stm32_dev_s *priv = (FAR struct stm32_dev_s *)dev->ad_priv;
  uint32_t regval;
  int i, ret = 0;

  if (ch == 0)
    {
      int offset;

#ifdef CONFIG_STM32_STM32L15XX
      priv->nchannels = priv->cchannels;
      regval = adc_getreg(priv, STM32_ADC_SQR5_OFFSET) & ADC_SQR5_RESERVED;
      for (i = 0, offset = 0; i < priv->nchannels && i < 6; i++, offset += 5)
        {
          regval |= (uint32_t)priv->chanlist[i] << offset;
        }

      adc_putreg(priv, STM32_ADC_SQR5_OFFSET, regval);

      regval = adc_getreg(priv, STM32_ADC_SQR4_OFFSET) & ADC_SQR4_RESERVED;
      for (i = 6, offset = 0; i < priv->nchannels && i < 12; i++, offset += 5)
        {
          regval |= (uint32_t)priv->chanlist[i] << offset;
        }

      adc_putreg(priv, STM32_ADC_SQR4_OFFSET, regval);

      regval = adc_getreg(priv, STM32_ADC_SQR3_OFFSET) & ADC_SQR3_RESERVED;
      for (i = 12, offset = 0; i < priv->nchannels && i < 18; i++, offset += 5)
        {
          regval |= (uint32_t)priv->chanlist[i] << offset;
        }

      adc_putreg(priv, STM32_ADC_SQR3_OFFSET, regval);

      regval = adc_getreg(priv, STM32_ADC_SQR2_OFFSET) & ADC_SQR2_RESERVED;
      for (i = 18, offset = 0; i < priv->nchannels && i < 24; i++, offset += 5)
        {
          regval |= (uint32_t)priv->chanlist[i] << offset;
        }

      adc_putreg(priv, STM32_ADC_SQR2_OFFSET, regval);

      regval = adc_getreg(priv, STM32_ADC_SQR1_OFFSET) & ADC_SQR1_RESERVED;
      for (i = 24, offset = 0; i < priv->nchannels && i < 28; i++, offset += 5)
      {
        regval |= (uint32_t)priv->chanlist[i] << offset;
      }

#else
      priv->nchannels = priv->cchannels;
      regval = adc_getreg(priv, STM32_ADC_SQR3_OFFSET) & ADC_SQR3_RESERVED;
      for (i = 0, offset = 0; i < priv->nchannels && i < 6; i++, offset += 5)
        {
          regval |= (uint32_t)priv->chanlist[i] << offset;
        }

      adc_putreg(priv, STM32_ADC_SQR3_OFFSET, regval);

      regval = adc_getreg(priv, STM32_ADC_SQR2_OFFSET) & ADC_SQR2_RESERVED;
      for (i = 6, offset = 0; i < priv->nchannels && i < 12; i++, offset += 5)
        {
          regval |= (uint32_t)priv->chanlist[i] << offset;
        }

      adc_putreg(priv, STM32_ADC_SQR2_OFFSET, regval);

      regval = adc_getreg(priv, STM32_ADC_SQR1_OFFSET) & ADC_SQR1_RESERVED;
      for (i = 12, offset = 0; i < priv->nchannels && i < 16; i++, offset += 5)
        {
          regval |= (uint32_t)priv->chanlist[i] << offset;
        }
#endif
    }
  else
    {
      ch = ch - 1;
      for (i = 0; i < priv->cchannels; i++)
        {
          if ((uint32_t)priv->chanlist[i] == ch)
            {
              ret = adc_set_ch_idx(dev,i);
              if (ret < 0)
                {
                  break;
                }

              regval  = adc_getreg(priv, STM32_ADC_SQR1_OFFSET) & ADC_SQR1_RESERVED;
              regval &= ~(ADC_SQR1_L_MASK);
              adc_putreg(priv, STM32_ADC_SQR1_OFFSET, regval);

              priv->current   = i;
              priv->nchannels = 1;
              return ret;
            }
        }

      ret = -ENODEV;
    }

    return ret;
}

/****************************************************************************
 * Name: adc_ioctl
 *
 * Description:
 *   All ioctl calls will be routed through this method.
 *
 * Input Parameters:
 *   dev - pointer to device structure used by the driver
 *   cmd - command
 *   arg - arguments passed with command
 *
 * Returned Value:
 *
 ****************************************************************************/

static int adc_ioctl(FAR struct adc_dev_s *dev, int cmd, unsigned long arg)
{
  FAR struct stm32_dev_s * priv = (FAR struct stm32_dev_s *)dev->ad_priv;
  int ret = OK;

  switch (cmd)
    {
#ifdef ADC_HAVE_DMA
      case ANIOC_TRIGGER:
        adc_startconv(priv, true);
        break;
#endif

#ifdef CONFIG_STM32_STM32L15XX
      case IO_ENABLE_TEMPER_VOLT_CH:
        adc_ioc_enable_tvref_register(dev, *(bool *)arg);
        break;

      case IO_ENABLE_DISABLE_PDI:
      case IO_ENABLE_DISABLE_PDD:
      case IO_ENABLE_DISABLE_PDD_PDI:
        adc_ioc_change_sleep_between_opers(dev, cmd, *(bool *)arg);
        break;

      case IO_ENABLE_DISABLE_AWDIE:
      case IO_ENABLE_DISABLE_EOCIE:
      case IO_ENABLE_DISABLE_JEOCIE:
      case IO_ENABLE_DISABLE_OVRIE:
      case IO_ENABLE_DISABLE_ALL_INTS:
        adc_ioc_change_ints(dev, cmd, *(bool*)arg);
        break;

      case IO_START_CONV:
        {
          uint8_t ch = ((uint8_t)arg);

          ret = adc_ioc_wait_rcnr_zeroed(priv);
          if (ret < 0)
            {
              set_errno(-ret);
              return ret;
            }

          ret = adc_set_ch(dev,ch);
          if (ret < 0)
            {
              set_errno(-ret);
              return ret;
            }

          if (ch)
            {
              /* Clear fifo */

              dev->ad_recv.af_head = 0;
              dev->ad_recv.af_tail = 0;
            }

          adc_startconv(priv, true);
          break;
        }

#if (STM32_CFGR_PLLSRC != 0 || STM32_SYSCLK_SW != RCC_CFGR_SW_HSI)
      case IO_STOP_ADC:
        adc_enable(priv, false);
        adc_enable_hsi(false);
        break;

      case IO_START_ADC:
        adc_enable_hsi(true);
        adc_enable(priv, true);
        break;
#endif
#endif /* CONFIG_STM32_STM32L15XX */

      default:
        adbg("ERROR: Unknown cmd: %d\n", cmd);
        ret = -ENOTTY;
    }

  UNUSED(priv);
  return ret;
}

/****************************************************************************
 * Name: adc_interrupt
 *
 * Description:
 *   Common ADC interrupt handler.
 *
 * Input Parameters:
 *
 * Returned Value:
 *
 ****************************************************************************/

static int adc_interrupt(FAR struct adc_dev_s *dev)
{
  FAR struct stm32_dev_s *priv = (FAR struct stm32_dev_s *)dev->ad_priv;
  uint32_t adcsr;
  int32_t  value;

  /* Identifies the interruption AWD, OVR or EOC */

  adcsr = adc_getreg(priv, STM32_ADC_SR_OFFSET);
  if ((adcsr & ADC_SR_AWD) != 0)
    {
      alldbg("WARNING: Analog Watchdog, Value converted out of range!\n");
    }

#if defined(CONFIG_STM32_STM32F20XX) || defined(CONFIG_STM32_STM32F40XX) || \
    defined(CONFIG_STM32_STM32L15XX)
  if ((adcsr & ADC_SR_OVR) != 0)
    {
      alldbg("WARNING: Overrun has occurred!\n");
    }
#endif

  /* EOC: End of conversion */

  if ((adcsr & ADC_SR_EOC) != 0)
    {
      /* Read the converted value and clear EOC bit
       * (It is cleared by reading the ADC_DR)
       */

      value  = adc_getreg(priv, STM32_ADC_DR_OFFSET);
      value &= ADC_DR_DATA_MASK;

      /* Give the ADC data to the ADC driver.  adc_receive accepts 3
       * parameters:
       *
       * 1) The first is the ADC device instance for this ADC block.
       * 2) The second is the channel number for the data, and
       * 3) The third is the converted data for the channel.
       */

      adc_receive(dev, priv->chanlist[priv->current], value);

      /* Set the channel number of the next channel that will complete
       * conversion.
       */

      priv->current++;

      if (priv->current >= priv->nchannels)
        {
          /* Restart the conversion sequence from the beginning */

          priv->current = 0;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: adc12_interrupt
 *
 * Description:
 *   ADC12 interrupt handler for the STM32 F1 family.
 *
 * Input Parameters:
 *
 * Returned Value:
 *
 ****************************************************************************/

#if defined(CONFIG_STM32_STM32F10XX) && (defined(CONFIG_STM32_ADC1) || \
    defined(CONFIG_STM32_ADC2))
static int adc12_interrupt(int irq, void *context)
{
  uint32_t regval;
  uint32_t pending;

  /* Check for pending ADC1 interrupts */

#ifdef CONFIG_STM32_ADC1
  regval  = getreg32(STM32_ADC1_SR);
  pending = regval & ADC_SR_ALLINTS;
  if (pending != 0)
    {
      adc_interrupt(&g_adcdev1);
      regval &= ~pending;
      putreg32(regval, STM32_ADC1_SR);
    }
#endif

  /* Check for pending ADC2 interrupts */

#ifdef CONFIG_STM32_ADC2
  regval  = getreg32(STM32_ADC2_SR);
  pending = regval & ADC_SR_ALLINTS;
  if (pending != 0)
    {
      adc_interrupt(&g_adcdev2);
      regval &= ~pending;
      putreg32(regval, STM32_ADC2_SR);
    }
#endif
  return OK;
}
#endif

/****************************************************************************
 * Name: adc3_interrupt
 *
 * Description:
 *   ADC1/2 interrupt handler for the STM32 F1 family.
 *
 * Input Parameters:
 *
 * Returned Value:
 *
 ****************************************************************************/

#if defined (CONFIG_STM32_STM32F10XX) && defined (CONFIG_STM32_ADC3)
static int adc3_interrupt(int irq, void *context)
{
  uint32_t regval;
  uint32_t pending;

  /* Check for pending ADC3 interrupts */

  regval  = getreg32(STM32_ADC3_SR);
  pending = regval & ADC_SR_ALLINTS;
  if (pending != 0)
    {
      adc_interrupt(&g_adcdev3);
      regval &= ~pending;
      putreg32(regval, STM32_ADC3_SR);
    }

  return OK;
}
#endif

/****************************************************************************
 * Name: adc123_interrupt
 *
 * Description:
 *   ADC interrupt handler for the STM32 L15XX family.
 *
 * Input Parameters:
 *   irq     - The IRQ number that generated the interrupt.
 *   context - Architecture specific register save information.
 *
 * Returned Value:
 *
 ****************************************************************************/

#ifdef CONFIG_STM32_STM32L15XX
static int adc_stm32l_interrupt(int irq, void *context)
{
  uint32_t regval;
  uint32_t pending;

  /* STM32L15XX-family has the only ADC. For the sake of the simplicity the ADC1
   * name is used everywhere
   */

#ifdef CONFIG_STM32_ADC1
  regval  = getreg32(STM32_ADC1_SR);
  pending = regval & ADC_SR_ALLINTS;
  if (pending != 0)
    {
      adc_interrupt(&g_adcdev1);
      regval &= ~pending;
      putreg32(regval, STM32_ADC1_SR);
    }
#endif

  return OK;
}
#endif

/****************************************************************************
 * Name: adc123_interrupt
 *
 * Description:
 *   ADC1/2/3 interrupt handler for the STM32 F4 family.
 *
 * Input Parameters:
 *
 * Returned Value:
 *
 ****************************************************************************/

#if defined(CONFIG_STM32_STM32F20XX) || defined(CONFIG_STM32_STM32F40XX)
static int adc123_interrupt(int irq, void *context)
{
  uint32_t regval;
  uint32_t pending;

  /* Check for pending ADC1 interrupts */

#ifdef CONFIG_STM32_ADC1
  regval  = getreg32(STM32_ADC1_SR);
  pending = regval & ADC_SR_ALLINTS;
  if (pending != 0)
    {
      adc_interrupt(&g_adcdev1);
      regval &= ~pending;
      putreg32(regval, STM32_ADC1_SR);
    }
#endif

  /* Check for pending ADC2 interrupts */

#ifdef CONFIG_STM32_ADC2
  regval = getreg32(STM32_ADC2_SR);
  pending = regval & ADC_SR_ALLINTS;
  if (pending != 0)
    {
      adc_interrupt(&g_adcdev2);
      regval &= ~pending;
      putreg32(regval, STM32_ADC2_SR);
    }
#endif

  /* Check for pending ADC3 interrupts */

#ifdef CONFIG_STM32_ADC3
  regval = getreg32(STM32_ADC3_SR);
  pending = regval & ADC_SR_ALLINTS;
  if (pending != 0)
    {
      adc_interrupt(&g_adcdev3);
      regval &= ~pending;
      putreg32(regval, STM32_ADC3_SR);
    }
#endif
  return OK;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/*******************************************************************************
 * Name: adc_change_sample_time
 *
 * Description    : Changes sample times for specified channels. This method
 *  doesn't make any register writing. So, it's only stores the information.
 *  Values provided by user will be written in registers only on the next adc
 *  peripheral start, as it was told to do in manual. However, before very first
 *  start, user can call this method and override default values either
 *  for every channels or for only some predefined by user channel(s)
 *
 * Input Parameters:
 *
 *   priv     - pointer to the adc device structure
 *   pdi_high - true:  The ADC is powered down when waiting for a start event
 *              false: The ADC is powered up when waiting for a start event
 *
 * Returned Value:
 *   None.
 *
 *******************************************************************************/

#ifdef CONFIG_STM32_STM32L15XX
void stm32_adcchange_sample_time(FAR struct adc_dev_s *dev,
                                 FAR struct adc_sample_time_s *time_samples)
{
  FAR struct stm32_dev_s *priv = (FAR struct stm32_dev_s *)dev->ad_priv;
  uint8_t ch_index;
  uint8_t i;

  /* Check if user wants to assign the same value for all channels
   * or just wants to change sample time values for certain channels */

  if (time_samples->all_same)
    {
      memset(priv->sample_rate, time_samples->all_ch_sample_time,
             ADC_CHANNELS_NUMBER);
    }
  else
    {
      for (i = 0; i < time_samples->channels_nbr; i++)
        {
          ch_index = time_samples->channel->channel;
          if (ch_index >= ADC_CHANNELS_NUMBER)
            {
              break;
            }

          priv->sample_rate[ch_index] = time_samples->channel->sample_time;
        }
    }
}
#endif

/****************************************************************************
 * Name: stm32_adcinitialize
 *
 * Description:
 *   Initialize the ADC.
 *
 *   The logic is, save nchannels : # of channels (conversions) in ADC_SQR1_L
 *   Then, take the chanlist array and store it in the SQR Regs,
 *     chanlist[0] -> ADC_SQR3_SQ1
 *     chanlist[1] -> ADC_SQR3_SQ2
 *     ...
 *     chanlist[15]-> ADC_SQR1_SQ16
 *
 *   up to
 *     chanlist[nchannels]
 *
 * Input Parameters:
 *   intf      - Could be {1,2,3} for ADC1, ADC2, or ADC3
 *   chanlist  - The list of channels
 *   cchannels - Number of channels
 *
 * Returned Value:
 *   Valid ADC device structure reference on succcess; a NULL on failure
 *
 ****************************************************************************/

struct adc_dev_s *stm32_adcinitialize(int intf, const uint8_t *chanlist,
                                      int cchannels)
{
  FAR struct adc_dev_s   *dev;
  FAR struct stm32_dev_s *priv;

  allvdbg("intf: %d cchannels: %d\n", intf, cchannels);

#ifdef CONFIG_STM32_ADC1
  if (intf == 1)
    {
      allvdbg("ADC1 Selected\n");
      dev = &g_adcdev1;
    }
  else
#endif
#ifdef CONFIG_STM32_ADC2
  if (intf == 2)
    {
      avdbg("ADC2 Selected\n");
      dev = &g_adcdev2;
    }
  else
#endif
#ifdef CONFIG_STM32_ADC3
  if (intf == 3)
    {
      avdbg("ADC3 Selected\n");
      dev = &g_adcdev3;
    }
  else
#endif
    {
      adbg("No ADC interface defined\n");
      return NULL;
    }

  /* Configure the selected ADC */

  priv = dev->ad_priv;

#if defined(CONFIG_STM32_STM32L15XX)

  /* Assign default values for the sample time table */

  memset(priv->sample_rate, ADC_DEFAULT_SAMPLE, ADC_CHANNELS_NUMBER);

#endif

#ifdef ADC_HAVE_DMA
  if (priv->hasdma)
    {
      DEBUGASSERT(priv->nchannels <= ADC_MAX_CHANNELS_DMA);
    }
  else
#endif
  {
    DEBUGASSERT(priv->nchannels <= ADC_MAX_CHANNELS_NODMA);
  }

  priv->cchannels = cchannels;

  memcpy(priv->chanlist, chanlist, cchannels);

  return dev;
}

#endif /* CONFIG_STM32_STM32F10XX || CONFIG_STM32_STM32F20XX || CONFIG_STM32_STM32F40XX */
#endif /* CONFIG_STM32_ADC || CONFIG_STM32_ADC2 || CONFIG_STM32_ADC3 */
#endif /* CONFIG_ADC */
