/* EPDSign  app */
/* Copyright ©2019 - 2023 Adrian Kennard, Andrews & Arnold Ltd.See LICENCE file for details .GPL 3.0 */

static const char TAG[] = "EPDSign";

#include "revk.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_crt_bundle.h"
#include "gfx.h"
#include "iec18004.h"
#include <hal/spi_types.h>
#include <driver/gpio.h>

#define	MAXGPIO	36
#define BITFIELDS "-^"
#define PORT_INV 0x4000
#define PORT_PU 0x2000
#define port_mask(p) ((p)&0xFF) // 16 bit

// Dynamic

#define	settings		\
	io(gfxena,)	\
        io(btn2,-8)     \
        io(btn1,-2)     \
        io(gfxmosi,37)  \
        io(gfxsck,38)   \
        io(gfxcs,39)    \
        io(gfxdc,40)    \
        io(gfxrst,41)   \
        io(gfxbusy,42)  \
        io(rgb,36)      \
        io(relay,36)    \
	u8(leds,24)	\
        u8(gfxflip,6)   \
	u8(holdtime,30)	\
	u32(refresh,3600)	\
	b(gfxinvert)	\
	s(imageurl,)	\
	s(imageidle,Example)	\
	s(imagexmas,)	\
	s(imageyear,)	\
	s(imagehall,)	\
	s(imagewait,G:Wait)	\
	s(imagebusy,Y:Busy)	\
	s(imageaway,R:Away)	\
	s(postcode,)	\
	s(toot,)		\
	s(tasbell,)	\
	s(tasaway,)	\
	s(tasbusy,)	\

#define u32(n,d)        uint32_t n;
#define s8(n,d) int8_t n;
#define u8(n,d) uint8_t n;
#define b(n) uint8_t n;
#define s(n,d) char * n;
#define io(n,d)           uint16_t n;
settings
#undef io
#undef u32
#undef s8
#undef u8
#undef b
#undef s
   httpd_handle_t webserver = NULL;
uint32_t pushed = 0;
uint32_t override = 0;
uint32_t last = -1;
char activename[30] = "";
char overridename[30] = "";
uint8_t *idle = NULL;
uint8_t *active = NULL;
SemaphoreHandle_t mutex = NULL;
char mqttinit = 0;
char tasawaystate = 0;
char tasbusystate = 0;
led_strip_handle_t strip = NULL;

const char *
getidle (struct tm *t)
{
   const char *basename = imageidle;    // The idle name, seasonally adjusted
   if (*imagexmas && t->tm_mon == 11 && t->tm_mday <= 25)
      basename = imagexmas;
   if (*imageyear && t->tm_mon == 0 && t->tm_mday <= 7)
      basename = imageyear;
   if (*imagehall && t->tm_mon == 9 && t->tm_mday == 31 && t->tm_hour >= 16)
      basename = imagehall;
   return basename;
}

const char *
skipcolour (const char *n)
{
   if (n && *n && n[1] == ':')
      n += 2;
   return n;
}

uint8_t *
getimage (const char *name, uint8_t * prev)
{
   if (!*imageurl || !name || !*name || revk_link_down ())
      return prev;
   name = skipcolour (name);
   char *url;
   asprintf (&url, "%s/%s.mono", imageurl, name);
   if (!url)
      return prev;
   ESP_LOGD (TAG, "Get %s", url);
   const int size = gfx_width () * gfx_height () / 8;
   int len = 0;
   uint8_t *buf = NULL;
   esp_http_client_config_t config = {
      .url = url,
      .crt_bundle_attach = esp_crt_bundle_attach,
   };
   esp_http_client_handle_t client = esp_http_client_init (&config);
   if (client)
   {
      if (!esp_http_client_open (client, 0))
      {
         if (esp_http_client_fetch_headers (client) == size)
         {
            buf = mallocspi (size);
            if (buf)
               len = esp_http_client_read_response (client, (char *) buf, size);
         }
         esp_http_client_close (client);
      }
      esp_http_client_cleanup (client);
   }
   if (len != size)
   {
      jo_t j = jo_object_alloc ();
      jo_string (j, "name", name);
      jo_string (j, "url", url);
      if (len)
      {
         jo_int (j, "len", len);
         jo_int (j, "expect", size);
      }
      revk_error ("image", &j);
      free (url);
      free (buf);
      return prev;
   }
   if (gfxinvert)
      for (int i = 0; i < size; i++)
         buf[i] ^= 0xFF;
   if (!prev || memcmp (prev, buf, len))
   {                            // New image
      jo_t j = jo_object_alloc ();
      jo_string (j, "name", name);
      jo_string (j, "url", url);
      jo_int (j, "len", len);
      revk_info ("image", &j);
   }
   free (url);
   free (prev);
   return buf;
}

void
image_load (const char *name, const uint8_t * image, char c)
{                               // Load image and set LEDs (image can be prefixed with colour, else default is used)
   if (name && *name && name[1] == ':')
      c = *name;
   if (strip)
   {                            // This is limited power mix
      uint8_t r = (c == 'R' ? 0xFF : c == 'M' || c == 'Y' ? 0x80 : c == 'W' ? 0x55 : 0);
      uint8_t g = (c == 'G' ? 0xFF : c == 'C' || c == 'Y' ? 0x80 : c == 'W' ? 0x55 : 0);
      uint8_t b = (c == 'B' ? 0xFF : c == 'C' || c == 'M' ? 0x80 : c == 'W' ? 0x55 : 0);
      for (int i = 0; i < leds; i++)
         led_strip_set_pixel (strip, i, r, g, b);
      led_strip_refresh (strip);
   }
   if (image)
      gfx_load (image);
}

void
setactive (char *value)
{
   if (!value || !strcmp (activename, value))
      return;
   xSemaphoreTake (mutex, portMAX_DELAY);
   strncpy (activename, value, sizeof (activename));
   free (active);
   active = NULL;
   if (!last)
      last = -1;                // Redisplay
   if (pushed)
      pushed = uptime ();
   xSemaphoreGive (mutex);
}

static void
web_head (httpd_req_t * req, const char *title)
{
   revk_web_head (req, title);
   httpd_resp_sendstr_chunk (req, "<style>"     //
                             "body{font-family:sans-serif;background:#8cf;}"    //
                             "</style><body><h1>");
   if (title)
      httpd_resp_sendstr_chunk (req, title);
   httpd_resp_sendstr_chunk (req, "</h1>");
}

static esp_err_t
web_icon (httpd_req_t * req)
{                               // serve image -  maybe make more generic file serve
   extern const char start[] asm ("_binary_apple_touch_icon_png_start");
   extern const char end[] asm ("_binary_apple_touch_icon_png_end");
   httpd_resp_set_type (req, "image/png");
   httpd_resp_send (req, start, end - start);
   return ESP_OK;
}

static esp_err_t
web_root (httpd_req_t * req)
{
   if (revk_link_down ())
      return revk_web_settings (req);   // Direct to web set up
   web_head (req, *hostname ? hostname : appname);
   if (*imageurl)
   {
      void i (const char *tag, const char *name)
      {
         name = skipcolour (name);
         httpd_resp_sendstr_chunk (req,
                                   "<figure style='float:right;background:white;border:10px solid white;border-left:20px solid white;margin:5px;");
         if (gfxinvert)
            httpd_resp_sendstr_chunk (req, ";filter:invert(1)");
         httpd_resp_sendstr_chunk (req, "'><img wdth=240 height=400 src='");
         httpd_resp_sendstr_chunk (req, imageurl);
         httpd_resp_sendstr_chunk (req, "/");
         httpd_resp_sendstr_chunk (req, name);
         httpd_resp_sendstr_chunk (req, ".png'><figcaption>");
         httpd_resp_sendstr_chunk (req, tag);
         httpd_resp_sendstr_chunk (req, "</figcaption></figure>");
      }
      time_t now = time (0);
      struct tm t;
      localtime_r (&now, &t);
      i ("Active", activename);
      i ("Idle", getidle (&t));
   }
   httpd_resp_sendstr_chunk (req, "<p><a href=/push>Ding!</a></p>");
   return revk_web_foot (req, 0, 1,NULL);
}

static esp_err_t
web_push (httpd_req_t * req)
{
   size_t l = httpd_req_get_url_query_len (req);
   char query[200];
   if (!*overridename && l > 0 && l < sizeof (query) && !httpd_req_get_url_query_str (req, query, sizeof (query)))
      strncpy (overridename, query, sizeof (overridename));
   else
      pushed = uptime ();
   return web_root (req);
}

static esp_err_t
web_active (httpd_req_t * req)
{
   size_t l = httpd_req_get_url_query_len (req);
   char query[200];
   if (l > 0 && l < sizeof (query) && !httpd_req_get_url_query_str (req, query, sizeof (query)))
   {
      char *q = query;
      if (*q == '?')
         q++;
      setactive (q);
   }
   return web_root (req);
}

static esp_err_t
web_message (httpd_req_t * req)
{
   size_t l = httpd_req_get_url_query_len (req);
   char query[200];
   if (l > 0 && l < sizeof (query) && !httpd_req_get_url_query_str (req, query, sizeof (query)))
   {
      char *q = query;
      if (*q == '?')
         q++;
      xSemaphoreTake (mutex, portMAX_DELAY);
      override = uptime ();
      gfx_message (q);
      xSemaphoreGive (mutex);
   }
   return web_root (req);
}

static void
register_uri (const httpd_uri_t * uri_struct)
{
   esp_err_t res = httpd_register_uri_handler (webserver, uri_struct);
   if (res != ESP_OK)
   {
      ESP_LOGE (TAG, "Failed to register %s, error code %d", uri_struct->uri, res);
   }
}

static void
register_get_uri (const char *uri, esp_err_t (*handler) (httpd_req_t * r))
{
   httpd_uri_t uri_struct = {
      .uri = uri,
      .method = HTTP_GET,
      .handler = handler,
   };
   register_uri (&uri_struct);
}

const char *
gfx_qr (const char *value, int s)
{
#ifndef	CONFIG_GFX_NONE
   unsigned int width = 0;
 uint8_t *qr = qr_encode (strlen (value), value, widthp: &width, noquiet:1);
   if (!qr)
      return "Failed to encode";
   int w = gfx_width ();
   int h = gfx_height ();
   if (!width || width > w || width > h)
   {
      free (qr);
      return "Too wide";
   }
   ESP_LOGD (TAG, "QR %d/%d %d", w, h, s);
   gfx_pos_t ox,
     oy;
   gfx_draw (width * s, width * s, 0, 0, &ox, &oy);
   for (int y = 0; y < width; y++)
      for (int x = 0; x < width; x++)
         if (qr[width * y + x] & QR_TAG_BLACK)
            for (int dy = 0; dy < s; dy++)
               for (int dx = 0; dx < s; dx++)
                  gfx_pixel (ox + x * s + dx, oy + y * s + dy, 0xFF);
   free (qr);
#endif
   return NULL;
}

void
tassub (char *name)
{
   if (!*name)
      return;
   char *topic;
   asprintf (&topic, "stat/%s/RESULT", name);
   lwmqtt_subscribe (revk_mqtt (0), topic);
   free (topic);
   asprintf (&topic, "cmnd/%s/POWER", name);
   revk_mqtt_send_raw (topic, 0, NULL, 1);
   free (topic);
}

const char *
app_callback (int client, const char *prefix, const char *target, const char *suffix, jo_t j)
{
   char value[1000];
   int len = 0;
   *value = 0;
   if (j)
   {
      len = jo_strncpy (j, value, sizeof (value));
      if (len < 0)
         return "Expecting JSON string";
      if (len > sizeof (value))
         return "Too long";
   }
   if (prefix && target && suffix && j && !strcmp (prefix, "stat") && !strcmp (suffix, "RESULT"))
   {
      jo_rewind (j);
      if (jo_find (j, "POWER") == JO_STRING)
      {                         // "ON" or "OFF"
         if (!strcmp (target, tasaway))
            tasawaystate = !jo_strcmp (j, "OFF");       // Off means we are away
         else if (!strcmp (target, tasbusy))
            tasbusystate = !jo_strcmp (j, "OFF");       // Off means we are busy
         setactive (tasawaystate ? imageaway : tasbusystate ? imagebusy : imagewait);
      }
   }
   if (client || !prefix || target || strcmp (prefix, prefixcommand) || !suffix)
      return NULL;              //Not for us or not a command from main MQTT
   if (!strcmp (suffix, "connect"))
   {
      mqttinit = 1;
      return "";
   }
   if (!strcmp (suffix, "message"))
   {
      xSemaphoreTake (mutex, portMAX_DELAY);
      override = uptime ();
      gfx_message (value);
      xSemaphoreGive (mutex);
      return "";
   }
   if (!strcmp (suffix, "cancel"))
   {
      override = 0;
      pushed = 0;
      return "";
   }
   if (!strcmp (suffix, "push"))
   {
      if (!*overridename && *value)
         strncpy (overridename, value, sizeof (overridename));
      else
         pushed = uptime ();
      return "";
   }
   if (!strcmp (suffix, "active"))
   {
      setactive (value);
      return "";
   }
   return NULL;
}

// --------------------------------------------------------------------------------
// Web
#ifdef	CONFIG_REVK_APCONFIG
#error 	Clash with CONFIG_REVK_APCONFIG set
#endif

void
push_task (void *arg)
{

   gpio_reset_pin (port_mask (btn1));
   gpio_set_direction (port_mask (btn1), GPIO_MODE_INPUT);
   while (1)
   {
      uint8_t l = gpio_get_level (port_mask (btn1));
      if (!l)
         pushed = uptime ();
      usleep (10000);
   }
}

void
app_main ()
{
   mutex = xSemaphoreCreateBinary ();
   xSemaphoreGive (mutex);
   revk_boot (&app_callback);
   revk_register ("gfx", 0, sizeof (gfxcs), &gfxcs, "- ", SETTING_SET | SETTING_BITFIELD | SETTING_SECRET);     // Header
   revk_register ("image", 0, 0, &imageurl, "http://ota.revk.uk/EPD", SETTING_SECRET);     // Header
   revk_register ("tas", 0, 0, &tasbell, NULL, SETTING_SECRET); // Header
#define io(n,d)           revk_register(#n,0,sizeof(n),&n,"- "#d,SETTING_SET|SETTING_BITFIELD);
#define b(n) revk_register(#n,0,sizeof(n),&n,NULL,SETTING_BOOLEAN);
#define u32(n,d) revk_register(#n,0,sizeof(n),&n,#d,0);
#define s8(n,d) revk_register(#n,0,sizeof(n),&n,#d,SETTING_SIGNED);
#define u8(n,d) revk_register(#n,0,sizeof(n),&n,#d,0);
#define s(n,d) revk_register(#n,0,0,&n,#d,0);
   settings
#undef io
#undef u32
#undef s8
#undef u8
#undef b
#undef s
      revk_start ();
   setactive (imagewait);

   if (leds)
   {
      led_strip_config_t strip_config = {
         .strip_gpio_num = (port_mask (rgb)),
         .max_leds = leds,
         .led_pixel_format = LED_PIXEL_FORMAT_GRB,      // Pixel format of your LED strip
         .led_model = LED_MODEL_WS2812, // LED strip model
         .flags.invert_out = ((rgb & PORT_INV) ? 1 : 0),        // whether to invert the output signal (useful when your hardware has a level inverter)
      };
      led_strip_rmt_config_t rmt_config = {
         .clk_src = RMT_CLK_SRC_DEFAULT,        // different clock source can lead to different power consumption
         .resolution_hz = 10 * 1000 * 1000,     // 10MHz
         .flags.with_dma = true,
      };
      REVK_ERR_CHECK (led_strip_new_rmt_device (&strip_config, &rmt_config, &strip));
      image_load (NULL, NULL, 'M');
   }

   // Web interface
   httpd_config_t config = HTTPD_DEFAULT_CONFIG ();
   config.max_uri_handlers = 5 + revk_num_web_handlers ();
   if (!httpd_start (&webserver, &config))
   {
      register_get_uri ("/", web_root);
      register_get_uri ("/apple-touch-icon.png", web_icon);
      register_get_uri ("/push", web_push);
      register_get_uri ("/message", web_message);
      register_get_uri ("/active", web_active);
      revk_web_settings_add (webserver);
   }
   {
    const char *e = gfx_init (cs: port_mask (gfxcs), sck: port_mask (gfxsck), mosi: port_mask (gfxmosi), dc: port_mask (gfxdc), rst: port_mask (gfxrst), busy: port_mask (gfxbusy), ena: port_mask (gfxena), flip: gfxflip, direct: 1, invert:gfxinvert);
      if (e)
      {
         ESP_LOGE (TAG, "gfx %s", e);
         jo_t j = jo_object_alloc ();
         jo_string (j, "error", "Failed to start");
         jo_string (j, "description", e);
         revk_error ("gfx", &j);
      }
   }
   revk_task ("push", push_task, NULL, 4);
   sleep (1);
   gfx_lock ();
   gfx_clear (255);             // Black
   gfx_unlock ();
   uint32_t lastrefresh = 0;
   uint8_t day = 0;
   while (1)
   {
      usleep (100000);
      time_t now = time (0);
      struct tm t;
      localtime_r (&now, &t);
      uint32_t up = uptime ();
      void addqr (void)
      {
         if (*postcode)
         {
            char temp[200];
            sprintf (temp, "%4d-%02d-%02d %02d:%02d %s", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, postcode);
            gfx_pos (0, gfx_height () - 1, GFX_B | GFX_L | GFX_V);
            gfx_qr (temp, 4);
         }
      }
      const char *basename = getidle (&t);
      if (!revk_link_down () && day != t.tm_mday)
      {                         // Get files
         day = t.tm_mday;
         xSemaphoreTake (mutex, portMAX_DELAY);
         idle = getimage (basename, idle);
         active = getimage (activename, active);
         xSemaphoreGive (mutex);
      }
      if (mqttinit)
      {
         ESP_LOGE (TAG, "MQTT Connected");
         mqttinit = 0;
         last = -1;
         tassub (tasaway);
         tassub (tasbusy);
      }
      if (*overridename)
      {                         // Special override
         uint8_t *image = getimage (overridename, NULL);
         if (image)
         {
            xSemaphoreTake (mutex, portMAX_DELAY);
            override = uptime ();
            last = 0;
            gfx_lock ();
            image_load (overridename, image, 'B');
            addqr ();
            gfx_unlock ();
            xSemaphoreGive (mutex);
            free (image);
         }
         *overridename = 0;
      }
      if (override + holdtime < up)
         override = 0;
      if (override)
         continue;
      if (pushed + holdtime < up)
         pushed = 0;            // Time out
      if (pushed)
      {                         // Bell was pushed
         if (last)
         {                      // Show status as was showing idle
            xSemaphoreTake (mutex, portMAX_DELAY);
            if (!active)
               active = getimage (activename, active);
            last = 0;
            if (*tasbell)
            {
               char *topic = NULL;
               asprintf (&topic, "cmnd/%s/POWER", tasbell);
               revk_mqtt_send_raw (topic, 0, "ON", 1);
               free (topic);
            }
            if (*toot)
            {
               char *pl = NULL;
               asprintf (&pl, "@%s\nDing dong\n%s\n%4d-%02d-%02d %02d:%02d:%02d", toot, activename, t.tm_year + 1900,
                         t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
               revk_mqtt_send_raw ("toot", 0, pl, 1);
               free (pl);
            }
            gfx_lock ();
            // These do a gfx_clear or replace whole buffer anyway
            if (!active)
               gfx_message ("PLEASE/WAIT");
            else
               image_load (activename, active, 'B');
            addqr ();
            gfx_unlock ();
            xSemaphoreGive (mutex);
         }
      } else if (last != now / 60)
      {                         // Show idle
         xSemaphoreTake (mutex, portMAX_DELAY);
         if (!idle)
            idle = getimage (basename, idle);
         gfx_lock ();
         if (!last || (refresh && lastrefresh != up / refresh))
         {
            lastrefresh = up / refresh;
            gfx_refresh ();
         }
         last = now / 60;
         // These do a gfx_clear or replace whole buffer anyway
         if (!idle)
            gfx_message ("CANWCH/Y GLOCH/ / /RING/THE/BELL");
         else
            image_load (basename, idle, 'K');
         addqr ();
         gfx_pos (gfx_width () - 2, gfx_height () - 2, GFX_R | GFX_B);  // Yes slightly in from edge
         gfx_text (1, "%02d:%02d", t.tm_hour, t.tm_min);
         gfx_unlock ();
         if (!active)
            active = getimage (activename, active);     // Just in case
         xSemaphoreGive (mutex);
      }
   }
}
