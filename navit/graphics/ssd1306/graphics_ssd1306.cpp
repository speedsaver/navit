/**
 * Navit, a modular navigation system.
 * Copyright (C) 2005-2018 Navit Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */
// style with: clang-format -style=WebKit -i *

#include <glib.h>
#include <stdlib.h>

extern "C" {
#include "config.h"
#include "item.h"		/* needs to be first, as attr.h depends on it */

#include "callback.h"
#include "debug.h"
#include "event.h"

#include "point.h"		/* needs to be before graphics.h */
#include "coord.h"

#include "graphics.h"
#include "plugin.h"
#include "navit.h"
}
#include "xmlconfig.h"
#include "vehicle.h"
#include "transform.h"
#include "track.h"
#include "vehicleprofile.h"
#include "roadprofile.h"
#include <sys/sysinfo.h>
#include "ArduiPi_OLED_lib.h"
#include "Adafruit_GFX.h"
#include "ArduiPi_OLED.h"
#include "graphics_init_animation.h"

const size_t init_animation_frames = 3;
const size_t init_animation_images = 3;
const size_t init_animation_count = init_animation_frames * init_animation_images;
const int refresh_rate_ms = 100;

ArduiPi_OLED display;
simple_bm *init_animation[init_animation_count];
const char* tone_cmd = "true";
extern char *version;

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32


struct graphics_priv {
	struct navit *nav;
	int frames;
	long tick;
	int fps;
	int width;
	int height;
	int imperial = 0;
	int debug = 0;
	long tone_next = 0;
	enum draw_mode_num mode;
	struct callback_list *cbl;
};

void
show_start_animation(struct graphics_priv *ssd1306, long current_tick)
{
	static int xpos[3] = { 0, 6*8, 11*8 };
	long step = current_tick % init_animation_frames;
#ifdef SIMPLE_BM_DEBUG
	simple_bm *bm = init_animation[0];
	display.drawBitmap(xpos[0],0,bm->get_bm(),bm->get_width(),bm->get_height(),WHITE);
#else
	for(size_t i = 0 ; i < init_animation_images ; i++ ) {
		simple_bm *bm = init_animation[step*init_animation_frames+i];
		display.drawBitmap(xpos[i],0,bm->get_bm(),bm->get_width(),bm->get_height(),WHITE);
	}
#endif
}

long
get_uptime()
{
	struct sysinfo s_info;
	int error = sysinfo(&s_info);
	if (error != 0) {
		printf("code error = %d\n", error);
	}
	return s_info.uptime;
}

static gboolean
graphics_ssd1306_idle(void *data)
{
	dbg(lvl_info, "idle\n");

	struct graphics_priv *ssd1306 = (struct graphics_priv *) data;

	display.clearDisplay();
	display.setTextSize(1);
	display.setTextColor(WHITE);
	display.setCursor(0, 0);

	char snum[32];

	struct attr attr, attr2, vattr;
	struct attr position_attr, position_fix_attr;
	struct attr_iter *iter;
	enum projection pro;
	struct coord c1;
	long current_tick = get_uptime();

	struct attr speed_attr;
	double speed = -1;
	int strength = -1;

	iter = navit_attr_iter_new();
	if (navit_get_attr(ssd1306->nav, attr_vehicle, &attr, iter) &&
	   !navit_get_attr(ssd1306->nav, attr_vehicle, &attr2, iter)) {
		vehicle_get_attr(attr.u.vehicle, attr_name, &vattr, NULL);
		navit_attr_iter_destroy(iter);

		if (vehicle_get_attr(attr.u.vehicle, attr_position_fix_type, &position_fix_attr, NULL)) {
			switch (position_fix_attr.u.num) {
			case 1:
			case 2:
				strength = 2;
				if (vehicle_get_attr(attr.u.vehicle, attr_position_sats_used, &position_fix_attr, NULL)) {
					if (position_fix_attr.u.num >= 3)
						strength = position_fix_attr.u.num - 1;
					if (strength > 5)
						strength = 5;
					if (strength > 3) {
						if (vehicle_get_attr(attr.u.vehicle, attr_position_hdop, &position_fix_attr, NULL)) {
							if (*position_fix_attr.u.numd > 2.0 && strength > 4)
								strength = 4;
							if (*position_fix_attr.u.numd > 4.0 && strength > 3)
								strength = 3;
						}
					}
				}
				break;
			default:
				strength = -1;
			}
		}
		if (ssd1306->debug) {
			display.drawLine(0, display.height() - 1, strength * 5, display.height() - 1, WHITE);
		}
		if (strength > -1) {
			if (vehicle_get_attr(attr.u.vehicle, attr_position_coord_geo, &position_attr, NULL)) {
				pro = position_attr.u.pcoord->pro;
				transform_from_geo(pro, position_attr.u.coord_geo, &c1);
				dbg(lvl_debug, "%f %f\n", position_attr.u.coord_geo->lat, position_attr.u.coord_geo->lng);
				sprintf(snum, "%f %f\n", position_attr.u.coord_geo->lat, position_attr.u.coord_geo->lng);
				if (ssd1306->debug) {
					display.printf(snum);
				}
				vehicle_get_attr(attr.u.vehicle, attr_position_speed, &speed_attr, NULL);
				speed = *speed_attr.u.numd / ( ssd1306->imperial ? 1.609344 : 1 );
				dbg(lvl_debug, "speed : %0.0f (%f)\n", speed, speed);
			} else {
				dbg(lvl_error, "vehicle_get_attr failed\n");
			}

			double routespeed = -1;
			int speeding = 0;
			int *flags;
			struct attr maxspeed_attr;
			struct tracking *tracking = navit_get_tracking(ssd1306->nav);

			if (getenv("GOTTA_GO_FAST")) {
				routespeed = 50;
				speed = 88;
			}

			if (tracking) {
				flags = tracking_get_current_flags(tracking);
				if (flags
				    && (*flags & AF_SPEED_LIMIT)
				    && tracking_get_attr(tracking, attr_maxspeed, &maxspeed_attr, NULL)) {
					routespeed = maxspeed_attr.u.num / ( ssd1306->imperial ? 1.609344 : 1 );
				}
				speeding = routespeed != -1 && (speed > routespeed + 1);
				if (speeding && current_tick >= ssd1306->tone_next) {
					system(tone_cmd);
					ssd1306->tone_next = current_tick + 2;
				}
				if ( current_tick % 10 ) {
					sprintf(snum, "%3.0f", speed);
					display.setTextSize(3);
					display.setCursor(1, 6);
					if (routespeed == -1) {
						display.printf(snum);
						display.setTextColor(BLACK, WHITE);
						display.setCursor(60, 6);
						display.printf("???");
						display.setTextColor(WHITE, BLACK);
					} else {
						dbg(lvl_debug, "route speed : %0.0f\n", routespeed);
						display.drawRect(62, 2, 62, display.height() - 4, WHITE);
						display.setCursor(66, 6);
						sprintf(snum, "%3.0f", routespeed);
						display.printf(snum);
						display.setCursor(1, 6);
						sprintf(snum, "%3.0f", speed);
						if (speeding
						    && current_tick % 2) {
							display.setTextColor(BLACK, WHITE);	// 'inverted' text
							display.printf(snum);
						} else {
							display.setTextColor(WHITE, BLACK);
							display.printf(snum);
						}
					}
				} else {
					display.setTextSize(3);
					display.setCursor(1, 6);
					display.printf(ssd1306->imperial ? "MPH" : "KM/H");
				}
			}
			if (ssd1306->debug) {
				display.drawLine(display.width() - 1 - ssd1306->fps, display.height() - 1, display.width() - 1, display.height() - 1, WHITE);
			}

			if (current_tick == ssd1306->tick) {
				ssd1306->frames++;
			} else {
				ssd1306->fps = ssd1306->frames;
				ssd1306->frames = 0;
				ssd1306->tick = current_tick;
			}
		} else {
			show_start_animation(ssd1306, current_tick);
		}
		display.display();
	}
	g_timeout_add(refresh_rate_ms, graphics_ssd1306_idle, data);
	return G_SOURCE_REMOVE;
}


static struct graphics_methods graphics_methods = {
	NULL,			//graphics_destroy,
	NULL,			//draw_mode,
	NULL,			//draw_lines,
	NULL,			//draw_polygon,
	NULL,			//draw_rectangle,
	NULL,
	NULL,			//draw_text,
	NULL,			//draw_image,
	NULL,
	NULL,			//draw_drag,
	NULL,
	NULL,			//gc_new,
	NULL,			//background_gc,
	NULL,			//overlay_new,
	NULL,			//image_new,
	NULL,			//get_data,
	NULL,			//image_free,
	NULL,
	NULL,			//overlay_disable,
	NULL,			//overlay_resize,
	NULL,			/* set_attr, */
	NULL,			/* show_native_keyboard */
	NULL,			/* hide_native_keyboard */
};

static struct graphics_priv *
graphics_ssd1306_new(struct navit *nav, struct graphics_methods *meth,
		     struct attr **attrs, struct callback_list *cbl)
{
	struct attr *attr, imperial_attr;
	if (!event_request_system("glib", "graphics_ssd1306_new"))
		return NULL;
	struct graphics_priv *this_ = g_new0(struct graphics_priv, 1);
	*meth = graphics_methods;

	this_->cbl = cbl;

	this_->width = SCREEN_WIDTH;
	if ((attr = attr_search(attrs, NULL, attr_w)))
		this_->width = attr->u.num;
	this_->height = SCREEN_HEIGHT;
	if ((attr = attr_search(attrs, NULL, attr_h)))
		this_->height = attr->u.num;
	if (nav) {
		if (navit_get_attr
		    (nav, attr_imperial, &imperial_attr, NULL)) {
			this_->imperial = imperial_attr.u.num;
		}
	}
	generate_init_animations(init_animation,init_animation_count);

	if (!display.init(OLED_I2C_RESET, 2))
		exit(-1);

	display.begin();
	display.clearDisplay();
	display.display();

	this_->nav = nav;
	this_->frames = 0;
	this_->fps = 0;
	this_->tick = get_uptime();

	graphics_ssd1306_idle(this_);

	char *bug = getenv("SSD1306_DEBUG_LEVEL");
	if ( bug )
		debug_level_set(dbg_module,(dbg_level)(*bug-'0'));

	dbg(lvl_info, "initialized\n");
	return this_;
}

void
plugin_init(void)
{
	tone_cmd = g_strdup_printf("aplay \"%s/tone7.wav\" 2>/dev/null >/dev/null&", getenv("NAVIT_SHAREDIR"));
	plugin_register_category_graphics("ssd1306", graphics_ssd1306_new);
}
