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

volatile uint8_t wificonnect = 1;
volatile uint32_t override = 0;

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
        u8(gfxflip,6)   \
	u8(holdtime,30)	\
	u32(refresh,3600)	\
	b(gfxinvert)	\

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
SemaphoreHandle_t mutex = NULL;

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
   if (client || !prefix || target || strcmp (prefix, prefixcommand) || !suffix)
      return NULL;              //Not for us or not a command from main MQTT
   if (!strcmp (suffix, "connect"))
   {
      return "";
   }
   if (!strcmp (suffix, "wifi") || !strcmp (suffix, "ipv6"))
   {
      wificonnect = 1;
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
app_main ()
{
   mutex = xSemaphoreCreateBinary ();
   xSemaphoreGive (mutex);
   revk_boot (&app_callback);
   revk_register ("gfx", 0, sizeof (gfxcs), &gfxcs, "- ", SETTING_SET | SETTING_BITFIELD | SETTING_SECRET);     // Header
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

   // Web interface
   httpd_config_t config = HTTPD_DEFAULT_CONFIG ();
   config.max_uri_handlers = 5 + revk_num_web_handlers ();
   if (!httpd_start (&webserver, &config))
   {
      register_get_uri ("/", web_root);
      register_get_uri ("/apple-touch-icon.png", web_icon);
      revk_web_settings_add (webserver);
   }
   if (gfxena)
   {
      gpio_reset_pin (port_mask (gfxena));
      gpio_set_direction (port_mask (gfxena), GPIO_MODE_OUTPUT);
      gpio_set_level (port_mask (gfxena), gfxena & PORT_INV ? 0 : 1);   // Enable
   }
   {
    const char *e = gfx_init (cs: port_mask (gfxcs), sck: port_mask (gfxsck), mosi: port_mask (gfxmosi), dc: port_mask (gfxdc), rst: port_mask (gfxrst), busy: port_mask (gfxbusy), flip: gfxflip, direct: 1, invert:gfxinvert);
      if (e)
      {
         ESP_LOGE (TAG, "gfx %s", e);
         jo_t j = jo_object_alloc ();
         jo_string (j, "error", "Failed to start");
         jo_string (j, "description", e);
         revk_error ("gfx", &j);
      }
   }
   ESP_LOGE (TAG, "Display %dx%d (%d)", gfx_width (), gfx_height (), gfxflip);

   uint32_t min = 0;
   while (1)
   {
      usleep (100000);
      time_t now = time (0);
      uint32_t up = uptime ();
      if (wificonnect)
      {
         wificonnect = 0;
         wifi_ap_record_t ap = {
         };
         esp_wifi_sta_get_ap_info (&ap);
         char msg[1000];
         char *p = msg;
         p += sprintf (p, "[-5]%s/%s/[5] / /[5]WiFi/[-5]%s/[5] /Channel %d/RSSI %d/ /", appname, hostname, (char *) ap.ssid, ap.primary, ap.rssi);
         if (sta_netif)
         {
            {
               esp_netif_ip_info_t ip;
               if (!esp_netif_get_ip_info (sta_netif, &ip) && ip.ip.addr)
                  p += sprintf (p, "IPv4/" IPSTR "/ /", IP2STR (&ip.ip));
            }
#ifdef CONFIG_LWIP_IPV6
            {
               esp_ip6_addr_t ip;
               if (!esp_netif_get_ip6_global (sta_netif, &ip))
                  p += sprintf (p, "IPv6/[2]" IPV6STR "/ /", IPV62STR (ip));
            }
#endif
         }
         gfx_lock ();
         gfx_message (msg);
         gfx_unlock ();
         override = now + 10;
      }
      if (override)
      {
         if (override < up)
            override = 0;
         else
            continue;
      }
      if (now / 60 == min)
         continue;
      min = now / 60;
      struct tm t;
      localtime_r (&now, &t);
      gfx_lock ();
      gfx_clear (0);
      gfx_pos (gfx_width () - 1, gfx_height () - 1, GFX_R | GFX_B);
      gfx_7seg (3, "%02d:%02d", t.tm_hour, t.tm_min);

      gfx_unlock ();
   }
}
