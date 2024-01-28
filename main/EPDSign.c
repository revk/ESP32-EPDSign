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

#define BITFIELDS "-^"

static struct
{                               // Flags
   uint8_t wificonnect:1;
   uint8_t redraw:1;
   uint8_t lightoverride:1;
} volatile b = { 0 };

volatile uint32_t override = 0;
uint8_t *image = NULL;          // Current image
time_t imagetime = 0;           // Current image time

// Dynamic
#ifdef	CONFIG_REVK_OLD_SETTINGS
struct gpio_s
{
   uint16_t num:14;
   uint16_t inv:1;
   uint16_t set:1;
};
#define	settings		\
	io(rgb,2)	\
	io(gfxena,)	\
        io(gfxmosi,40)  \
        io(gfxsck,39)   \
        io(gfxcs,38)    \
        io(gfxdc,37)    \
        io(gfxrst,36)   \
        io(gfxbusy,35)  \
        u8(gfxflip,6)   \
	u8(startup,10)	\
	u8(leds,25)	\
	u32(refresh,86400)	\
	u32l(recheck,60)	\
	u8l(showtime,0)	\
	b(gfxinvert)	\
	sl(imageurl,)	\
	sl(lights,RGB)	\
	u16l(lighton,0)	\
	u16l(lightoff,0)	\

#define u32(n,d)        uint32_t n;
#define u32l(n,d)        uint32_t n;
#define u16l(n,d)        uint16_t n;
#define s8(n,d) int8_t n;
#define u8(n,d) uint8_t n;
#define u8l(n,d) uint8_t n;
#define b(n) uint8_t n;
#define sl(n,d) char * n;
#define io(n,d)           struct gpio_s n;
settings
#undef io
#undef u32
#undef u32l
#undef u16l
#undef s8
#undef u8
#undef u8l
#undef b
#undef sl
#endif
   httpd_handle_t webserver = NULL;
led_strip_handle_t strip = NULL;

static void
web_head (httpd_req_t * req, const char *title)
{
   revk_web_head (req, title);
   revk_web_send (req, "<style>"        //
                  "body{font-family:sans-serif;background:#8cf;}"       //
                  "</style><body><h1>%s</h1>", title ? : "");
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
   return revk_web_foot (req, 0, 1, NULL);
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
showlights (const char *rgb)
{
   if (!strip)
      return;
   const char *c = rgb;
   for (int i = 0; i < leds; i++)
   {
      revk_led (strip, i, 255, revk_rgb (*c));
      if (*c)
         c++;
      if (!*c)
         c = rgb;
   }
   led_strip_refresh (strip);
}

const char *
app_callback (int client, const char *prefix, const char *target, const char *suffix, jo_t j)
{
   char value[1000];
   int len = 0;
   *value = 0;
   if (j && jo_here (j) == JO_STRING)
   {
      len = jo_strncpy (j, value, sizeof (value));
      if (len < 0)
         return "Expecting JSON string";
      if (len > sizeof (value))
         return "Too long";
   }
   if (client || !prefix || target || strcmp (prefix, prefixcommand) || !suffix)
      return NULL;              //Not for us or not a command from main MQTT
   if (!strcmp (suffix, "setting"))
   {
      imagetime = 0;
      b.redraw = 1;
      return "";
   }
   if (!strcmp (suffix, "connect"))
   {
      return "";
   }
   if (!strcmp (suffix, "wifi") || !strcmp (suffix, "ipv6"))
   {
      b.wificonnect = 1;
      return "";
   }
   if (strip && !strcmp (suffix, "rgb"))
   {
      b.lightoverride = (*value ? 1 : 0);
      showlights (value);
      return "";
   }
   return NULL;
}

int
getimage (char season)
{
   if (!*imageurl || revk_link_down ())
      return 0;
   time_t now = time (0);
   int l = strlen (imageurl);
   char *url = mallocspi (l + 3);
   strcpy (url, imageurl);
   char *s = strrchr (url, '*');
   if (s)
   {
      if (season)
         *s = season;
      else
         while (*s++)
            s[-1] = *s;
   } else if (season)
   {
      url[l++] = '?';
      url[l++] = season;
      url[l] = 0;
   }
   ESP_LOGD (TAG, "Get %s", url);
   const int size = gfx_width () * gfx_height () / 8;
   int len = 0;
   uint8_t *buf = NULL;
   esp_http_client_config_t config = {
      .url = url,
      .crt_bundle_attach = esp_crt_bundle_attach,
   };
   esp_http_client_handle_t client = esp_http_client_init (&config);
   int response = 0;
   if (client)
   {
      if (imagetime)
      {
         char when[50];
         struct tm t;
         gmtime_r (&imagetime, &t);
         strftime (when, sizeof (when), "%a, %d %b %Y %T GMT", &t);
         esp_http_client_set_header (client, "If-Modified-Since", when);
      }
      if (!esp_http_client_open (client, 0))
      {
         if (esp_http_client_fetch_headers (client) == size)
         {
            buf = mallocspi (size);
            if (buf)
               len = esp_http_client_read_response (client, (char *) buf, size);
         }
         response = esp_http_client_get_status_code (client);
         esp_http_client_close (client);
      }
      esp_http_client_cleanup (client);
   }
   ESP_LOGD (TAG, "Get %s %d", url, response);
   if (response != 304)
   {
      {
         jo_t j = jo_object_alloc ();
         jo_string (j, "url", url);
         if (response)
            jo_int (j, "response", response);
         if (len)
         {
            jo_int (j, "len", len);
            if (len != size)
               jo_int (j, "expect", size);
         }
         revk_error ("image", &j);
      }
      if (len == size)
      {
         if (gfxinvert)
            for (int i = 0; i < size; i++)
               buf[i] ^= 0xFF;
         if (image && !memcmp (buf, image, size))
            response = 0;       // No change
         free (image);
         image = buf;
         imagetime = now;
      }
   }
   free (url);
   return response;
}

// --------------------------------------------------------------------------------
// Web
#ifdef	CONFIG_REVK_APCONFIG
#error 	Clash with CONFIG_REVK_APCONFIG set
#endif

void
app_main ()
{
   revk_boot (&app_callback);
#ifdef	CONFIG_REVK_OLD_SETTINGS
   revk_register ("gfx", 0, sizeof (gfxcs), &gfxcs, "- ", SETTING_SET | SETTING_BITFIELD | SETTING_SECRET);     // Header
#define io(n,d)           revk_register(#n,0,sizeof(n),&n,"- "#d,SETTING_SET|SETTING_BITFIELD);
#define b(n) revk_register(#n,0,sizeof(n),&n,NULL,SETTING_BOOLEAN);
#define u32(n,d) revk_register(#n,0,sizeof(n),&n,#d,0);
#define u32l(n,d) revk_register(#n,0,sizeof(n),&n,#d,SETTING_LIVE);
#define u16l(n,d) revk_register(#n,0,sizeof(n),&n,#d,SETTING_LIVE);
#define s8(n,d) revk_register(#n,0,sizeof(n),&n,#d,SETTING_SIGNED);
#define u8(n,d) revk_register(#n,0,sizeof(n),&n,#d,0);
#define u8l(n,d) revk_register(#n,0,sizeof(n),&n,#d,SETTING_LIVE);
#define sl(n,d) revk_register(#n,0,0,&n,#d,SETTING_LIVE);
   settings
#undef io
#undef u32
#undef u32l
#undef u16l
#undef s8
#undef u8
#undef u8l
#undef b
#undef sl
#endif
      revk_start ();

   ESP_LOGE (TAG, "test0=%d", test0);
   ESP_LOGE (TAG, "test1=%d", test1);
   ESP_LOGE (TAG, "test2=%d", test2);
   ESP_LOGE (TAG, "test3=%d", test3);
   ESP_LOGE (TAG, "gfxbusy.set=%d .num=%d .invert=%d", gfxbusy.set, gfxbusy.num, gfxbusy.invert);
   ESP_LOGE(TAG,"lights=%s",lights);

   if (leds && rgb.set)
   {
      led_strip_config_t strip_config = {
         .strip_gpio_num = (rgb.num),
         .max_leds = leds,
         .led_pixel_format = LED_PIXEL_FORMAT_GRB,      // Pixel format of your LED strip
         .led_model = LED_MODEL_WS2812, // LED strip model
         .flags.invert_out = rgb.invert,        // whether to invert the output signal (useful when your hardware has a level inverter)
      };
      led_strip_rmt_config_t rmt_config = {
         .clk_src = RMT_CLK_SRC_DEFAULT,        // different clock source can lead to different power consumption
         .resolution_hz = 10 * 1000 * 1000,     // 10MHz
         .flags.with_dma = true,
      };
      REVK_ERR_CHECK (led_strip_new_rmt_device (&strip_config, &rmt_config, &strip));
      showlights ("b");
   }
   // Web interface
   httpd_config_t config = HTTPD_DEFAULT_CONFIG ();
   config.max_uri_handlers = 5 + revk_num_web_handlers ();
   if (!httpd_start (&webserver, &config))
   {
      register_get_uri ("/", web_root);
      register_get_uri ("/apple-touch-icon.png", web_icon);
      revk_web_settings_add (webserver);
   }
   if (gfxena.set)
   {
      gpio_reset_pin (gfxena.num);
      gpio_set_direction (gfxena.num, GPIO_MODE_OUTPUT);
      gpio_set_level (gfxena.num, gfxena.invert);       // Enable
   }
   {
    const char *e = gfx_init (cs: gfxcs.num, sck: gfxsck.num, mosi: gfxmosi.num, dc: gfxdc.num, rst: gfxrst.num, busy: gfxbusy.num, flip: gfxflip, direct: 1, invert:gfxinvert);
      if (e)
      {
         ESP_LOGE (TAG, "gfx %s", e);
         jo_t j = jo_object_alloc ();
         jo_string (j, "error", "Failed to start");
         jo_string (j, "description", e);
         revk_error ("gfx", &j);
      }
   }
   gfx_lock ();
   gfx_clear (0);
   gfx_refresh ();
   gfx_unlock ();
   uint32_t fresh = 0;
   uint32_t min = 0;
   uint32_t check = 0;
   while (1)
   {
      usleep (100000);
      time_t now = time (0);
      uint32_t up = uptime ();
      if (b.wificonnect)
      {
         b.wificonnect = 0;
         override = up + startup;
         if (startup)
         {
            wifi_ap_record_t ap = {
            };
            esp_wifi_sta_get_ap_info (&ap);
            char msg[1000];
            char *p = msg;
            char temp[20];
            p += sprintf (p, "[-6]%s/%s/[3]%s %s/[6] / /", appname, hostname, revk_version, revk_build_date (temp) ? : "?");
            if (sta_netif && *ap.ssid)
            {
               p += sprintf (p, "[6]WiFi/[-6]%s/[6] /Channel %d/RSSI %d/ /", (char *) ap.ssid, ap.primary, ap.rssi);
               {
                  esp_netif_ip_info_t ip;
                  if (!esp_netif_get_ip_info (sta_netif, &ip) && ip.ip.addr)
                     p += sprintf (p, "IPv4/" IPSTR "/ /", IP2STR (&ip.ip));
               }
#ifdef CONFIG_LWIP_IPV6
               {
                  esp_ip6_addr_t ip[LWIP_IPV6_NUM_ADDRESSES];
                  int n = esp_netif_get_all_ip6 (sta_netif, ip);
                  if (n)
                  {
                     p += sprintf (p, "IPv6/[2]");
                     char *q = p;
                     for (int i = 0; i < n; i++)
                        p += sprintf (p, IPV6STR "/", IPV62STR (ip[i]));
                     while (*q)
                     {
                        *q = toupper (*q);
                        q++;
                     }
                  }
               }
#endif
            }
            ESP_LOGE (TAG, "%s", msg);
            gfx_lock ();
            gfx_message (msg);
            gfx_unlock ();
         }
      }
      if (override)
      {
         if (override < up)
            min = override = 0;
         else
            continue;
      }
      if (now / 60 == min && !b.redraw)
         continue;              // Check / update every minute
      min = now / 60;
      struct tm t;
      localtime_r (&now, &t);
      if (*lights && !b.lightoverride)
      {
         int hhmm = t.tm_hour * 100 + t.tm_min;
         showlights (lighton == lightoff || (lighton < lightoff && lighton <= hhmm && lightoff > hhmm)
                     || (lightoff < lighton && (lighton <= hhmm || lightoff > hhmm)) ? lights : "");
      }
      b.redraw = 0;
      int response = 0;
      {                         // Seasonal changes
         static char lastseason = 0;
         char season = revk_season (now);
#ifdef  CONFIG_REVK_LUNAR
         if (now < revk_moon_full_last (now) + 12 * 3600 || now > revk_moon_full_next (now) - 12 * 3600)
            season = 'M';
         if (now < revk_moon_new (now) + 12 * 3600 && now > revk_moon_new (now) - 12 * 3600)
            season = 'N';
#endif
         if (lastseason != season)
         {                      // Change of image
            lastseason = season;
            imagetime = 0;
         }
         if (!recheck || now / recheck != check || !imagetime)
         {                      // Periodic image check
            if (recheck)
               check = now / recheck;
            response = getimage (season);
         }
      }
      if (response != 200 && image && !showtime)
         continue;              // Static image
      gfx_lock ();
      if (refresh && now / refresh != fresh)
      {                         // Periodic refresh, e.g. once a day
         fresh = now / refresh;
         gfx_refresh ();
      } else if (response == 200 && (!showtime || !image))
         gfx_refresh ();        // New image but not doing the regular clock updates
      if (image)
         gfx_load (image);
      else
         gfx_clear (0);
      if (!image && response)
      {                         // Error
         gfx_pos (0, 0, GFX_L | GFX_T);
         gfx_7seg (9, "%d", response);
         gfx_pos (0, 100, GFX_L | GFX_T);
         gfx_text (-1, "%s", imageurl);
      }
      if (showtime || !image)
      {
         gfx_pos (gfx_width () / 2, gfx_height () - 1, GFX_C | GFX_B);
         if (showtime * (6 * 15 + 1) <= gfx_width ())   // Datetime fits
            gfx_7seg (showtime ? : 4, "%04d-%02d-%02d %02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
         else
            gfx_7seg (showtime, "%02d:%02d", t.tm_hour, t.tm_min);
      }
      gfx_unlock ();
   }
}

void
revk_web_extra (httpd_req_t * req)
{
   revk_web_send (req, "<tr><td>ImageURL</td><td><input size=80 name=imageurl value='%s'></td><td>"     //
                  "<tr><td>Recheck</td><td><input size=5 name=recheck value='%d'> seconds</td></tr>"    //
                  "<tr><td>ShowTime</td><td><input size=2 name=showtime value='%d'> size</td></tr>"     //
                  "<tr><td>Lights</td><td><input size=20 name=lights value='%s'> On:<input name=lighton value='%04d' size=5> Off:<input name=lightoff value='%04d' size=5> HHMM</td></tr>",     //
                  imageurl, recheck, showtime, lights, lighton, lightoff);
}
