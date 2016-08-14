/****************************************************************************
 * arch/arm/src/kinetis/kinetis_i2c.c
 *
 *   Copyright (C) 2016 Gregory Nutt. All rights reserved.
 *   Author:  Matias v01d <phreakuencies@gmail.com>
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

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/wdog.h>
#include <nuttx/i2c/i2c_master.h>

#include <nuttx/irq.h>
#include <arch/board/board.h>

#include "chip.h"
#include "up_arch.h"
#include "up_internal.h"

#include "kinetis_config.h"
#include "chip.h"
#include "chip/kinetis_i2c.h"
#include "chip/kinetis_sim.h"
#include "chip/kinetis_pinmux.h"
#include "kinetis.h"
#include "kinetis_i2c.h"

#if defined(CONFIG_KINETIS_I2C0)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define I2C_TIMEOUT  (20*1000/CONFIG_USEC_PER_TICK)   /* 20 mS */

#define I2C_DEFAULT_FREQUENCY 400000

#define STATE_OK                0
#define STATE_ARBITRATION_ERROR 1
#define STATE_TIMEOUT           2
#define STATE_NAK               3

/* TODO:
 * - revisar tamanio de todos los registros (getreg/putreg)
 */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct kinetis_i2cdev_s
{
  struct i2c_master_s dev;    /* Generic I2C device */
  unsigned int base;          /* Base address of registers */
  uint16_t irqid;             /* IRQ for this device */
  uint32_t baseFreq;          /* branch frequency */

  sem_t mutex;                /* Only one thread can access at a time */
  sem_t wait;                 /* Place to wait for state machine completion */
  volatile uint8_t state;     /* State of state machine */
  WDOG_ID timeout;            /* watchdog to timeout when bus hung */
  uint32_t frequency;         /* Current I2C frequency */

  int restart;                /* Should next transfer restart or not */

  struct i2c_msg_s *msgs;     /* remaining transfers - first one is in
                               * progress */
  unsigned int nmsg;          /* number of transfer remaining */

  uint16_t wrcnt;             /* number of bytes sent to tx fifo */
  uint16_t rdcnt;             /* number of bytes read from rx fifo */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int  kinetis_i2c_start(struct kinetis_i2cdev_s *priv);
static void kinetis_i2c_stop(struct kinetis_i2cdev_s *priv);
static int  kinetis_i2c_interrupt(int irq, FAR void *context);
static void kinetis_i2c_timeout(int argc, uint32_t arg, ...);
static void kinetis_i2c_setfrequency(struct kinetis_i2cdev_s *priv,
                                     uint32_t frequency);
static int  kinetis_i2c_transfer(FAR struct i2c_master_s *dev,
                                 FAR struct i2c_msg_s *msgs, int count);
#ifdef CONFIG_I2C_RESET
static int  kinetis_i2c_reset(FAR struct i2c_master_s *dev);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct i2c_ops_s g_i2c_ops =
{
  .transfer = kinetis_i2c_transfer
#ifdef CONFIG_I2C_RESET
  ,.reset   = kinetis_i2c_reset
#endif
};

static struct kinetis_i2cdev_s g_i2c_dev;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kinetis_i2c_setfrequency
 *
 * Description:
 *   Set the frequency for the next transfer
 *
 ****************************************************************************/

static void kinetis_i2c_setfrequency(struct kinetis_i2cdev_s *priv,
                                     uint32_t frequency)
{
  if (frequency == priv->frequency)
    {
      return;
    }

  /* TODO: use apropriate definitions */

#if BOARD_BUS_FREQ == 120000000
  if (frequency < 400000)
    {
      putreg8(I2C_F_DIV1152, KINETIS_I2C0_F);   /* 104 kHz */
    }
  else if (frequency < 1000000)
    {
      putreg8(I2C_F_DIV288, KINETIS_I2C0_F);    /* 416 kHz */
    }
  else
    {
      putreg8(I2C_F_DIV128, KINETIS_I2C0_F);    /* 0.94 MHz */
    }

  putreg8(4, KINETIS_I2C0_FLT);
#elif BOARD_BUS_FREQ == 108000000
  if (frequency < 400000)
    {
      putreg8(I2C_F_DIV1024, KINETIS_I2C0_F);   /* 105 kHz */
    }
  else if (frequency < 1000000)
    {
      putreg8(I2C_F_DIV256, KINETIS_I2C0_F);    /* 422 kHz */
    }
  else
    {
      putreg8(I2C_F_DIV112, KINETIS_I2C0_F);    /* 0.96 MHz */
    }

  putreg8(4, KINETIS_I2C0_FLT);
#elif BOARD_BUS_FREQ == 96000000
  if (frequency < 400000)
    {
      putreg8(I2C_F_DIV960, KINETIS_I2C0_F);    /* 100 kHz */
    }
  else if (frequency < 1000000)
    {
      putreg8(I2C_F_DIV240, KINETIS_I2C0_F);    /* 400 kHz */
    }
  else
    {
      putreg8(I2C_F_DIV96, KINETIS_I2C0_F);     /* 1.0 MHz */
    }

  putreg8(4, KINETIS_I2C0_FLT);
#elif BOARD_BUS_FREQ == 90000000
  if (frequency < 400000)
    {
      putreg8(I2C_F_DIV896, KINETIS_I2C0_F);    /* 100 kHz */
    }
  else if (frequency < 1000000)
    {
      putreg8(I2C_F_DIV224, KINETIS_I2C0_F);    /* 402 kHz */
    }
  else
    {
      putreg8(I2C_F_DIV88, KINETIS_I2C0_F);     /* 1.02 MHz */
    }

  putreg8(4, KINETIS_I2C0_FLT);
#elif BOARD_BUS_FREQ == 80000000
  if (frequency < 400000)
    {
      putreg8(I2C_F_DIV768, KINETIS_I2C0_F);    /* 104 kHz */
    }
  else if (frequency < 1000000)
    {
      putreg8(I2C_F_DIV192, KINETIS_I2C0_F);    /* 416 kHz */
    }
  else
    {
      putreg8(I2C_F_DIV80, KINETIS_I2C0_F);     /* 1.0 MHz */
    }

  putreg8(4, KINETIS_I2C0_FLT);
#elif BOARD_BUS_FREQ == 72000000
  if (frequency < 400000)
    {
      putreg8(I2C_F_DIV640, KINETIS_I2C0_F);    /* 112 kHz */
    }
  else if (frequency < 1000000)
    {
      putreg8(I2C_F_DIV192, KINETIS_I2C0_F);    /* 375 kHz */
    }
  else
    {
      putreg8(I2C_F_DIV72, KINETIS_I2C0_F);     /* 1.0 MHz */
    }

  putreg8(4, KINETIS_I2C0_FLT);
#elif BOARD_BUS_FREQ == 64000000
  if (frequency < 400000)
    {
      putreg8(I2C_F_DIV640, KINETIS_I2C0_F);    /* 100 kHz */
    }
  else if (frequency < 1000000)
    {
      putreg8(I2C_F_DIV160, KINETIS_I2C0_F);    /* 400 kHz */
    }
  else
    {
      putreg8(I2C_F_DIV64, KINETIS_I2C0_F);     /* 1.0 MHz */
    }

  putreg8(4, KINETIS_I2C0_FLT);
#elif BOARD_BUS_FREQ == 60000000
  if (frequency < 400000)
    {
      putreg8(0x2C, KINETIS_I2C0_F);            /* 104 kHz */
    }
  else if (frequency < 1000000)
    {
      putreg8(0x1C, KINETIS_I2C0_F);            /* 416 kHz */
    }
  else
    {
      putreg8(0x12, KINETIS_I2C0_F);            /* 938 kHz */
    }

  putreg8(4, KINETIS_I2C0_FLT);
#elif BOARD_BUS_FREQ == 56000000
  if (frequency < 400000)
    {
      putreg8(0x2B, KINETIS_I2C0_F);            /* 109 kHz */
    }
  else if (frequency < 1000000)
    {
      putreg8(0x1C, KINETIS_I2C0_F);            /* 389 kHz */
    }
  else
    {
      putreg8(0x0E, KINETIS_I2C0_F);            /* 1 MHz */
    }

  putreg8(4, KINETIS_I2C0_FLT);
#elif BOARD_BUS_FREQ == 54000000
  if (frequency < 400000)
    {
      putreg8(I2C_F_DIV512, KINETIS_I2C0_F);    /* 105 kHz */
    }
  else if (frequency < 1000000)
    {
      putreg8(I2C_F_DIV128, KINETIS_I2C0_F);    /* 422 kHz */
    }
  else
    {
      putreg8(I2C_F_DIV56, KINETIS_I2C0_F);     /* 0.96 MHz */
    }

  putreg8(4, KINETIS_I2C0_FLT);
#elif BOARD_BUS_FREQ == 48000000
  if (frequency < 400000)
    {
      putreg8(0x27, KINETIS_I2C0_F);            /* 100 kHz */
    }
  else if (frequency < 1000000)
    {
      putreg8(0x1A, KINETIS_I2C0_F);            /* 400 kHz */
    }
  else
    {
      putreg8(0x0D, KINETIS_I2C0_F);            /* 1 MHz */
    }

  putreg8(4, KINETIS_I2C0_FLT);
#elif BOARD_BUS_FREQ == 40000000
  if (frequency < 400000)
    {
      putreg8(0x29, KINETIS_I2C0_F);            /* 104 kHz */
    }
  else if (frequency < 1000000)
    {
      putreg8(0x19, KINETIS_I2C0_F);            /* 416 kHz */
    }
  else
    {
      putreg8(0x0B, KINETIS_I2C0_F);            /* 1 MHz */
    }

  putreg8(3, KINETIS_I2C0_FLT);
#elif BOARD_BUS_FREQ == 36000000
  if (frequency < 400000)
    {
      putreg8(0x28, KINETIS_I2C0_F);            /* 113 kHz */
    }
  else if (frequency < 1000000)
    {
      putreg8(0x19, KINETIS_I2C0_F);            /* 375 kHz */
    }
  else
    {
      putreg8(0x0A, KINETIS_I2C0_F);            /* 1 MHz */
    }

  putreg8(3, KINETIS_I2C0_FLT);
#elif BOARD_BUS_FREQ == 24000000
  if (frequency < 400000)
    {
      putreg8(0x1F, KINETIS_I2C0_F);            /* 100 kHz */
    }
  else if (frequency < 1000000)
    {
      putreg8(0x12, KINETIS_I2C0_F);            /* 375 kHz */
    }
  else
    {
      putreg8(0x02, KINETIS_I2C0_F);            /* 1 MHz */
    }

  putreg8(2, KINETIS_I2C0_FLT);
#elif BOARD_BUS_FREQ == 16000000
  if (frequency < 400000)
    {
      putreg8(0x20, KINETIS_I2C0_F);            /* 100 kHz */
    }
  else if (frequency < 1000000)
    {
      putreg8(0x07, KINETIS_I2C0_F);            /* 400 kHz */
    }
  else
    {
      putreg8(0x00, KINETIS_I2C0_F);            /* 800 MHz */
    }

  putreg8(1, KINETIS_I2C0_FLT);
#elif BOARD_BUS_FREQ == 8000000
  if (frequency < 400000)
    {
      putreg8(0x14, KINETIS_I2C0_F);            /* 100 kHz */
    }
  else
    {
      putreg8(0x00, KINETIS_I2C0_F);            /* 400 kHz */
    }

  putreg8(1, KINETIS_I2C0_FLT);
#elif BOARD_BUS_FREQ == 4000000
  if (frequency < 400000)
    {
      putreg8(0x07, KINETIS_I2C0_F);            /* 100 kHz */
    }
  else
    {
      putreg8(0x00, KINETIS_I2C0_F);            /* 200 kHz */
    }

  putreg8(1, KINETIS_I2C0_FLT);
#elif BOARD_BUS_FREQ == 2000000
  putreg8(0x00, KINETIS_I2C0_F);                /* 100 kHz */
  putreg8(1, KINETIS_I2C0_FLT);
#else
#  error "F_BUS must be 120, 108, 96, 9, 80, 72, 64, 60, 56, 54, 48, 40, 36, 24, 16, 8, 4 or 2 MHz"
#endif

  priv->frequency = frequency;
}

/****************************************************************************
 * Name: kinetis_i2c_start
 *
 * Description:
 *   Initiate I2C transfer (START/RSTART + address)
 *
 ****************************************************************************/

static int kinetis_i2c_start(struct kinetis_i2cdev_s *priv)
{
  struct i2c_msg_s *msg;

  msg = priv->msgs;

  /* Now take control of the bus */

  if (getreg8(KINETIS_I2C0_C1) & I2C_C1_MST)
    {
      /* We are already the bus master, so send a repeated start */

      putreg8(I2C_C1_IICEN | I2C_C1_IICIE | I2C_C1_MST | I2C_C1_RSTA |
              I2C_C1_TX, KINETIS_I2C0_C1);
    }
  else
    {
      /* We are not currently the bus master, so wait for bus ready */

      while (getreg8(KINETIS_I2C0_S) & I2C_S_BUSY);

      /* Become the bus master in transmit mode (send start) */

      putreg8(I2C_C1_IICEN | I2C_C1_IICIE | I2C_C1_MST | I2C_C1_TX,
              KINETIS_I2C0_C1);
    }

  if (I2C_M_READ & msg->flags)  /* DEBUG: should happen always */
    {
      /* Wait until start condition establishes control of the bus */

      while (1)
        {
          if (getreg8(KINETIS_I2C0_S) & I2C_S_BUSY)
            {
              break;
            }
        }
    }

  /* Initiate actual transfer (send address) */

  putreg8((I2C_M_READ & msg->flags) == I2C_M_READ ?
          I2C_READADDR8(msg->addr) : I2C_WRITEADDR8(msg->addr),
          KINETIS_I2C0_D);

  return OK;
}

/****************************************************************************
 * Name: kinetis_i2c_stop
 *
 * Description:
 *   Perform a I2C transfer stop
 *
 ****************************************************************************/

static void kinetis_i2c_stop(struct kinetis_i2cdev_s *priv)
{
  putreg8(I2C_C1_IICEN | I2C_C1_IICIE, KINETIS_I2C0_C1);
  sem_post(&priv->wait);
}

/****************************************************************************
 * Name: kinetis_i2c_timeout
 *
 * Description:
 *   Watchdog timer for timeout of I2C operation
 *
 ****************************************************************************/

static void kinetis_i2c_timeout(int argc, uint32_t arg, ...)
{
  struct kinetis_i2cdev_s *priv = (struct kinetis_i2cdev_s *)arg;

  irqstate_t flags = enter_critical_section();
  priv->state = STATE_TIMEOUT;
  sem_post(&priv->wait);
  leave_critical_section(flags);
}

/****************************************************************************
 * Name: kinetis_i2c_nextmsg
 *
 * Description:
 *   Setup for the next message.
 *
 ****************************************************************************/

void kinetis_i2c_nextmsg(struct kinetis_i2cdev_s *priv)
{
  priv->nmsg--;

  if (priv->nmsg > 0)
    {
      priv->msgs++;
      priv->wrcnt = 0;
      priv->rdcnt = 0;

      if (priv->restart)
        {
          sem_post(&priv->wait);
        }
    }
  else
    {
      kinetis_i2c_stop(priv);
    }
}

/****************************************************************************
 * Name: kinetis_i2c_interrupt
 *
 * Description:
 *   The I2C Interrupt Handler
 *
 ****************************************************************************/

static int kinetis_i2c_interrupt(int irq, FAR void *context)
{
  struct kinetis_i2cdev_s *priv;
  struct i2c_msg_s *msg;
  uint32_t state;
  int regval;
  int dummy;
  UNUSED(dummy);

  if (irq == KINETIS_IRQ_I2C0)
    {
      priv = &g_i2c_dev;
    }
  else
    {
      PANIC();
    }

  /* Get current state */

  state = getreg8(KINETIS_I2C0_S);
  msg = priv->msgs;

  /* Arbitration lost */

  if (state & I2C_S_ARBL)
    {
      putreg8(I2C_S_IICIF | I2C_S_ARBL, KINETIS_I2C0_S);
      priv->state = STATE_ARBITRATION_ERROR;
      kinetis_i2c_stop(priv);
    }
  else
    {
      /* Clear interrupt */

      putreg8(I2C_S_IICIF, KINETIS_I2C0_S);
      regval = getreg8(KINETIS_I2C0_C1);

      /* TX mode */

      if (regval & I2C_C1_TX)
        {
          /* Last write was not acknowledged */

          if (state & I2C_S_RXAK)
            {
              priv->state = STATE_NAK;  /* Set error flag */
              kinetis_i2c_stop(priv);   /* Send STOP */
            }
          else
            {
              /* Actually intending to write */

              if ((I2C_M_READ & msg->flags) == 0)
                {
                  /* Wrote everything */

                  if (priv->wrcnt == msg->length)
                    {
                      /* Continue with next message */

                      kinetis_i2c_nextmsg(priv);

                      if (!priv->restart)
                        {
                          /* Initiate transfer of following message */

                          putreg8(priv->msgs->buffer[priv->wrcnt], KINETIS_I2C0_D);
                          priv->wrcnt++;

                          sem_post(&priv->wait);
                        }
                    }
                  else
                    {
                      /* Put next byte */

                      putreg8(msg->buffer[priv->wrcnt], KINETIS_I2C0_D); 
                      priv->wrcnt++;
                    }
                }

              /* Actually intending to read (address was just sent) */

              else
                {
                  if (msg->length == 1 && priv->restart) /* go to RX mode,
                                                            do not send ACK */
                    {
                      putreg8(I2C_C1_IICEN | I2C_C1_IICIE | I2C_C1_MST |
                              I2C_C1_TXAK, KINETIS_I2C0_C1);
                    }
                  else /* go to RX mode */
                    {
                      putreg8(I2C_C1_IICEN | I2C_C1_IICIE | I2C_C1_MST,
                              KINETIS_I2C0_C1);
                    }

                  /* TODO: handle zero-length reads */
                  /* Dummy read to initiate reception */

                  dummy = getreg8(KINETIS_I2C0_D); 
                }
            }
        }

      /* RX: mode */

      else
        {
          /* If last receiving byte */

          if (priv->rdcnt == (msg->length - 1))
            {
              if (priv->restart)
                {
                  /* go to TX mode before last read, otherwise a new read is
                   * triggered.
                   */

                  /* Go to TX mode */

                  putreg8(I2C_C1_IICEN | I2C_C1_IICIE | I2C_C1_MST | I2C_C1_TX,
                          KINETIS_I2C0_C1);
                }
              else if ((priv->msgs + 1)->length == 1)
                {
                  /* we will continue reception on next message.
                   * if next message is length == 1, this is actually the 2nd to
                   * last byte, so do not send ACK */

                  /* Do not ACK any more */

                  putreg8(I2C_C1_IICEN | I2C_C1_IICIE | I2C_C1_MST | I2C_C1_TXAK,
                          KINETIS_I2C0_C1);
                }

              msg->buffer[priv->rdcnt] = getreg8(KINETIS_I2C0_D);
              priv->rdcnt++;

              kinetis_i2c_nextmsg(priv);
            }

          /* Second to last receiving byte */

          else if (priv->rdcnt == (msg->length - 2))
            {
              if (priv->restart)
                {
                  /* Do not ACK any more */

                  putreg8(I2C_C1_IICEN | I2C_C1_IICIE | I2C_C1_MST | I2C_C1_TXAK,
                          KINETIS_I2C0_C1);
                }

              msg->buffer[priv->rdcnt] = getreg8(KINETIS_I2C0_D);
              priv->rdcnt++;
            }
          else
            {
              msg->buffer[priv->rdcnt] = getreg8(KINETIS_I2C0_D);
              priv->rdcnt++;
            }
        }
    }

  return OK;
}

/****************************************************************************
 * Name: kinetis_i2c_transfer
 *
 * Description:
 *   Perform a sequence of I2C transfers
 *
 ****************************************************************************/

static int kinetis_i2c_transfer(FAR struct i2c_master_s *dev,
                                FAR struct i2c_msg_s *msgs, int count)
{
  struct kinetis_i2cdev_s *priv = (struct kinetis_i2cdev_s *)dev;
  int msg_n;

  DEBUGASSERT(dev != NULL);

  /* Get exclusive access to the I2C bus */

  sem_wait(&priv->mutex);

  /* Set up for the transfer */

  msg_n = 0;
  priv->msgs = msgs;
  priv->nmsg = count;
  priv->state = STATE_OK;
  priv->wrcnt = 0;
  priv->rdcnt = 0;

  /* Configure the I2C frequency. REVISIT: Note that the frequency is set
   * only on the first message. This could be extended to support
   * different transfer frequencies for each message segment.
   */

  kinetis_i2c_setfrequency(priv, msgs->frequency);

  /* Clear the status flags */

  putreg8(I2C_S_IICIF | I2C_S_ARBL, KINETIS_I2C0_S);

  /* Process every message */

  while (priv->nmsg && priv->state == STATE_OK)
    {
      priv->restart = 1;

      /* process NORESTART flag */

      if (priv->nmsg > 1)
        {
          struct i2c_msg_s* nextmsg = (priv->msgs + 1);

          /* if there is a following message with "norestart" flag of
           * the same type as the current one, we can avoid the restart
           */

          if ((nextmsg->flags & I2C_M_NORESTART) &&
              nextmsg->addr == priv->msgs->addr &&
              nextmsg->frequency == priv->msgs->frequency &&
              (nextmsg->flags & I2C_M_READ) == (priv->msgs->flags & I2C_M_READ))
            {
              /* "no restart" can be performed */

              priv->restart = 0;
            }
        }

      /* only send start when required (we are trusting the flags setting to be correctly used here) */

      if (!(priv->msgs->flags & I2C_M_NORESTART))
        {
          /* Initiate the transfer, in case restart is required */

          kinetis_i2c_start(priv);
        }

      /* Wait for transfer complete */

      wd_start(priv->timeout, I2C_TIMEOUT, kinetis_i2c_timeout, 1,
               (uint32_t) priv);
      sem_wait(&priv->wait);

      wd_cancel(priv->timeout);

      msg_n++;
    }

  /* Disable interrupts */

  putreg8(I2C_C1_IICEN, KINETIS_I2C0_C1);

  /* Release access to I2C bus */

  sem_post(&priv->mutex);

  if (priv->state != STATE_OK)
    {
      return -EIO;
    }
  else
    {
      return 0;
    }
}

/************************************************************************************
 * Name: kinetis_i2c_reset
 *
 * Description:
 *   Perform an I2C bus reset in an attempt to break loose stuck I2C devices.
 *
 * Input Parameters:
 *   dev   - Device-specific state data
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ************************************************************************************/

#ifdef CONFIG_I2C_RESET
static int kinetis_i2c_reset(FAR struct i2c_master_s *dev)
{
  return OK;
}
#endif  /* CONFIG_I2C_RESET */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kinetis_i2cbus_initialize
 *
 * Description:
 *   Initialise an I2C device
 *
 ****************************************************************************/

struct i2c_master_s *kinetis_i2cbus_initialize(int port)
{
  struct kinetis_i2cdev_s *priv;

  if (port > 1)
    {
      i2cerr("ERROR: Kinetis I2C Only suppors ports 0 and 1\n");
      return NULL;
    }

  irqstate_t flags;
  uint32_t regval;

  flags = enter_critical_section();

  if (port == 0)
    {
      priv = &g_i2c_dev;
      priv->base = KINETIS_I2C0_BASE;
      priv->irqid = KINETIS_IRQ_I2C0;
      priv->baseFreq = BOARD_BUS_FREQ;

      /* Enable clock */

      regval = getreg32(KINETIS_SIM_SCGC4);
      regval |= SIM_SCGC4_I2C0;
      putreg32(regval, KINETIS_SIM_SCGC4);

      kinetis_i2c_setfrequency(priv, I2C_DEFAULT_FREQUENCY);

      /* Disable while configuring */

      putreg8(0, KINETIS_I2C0_C1);

      /* Configure pins */

      kinetis_pinconfig(PIN_I2C0_SCL);
      kinetis_pinconfig(PIN_I2C0_SDA);

      /* Enable */

      putreg8(I2C_C1_IICEN, KINETIS_I2C0_C1);

      /* High-drive select (TODO: why)? */

      regval = getreg8(KINETIS_I2C0_C2);
      regval |= I2C_C2_HDRS;
      putreg8(regval, KINETIS_I2C0_C2);
    }
  else
    {
      return NULL;
    }

  leave_critical_section(flags);

  sem_init(&priv->mutex, 0, 1);
  sem_init(&priv->wait, 0, 0);

  /* Allocate a watchdog timer */

  priv->timeout = wd_create();
  DEBUGASSERT(priv->timeout != 0);

  /* Attach Interrupt Handler */

  irq_attach(priv->irqid, kinetis_i2c_interrupt);

  /* Enable Interrupt Handler */

  up_enable_irq(priv->irqid);

  /* Install our operations */

  priv->dev.ops = &g_i2c_ops;
  return &priv->dev;
}

/****************************************************************************
 * Name: kinetis_i2cbus_uninitialize
 *
 * Description:
 *   Uninitialise an I2C device
 *
 ****************************************************************************/

int kinetis_i2cbus_uninitialize(FAR struct i2c_master_s *dev)
{
  struct kinetis_i2cdev_s *priv = (struct kinetis_i2cdev_s *)dev;

  putreg8(0, KINETIS_I2C0_C1);

  up_disable_irq(priv->irqid);
  irq_detach(priv->irqid);
  return OK;
}

#endif /* CONFIG_KINETIS_I2C0 */
