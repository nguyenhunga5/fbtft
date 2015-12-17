/*
 * FB driver for the ILI9488 LCD display controller
 *
 * Copyright (C) 2013 Christian Vogelgsang
 * Based on adafruit22fb.c by Noralf Tronnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/gpio.h>

#include "fbtft.h"

#define DRVNAME		"fb_ili9488"
#define WIDTH		420
#define HEIGHT		320

/*
 * This macros and related parts are taken from:
 * https://github.com/notro/fbtft/pull/314/files#diff-ac30e285b59d7a6eff5b06b7f256dcd7R12
 */
#define BYPASS_GPIOLIB /* Speed up gpio access by directly writing to io address */

#ifdef BYPASS_GPIOLIB
#define GPIOSET(no, ishigh)           \
do {                                  \
   if (ishigh)                   \
       set |= (1 << (no));   \
   else                          \
       reset |= (1 << (no)); \
} while (0)
#endif

/* this init sequence matches PiScreen */
static int default_init_sequence[] = {
    -1,0xb0,0x0,
    -1,0x11,
    -2,120,
    -1,0x3A,0x55,
    -1,0xC2,0x33,
    -1,0xC5,0x00,0x1E,0x80,
    -1,0x36,0x28,
    -1,0xB1,0xB0,
    -1,0xE0,0x00,0x04,0x0E,0x08,0x17,0x0A,0x40,0x79,0x4D,0x07,0x0E,0x0A,0x1A,0x1D,0x0F,
    -1,0xE1,0x00,0x1B,0x1F,0x02,0x10,0x05,0x32,0x34,0x43,0x02,0x0A,0x09,0x33,0x37,0x0F,
    -1,0x11,
    -1,0x29,
    -3
};

static void set_addr_win(struct fbtft_par *par, int xs, int ys, int xe, int ye)
{
	fbtft_par_dbg(DEBUG_SET_ADDR_WIN, par,
		"%s(xs=%d, ys=%d, xe=%d, ye=%d)\n", __func__, xs, ys, xe, ye);

	/* Column address set */
	write_reg(par, 0x2A,
		(xs >> 8) & 0xFF, xs & 0xFF, (xe >> 8) & 0xFF, xe & 0xFF);

	/* Row adress set */
	write_reg(par, 0x2B,
		(ys >> 8) & 0xFF, ys & 0xFF, (ye >> 8) & 0xFF, ye & 0xFF);

	/* Memory write */
	write_reg(par, 0x2C);
}

#define MY  (1<<7)   /* vertical mirrorring */
#define MX  (1<<6)   /* horizontal mirrorring */
#define MV  (1<<5)   /* swap column and rows (90deg rotate) */
static int set_var(struct fbtft_par *par)
{
	fbtft_par_dbg(DEBUG_INIT_DISPLAY, par, "%s()\n", __func__);

	switch (par->info->var.rotate) {
	case 0:
		write_reg(par, 0x36, MX | MY | (par->bgr << 3));
		break;
	case 90:
		write_reg(par, 0x36, MV | (par->bgr << 3));
		break;
	case 180:
		write_reg(par, 0x36, (par->bgr << 3));
		break;
	case 270:
		write_reg(par, 0x36, MY | MV | (par->bgr << 3));
		break;
	default:
		break;
	}

	return 0;
}

/*
 * Option to use direct gpio access to speed up display refresh
 *
 */
static int fbtft_ili9488_write_gpio8_wr(struct fbtft_par *par, void *buf, size_t len)
{
   u8 data;
   static u8 prev_data = 0xff;
#ifdef BYPASS_GPIOLIB
   unsigned int set = 0;
   unsigned int reset = 0;
#else
   int i;
#endif

   fbtft_par_dbg_hex(DEBUG_WRITE, par, par->info->device, u8, buf, len,
       "%s-OPTIMIZED(len=%d): ", __func__, len);
#ifdef BYPASS_GPIOLIB
   while (len--) {
       data = *(u8 *) buf;

       if (data != prev_data)
       {
           /* Set data */
           GPIOSET(par->gpio.db[0], (data&0x01));
           GPIOSET(par->gpio.db[1], (data&0x02));
           GPIOSET(par->gpio.db[2], (data&0x04));
           GPIOSET(par->gpio.db[3], (data&0x08));
           GPIOSET(par->gpio.db[4], (data&0x10));
           GPIOSET(par->gpio.db[5], (data&0x20));
           GPIOSET(par->gpio.db[6], (data&0x40));
           GPIOSET(par->gpio.db[7], (data&0x80));
           writel(set, __io_address(GPIO_BASE+0x1C));
           writel(reset, __io_address(GPIO_BASE+0x28));
       }

       /* Pulse /WR low */
       writel((1<<par->gpio.wr),  __io_address(GPIO_BASE+0x28));
       writel(0,  __io_address(GPIO_BASE+0x28)); /* used as a delay */
       writel((1<<par->gpio.wr),  __io_address(GPIO_BASE+0x1C));

       set = 0;
       reset = 0;
       prev_data = data;
       buf++;

   }
#else
   while (len--) {
       data = *(u8 *) buf;

       /* Start writing by pulling down /WR */
       gpio_set_value(par->gpio.wr, 0);

       /* Set data */
#ifndef DO_NOT_OPTIMIZE_FBTFT_WRITE_GPIO
       if (data == prev_data) {
           gpio_set_value(par->gpio.wr, 0); /* used as delay */
       } else {
           for (i = 0; i < 8; i++) {
               if ((data & 1) != (prev_data & 1))
                   gpio_set_value(par->gpio.db[i],
                               data & 1);
               data >>= 1;
               prev_data >>= 1;
           }
       }
#else
       for (i = 0; i < 8; i++) {
           gpio_set_value(par->gpio.db[i], data & 1);
           data >>= 1;
       }
#endif

       /* Pullup /WR */
       gpio_set_value(par->gpio.wr, 1);

#ifndef DO_NOT_OPTIMIZE_FBTFT_WRITE_GPIO
       prev_data = *(u8 *) buf;
#endif
       buf++;
   }
#endif

   return 0;
}

static struct fbtft_display display = {
	.regwidth = 8,
	.width = WIDTH,
	.height = HEIGHT,
    .init_sequence = default_init_sequence,
	.fbtftops = {
		//.init_display = init_display,
		.set_addr_win = set_addr_win,
		.set_var = set_var,
        .write = fbtft_ili9488_write_gpio8_wr,
	},
};
FBTFT_REGISTER_DRIVER(DRVNAME, "ilitek,ili9488", &display);

MODULE_ALIAS("spi:" DRVNAME);
MODULE_ALIAS("platform:" DRVNAME);
MODULE_ALIAS("spi:ili9488");
MODULE_ALIAS("platform:ili9488");

MODULE_DESCRIPTION("FB driver for the ili9488 LCD display controller base on https://github.com/ont/fbtft");
MODULE_AUTHOR("nguyenhunga5 <nguyenthanhhung19872004@gmail.com@gmail.com>");
MODULE_LICENSE("GPL");
