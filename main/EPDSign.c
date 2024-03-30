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
#include "esp_vfs_fat.h"
#include "gfx.h"
#include "iec18004.h"
#include <hal/spi_types.h>
#include <driver/gpio.h>

#define	LEFT	0x80            // Flags on font size
#define	RIGHT	0x40
#define	LINE	0x20
#define	MASK	0x1F
#define MINSIZE	4

const char sd_mount[] = "/sd";

static struct
{                               // Flags
   uint8_t wificonnect:1;
   uint8_t redraw:1;
   uint8_t lightoverride:1;
} volatile b = { 0 };

volatile uint32_t override = 0;
uint8_t *image = NULL;          // Current image
time_t imagetime = 0;           // Current image time
time_t binnext = 0;
time_t binfirst = 0;
char *bins = NULL;
int defcon = -1;                // DEFCON level

led_strip_handle_t strip = NULL;
sdmmc_card_t *card = NULL;

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

const char *const longday[] = { "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY" };

time_t
parse_time (const char *t)
{
   struct tm tm = { 0 };
   int y = 0,
      m = 0,
      d = 0,
      H = 0,
      M = 0,
      S = 0;
   sscanf (t, "%d-%d-%d %d:%d:%d", &y, &m, &d, &H, &M, &S);
   tm.tm_year = y - 1900;
   tm.tm_mon = m - 1;
   tm.tm_mday = d;
   tm.tm_hour = H;
   tm.tm_min = M;
   tm.tm_sec = S;
   tm.tm_isdst = -1;
   return mktime (&tm);
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

char *
setdefcon (int level, char *value)
{                               // DEFCON state
   // With value it is used to turn on/off a defcon state, the lowest set dictates the defcon level
   // With no value, this sets the DEFCON state directly instead of using lowest of state set
   static uint8_t state = 0;    // DEFCON state
   if (*value)
   {
      if (*value == '1' || *value == 't' || *value == 'y')
         state |= (1 << level);
      else
         state &= ~(1 << level);
      int l;
      for (l = 0; l < 8 && !(state & (1 << l)); l++);
      defcon = l;
   } else
      defcon = level;
   return "";
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
   if (prefix && !strcmp (prefix, "DEFCON") && target && isdigit ((int) *target) && !target[1])
   {
      const char *err = setdefcon (*target - '0', value);
      b.redraw = 1;
      return err;
   }
   if (client || !prefix || target || strcmp (prefix, prefixcommand) || !suffix)
      return NULL;
   // Not for us or not a command from main MQTT
   if (!strcmp (suffix, "setting"))
   {
      imagetime = 0;
      b.redraw = 1;
      return "";
   }
   if (!strcmp (suffix, "connect"))
   {
      lwmqtt_subscribe (revk_mqtt (0), "DEFCON/#");
      return "";
   }
   if (!strcmp (suffix, "shutdown"))
   {
      if (card)
      {
         esp_vfs_fat_sdcard_unmount (sd_mount, card);
         card = NULL;
      }
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
   if (!*imageurl)
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
   int32_t len = 0;
   uint8_t *buf = NULL;
   esp_http_client_config_t config = {
      .url = url,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .timeout_ms = 20000,
   };
   int response = -1;
   if (!revk_link_down ())
   {
      esp_http_client_handle_t client = esp_http_client_init (&config);
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
            len = esp_http_client_fetch_headers (client);
            if (len == size)
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
   }
   ESP_LOGD (TAG, "Get %s %d", url, response);
   if (response != 304)
   {
      {
         jo_t j = jo_object_alloc ();
         jo_string (j, "url", url);
         if (response && response != -1)
            jo_int (j, "response", response);
         if (len == -ESP_ERR_HTTP_EAGAIN)
            jo_string (j, "error", "timeout");
         else if (len)
         {
            jo_int (j, "len", len);
            if (len != size)
               jo_int (j, "expect", size);
         }
         revk_error ("image", &j);
      }
      if (len == size && buf)
      {
         if (gfxinvert)
            for (int32_t i = 0; i < size; i++)
               buf[i] ^= 0xFF;
         if (image && !memcmp (buf, image, size))
            response = 0;       // No change
         free (image);
         image = buf;
         imagetime = now;
         buf = NULL;
      }
   }
   if (card)
   {                            // SD
      char *s = strrchr (url, '/');
      if (s)
      {
         char *fn = NULL;
         asprintf (&fn, "%s%s", sd_mount, s);
         if (image && response == 200)
         {                      // Save to card
            FILE *f = fopen (fn, "w");
            if (f)
            {
               jo_t j = jo_object_alloc ();
               if (fwrite (image, size, 1, f) != 1)
                  jo_string (j, "error", "write failed");
               fclose (f);
               jo_string (j, "write", fn);
               revk_info ("SD", &j);
            }
         } else if (!image || (response && response != 304))
         {                      // Load from card
            FILE *f = fopen (fn, "r");
            if (f)
            {
               if (!buf)
                  buf = mallocspi (size);
               if (buf)
               {
                  if (fread (buf, size, 1, f) == 1)
                  {
                     if (image && !memcmp (buf, image, size))
                        response = 0;   // No change
                     else
                     {
                        jo_t j = jo_object_alloc ();
                        jo_string (j, "read", fn);
                        revk_info ("SD", &j);
                        response = 200; // Treat as received
                        free (image);
                        image = buf;
                        buf = NULL;
                     }
                  }
               }
               fclose (f);
            }
         }
         free (fn);
      }
   }
   free (url);
   free (buf);
   return response;
}

//--------------------------------------------------------------------------------
// Web

#ifdef	CONFIG_REVK_APCONFIG
#error 	Clash with CONFIG_REVK_APCONFIG set
#endif

void
app_main ()
{
   revk_boot (&app_callback);
   revk_start ();

   if (leds && rgb.set)
   {
      led_strip_config_t strip_config = {
         .strip_gpio_num = (rgb.num),
         .max_leds = leds,
         .led_pixel_format = LED_PIXEL_FORMAT_GRB,      // Pixel format of your LED strip
         .led_model = LED_MODEL_WS2812, // LED strip model
         .flags.invert_out = rgb.invert,        // whether to invert the output signal(useful when your hardware has a level inverter)
      };
      led_strip_rmt_config_t rmt_config = {
         .clk_src = RMT_CLK_SRC_DEFAULT,        // different clock source can lead to different power consumption
         .resolution_hz = 10 * 1000 * 1000,     // 10 MHz
         .flags.with_dma = true,
      };
      REVK_ERR_CHECK (led_strip_new_rmt_device (&strip_config, &rmt_config, &strip));
      showlights ("b");
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
   if (sdmosi.set)
   {
      revk_gpio_input (sdcd);
      sdmmc_host_t host = SDSPI_HOST_DEFAULT ();
      host.max_freq_khz = SDMMC_FREQ_PROBING;
      spi_bus_config_t bus_cfg = {
         .mosi_io_num = sdmosi.num,
         .miso_io_num = sdmiso.num,
         .sclk_io_num = sdsck.num,
         .quadwp_io_num = -1,
         .quadhd_io_num = -1,
         .max_transfer_sz = 4000,
      };
      esp_err_t ret = spi_bus_initialize (host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
      if (ret != ESP_OK)
      {
         jo_t j = jo_object_alloc ();
         jo_string (j, "error", "SPI failed");
         jo_int (j, "code", ret);
         jo_int (j, "MOSI", sdmosi.num);
         jo_int (j, "MISO", sdmiso.num);
         jo_int (j, "CLK", sdsck.num);
         revk_error ("SD", &j);
      } else
      {
         esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = 1,
            .max_files = 2,
            .allocation_unit_size = 16 * 1024,
            .disk_status_check_enable = 1,
         };
         sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT ();
         //slot_config.gpio_cs = sdss.num;
         slot_config.gpio_cs = -1;
         revk_gpio_output (sdss, 0);    // Bodge for faster access when one SD card and ESP IDF V5+
         slot_config.gpio_cd = sdcd.num;
         slot_config.host_id = host.slot;
         ret = esp_vfs_fat_sdspi_mount (sd_mount, &host, &slot_config, &mount_config, &card);
         if (ret)
         {
            jo_t j = jo_object_alloc ();
            jo_string (j, "error", "Failed to mount");
            jo_int (j, "code", ret);
            revk_error ("SD", &j);
            card = NULL;
         }
         // TODO SD LED
      }
   }
   gfx_lock ();
   gfx_clear (0);
   gfx_refresh ();
   gfx_unlock ();
   uint32_t fresh = 0;
   uint32_t min = 0;
   uint32_t check = 0;
   uint8_t reshow = 0;
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
      int response = 0;
      {                         // Seasonal changes
         static char lastseason = 0;
         const char season = *revk_season (now);
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
      if (response != 200 && image && !showtime && !fast && !b.redraw)
         continue;
      b.redraw = 0;
      if (*binsurl && (!bins || now > binnext) && !revk_link_down ())
      {                         // Load bins
         free (bins);
         bins = NULL;
         int32_t len = 0;
         esp_http_client_config_t config = {
            .url = binsurl,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 60000,        // Can take time
         };
         esp_http_client_handle_t client = esp_http_client_init (&config);
         if (client)
         {
            if (!esp_http_client_open (client, 0))
            {
               len = esp_http_client_fetch_headers (client);
               if (!len)
                  len = 1000;
               if (len > 0 && len <= 1000)
               {
                  bins = mallocspi (len);
                  if (bins)
                  {
                     len = esp_http_client_read_response (client, bins, len - 1);
                     esp_http_client_close (client);
                     if (len > 0)
                     {
                        bins[len] = 0;
                        ESP_LOGD (TAG, "Bins %s", bins);
                     } else
                     {
                        free (bins);
                        bins = NULL;
                     }
                  }
               }
            }
            esp_http_client_cleanup (client);
         }
         if (bins)
         {
            binfirst = 0;
            jo_t j = jo_parse_str (bins);
            jo_type_t t = jo_next (j);  // Start object
            while (t == JO_TAG)
            {
               char tag[20] = "",
                  val[25] = "";
               jo_strncpy (j, tag, sizeof (tag));
               t = jo_next (j);
               jo_strncpy (j, val, sizeof (val));
               time_t this = parse_time (val);
               if (this > now - 3600 && (!binfirst || this < binfirst))
                  binfirst = this;
               t = jo_skip (j);
            }
            jo_free (&j);
            if (binfirst)
               binnext = binfirst + 3600;
         }
         if (binnext < now + 3600)
            binnext = now + 3600;
      }
      // Static image
      gfx_lock ();
      if (reshow)
         reshow--;
      if (refresh && now / refresh != fresh)
      {                         // Periodic refresh, e.g.once a day
         fresh = now / refresh;
         gfx_refresh ();
      } else if (response == 200 && !showtime)
      {                         // Image changed
         if (fast)
            reshow = fast;      // Fast update
         else
            gfx_refresh ();     // New image but not doing the regular clock updates
      }
      if (image)
         gfx_load (image);
      else
         gfx_clear (0);
      if (bins && binfirst)
      {                         // Show next bin dates
         gfx_pos (gfx_width () / 2, 0, GFX_C | GFX_T | GFX_V);
         //gfx_text (-7, "NEXT BIN");
         struct tm tm;
         localtime_r (&binfirst, &tm);
         gfx_text (-9, tm.tm_yday == t.tm_yday ? "* TODAY *" : tm.tm_yday == t.tm_yday + 1 ? "TOMORROW" : longday[tm.tm_wday]);
         char lights[10],
          *l = lights;
         jo_t j = jo_parse_str (bins);
         jo_type_t t = jo_next (j);     // Start object
         while (t == JO_TAG)
         {
            char tag[20] = "",
               val[25] = "";
            jo_strncpy (j, tag, sizeof (tag));
            t = jo_next (j);
            jo_strncpy (j, val, sizeof (val));
            time_t this = parse_time (val);
            if (this && this <= binfirst)
            {
               if (*tag && tag[1] == ':')
               {
                  if (l < lights + sizeof (lights) - 1)
                     *l++ = *tag;
                  gfx_text (-7, tag + 2);
               } else
                  gfx_text (-7, tag);
            }
            t = jo_skip (j);
         }
         jo_free (&j);
         *l = 0;
         if (*lights)
         {
            showlights (binfirst < now + 86400 ? lights : "K");
            b.lightoverride = 1;
         }
      }
      if (!image && response)
      {                         // Error
         gfx_pos (0, 0, GFX_L | GFX_T);
         gfx_7seg (9, "%d", response);
         gfx_pos (0, 100, GFX_L | GFX_T);
         gfx_text (-1, "%s", *imageurl ? imageurl : "No URL set");
      }
      // Info at bottom
      gfx_pos_t y = gfx_height () - 1;
      gfx_pos_t lasty = 0;
      uint8_t lasts = 0;
      int start (uint8_t s)
      {
         if (lasts & LINE)
         {
            gfx_pos (0, y, 0);
            gfx_fill (gfx_width (), 1, 255);
            y -= ((lasts & MASK) ? : MINSIZE) / 2;
            y -= ((s & MASK) ? : MINSIZE) / 2;
         }
         if (((lasts & RIGHT) && (s & LEFT)) || ((lasts & LEFT) && (s & RIGHT)))
            y = lasty;          // Assuming left/right can coexist (may be the case for DEFCON)
         lasts = s;
         gfx_pos ((s & LEFT) ? 0 : (s & RIGHT) ? gfx_width () - 1 : gfx_width () / 2, lasty = y,
                  (s & LEFT ? GFX_L : 0) | (s & RIGHT ? GFX_R : 0) | (s & (LEFT | RIGHT) ? 0 : GFX_C) | GFX_B);
         s &= MASK;
         if (!s)
            s = MINSIZE;
         return s;
      }
      if (showtime || !image)
      {
         int s = start (showtime);
         if (*refdate)
         {
            uint32_t secs = 0;

            int year = t.tm_year + 1900;
            struct tm t = { 0 };
            int y = 0,
               m = 0,
               d = 0,
               H = 0,
               M = 0,
               S = 0;
            if (sscanf (refdate, "%d-%d-%d %d:%d:%d", &y, &m, &d, &H, &M, &S) >= 3)
            {
               t.tm_year = (y ? : year) - 1900;
               t.tm_mon = m - 1;
               t.tm_mday = d;
               t.tm_hour = H;
               t.tm_min = M;
               t.tm_sec = S;
               t.tm_isdst = -1;
               int s = mktime (&t) - now;
               if (s < 0 && !y)
               {                // To next date
                  t.tm_year++;
                  t.tm_isdst = -1;
                  s = mktime (&t) - now;
               }
               if (s < 0)
                  s = -s;
               secs = s;
            } else
            {                   // Try uptime as hostname
               struct sockaddr_in6 dest_addr = { 0 };
               inet6_aton (refdate, &dest_addr.sin6_addr);
               dest_addr.sin6_family = AF_INET6;
               dest_addr.sin6_port = htons (161);
               //dest_addr.sin6_scope_id = esp_netif_get_netif_impl_index(EXAMPLE_INTERFACE);
               int sock = socket (AF_INET6, SOCK_DGRAM, IPPROTO_IPV6);
               if (sock < 0)
                  ESP_LOGE (TAG, "SNMP sock failed %s", refdate);
               else
               {                // very crude IPv6 SNMP uptime
                  uint8_t payload[] =
                     { 0x30, 0x29, 0x02, 0x01, 0x01, 0x04, 0x06, 0x70, 0x75, 0x62, 0x6c, 0x69, 0x63, 0xa0, 0x1c, 0x02, 0x04,
                     0x00, 0x00, 0x00, 0x00, 0x02, 0x01, 0x00, 0x02, 0x01, 0x00, 0x30, 0x0e, 0x30, 0x0c, 0x06, 0x08, 0x2b,
                     0x06, 0x01, 0x02, 0x01, 0x01, 0x03, 0x00, 0x05, 0x00
                  };
                  uint32_t id = ((esp_random () & 0x7FFFFF7F) | 0x40000040);    // bodegy
                  *(uint32_t *) (payload + 17) = id;
                  int err = sendto (sock, payload, sizeof (payload), 0, (struct sockaddr *) &dest_addr, sizeof (dest_addr));
                  if (err < 0)
                     ESP_LOGE (TAG, "SNMP Tx failed");
                  else
                  {
                     struct timeval timeout;
                     timeout.tv_sec = 1;
                     timeout.tv_usec = 0;
                     setsockopt (sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
                     uint8_t rx[100];
                     struct sockaddr_storage source_addr;
                     socklen_t socklen = sizeof (source_addr);
                     int len = recvfrom (sock, rx, sizeof (rx), 0, (struct sockaddr *) &source_addr, &socklen);
                     if (len < 47)
                        ESP_LOGE (TAG, "SNMP Rx failed (%d)", len);
                     else
                     {          // Crude, not parsed
                        if (*(uint32_t *) (rx + 17) != id)
                           ESP_LOGE (TAG, "SNMP Bad ID (len %d) ID %08lX", len, id);
                        else
                           secs = ((rx[len - 4] << 24) | (rx[len - 3] << 16) | (rx[len - 2] << 8) | rx[len - 1]) / 100;
                     }
                  }
                  close (sock);
               }
            }
            // Show days, 4 sig fig
            if (!secs)
               gfx_7seg (s, "----");
            else if (secs < 86400 && s * (7 + 6 + 7 + 6 + 6) <= gfx_width ())
               gfx_7seg (s, "0:%02ld:%02ld", secs / 3600, secs % 3600 / 60);
            else if (secs < 864000)
               gfx_7seg (s, "%ld.%03ld", secs / 86400, secs % 86400 * 10 / 864);
            else if (secs < 8640000)
               gfx_7seg (s, "%ld.%02ld", secs / 86400, secs % 86400 / 864);
            else if (secs < 86400000)
               gfx_7seg (s, "%ld.%ld", secs / 86400, secs % 86400 / 8640);
            else if (secs < 864000000)
               gfx_7seg (s, "%ld", secs / 86400);
            else
               gfx_7seg (s, "9999");
         } else if (s * (6 * 15 + 1) <= gfx_width ())   // Datetime fits
            gfx_7seg (s, "%04d-%02d-%02d %02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
         else
            gfx_7seg (s, "%02d:%02d", t.tm_hour, t.tm_min);
         y -= s * 10;
      }
      if (showday)
      {
         int s = start (showday);
         gfx_text (s, longday[t.tm_wday]);
         y -= s * 8;
      }
      if (showdefcon)
      {
         int s = start (showdefcon);
         if (defcon < 0 || defcon > 5)
            gfx_7seg (s, "-");
         else
            gfx_7seg (s, "%d", defcon);
         y -= s * 10;
      }
#ifdef	CONFIG_REVK_SOLAR
      if (showset && (poslat || poslon))
      {
         int s = start (showset);
         time_t when =
            sun_set (t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, (double) poslat / poslat_scale, (double) poslon / poslon_scale,
                     SUN_CIVIL_TWILIGHT);
         struct tm tm = { 0 };
         localtime_r (&when, &tm);
         gfx_7seg (s, "%02d:%02d", tm.tm_hour, tm.tm_min);
         y -= s * 10;
      }
      if (showrise && (poslat || poslon))
      {
         int s = start (showrise);
         time_t when =
            sun_rise (t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, (double) poslat / poslat_scale, (double) poslon / poslon_scale,
                      SUN_CIVIL_TWILIGHT);
         struct tm tm = { 0 };
         localtime_r (&when, &tm);
         gfx_7seg (s, "%02d:%02d", tm.tm_hour, tm.tm_min);
         y -= s * 10;
      }
#endif

      start (0);
      gfx_unlock ();
      if (reshow)
         b.redraw = 1;
   }
}

void
revk_web_extra (httpd_req_t * req)
{
   revk_web_setting (req, "Image Base URL", "imageurl");
   revk_web_setting (req, "Image check", "recheck");
   revk_web_setting (req, "Clock size", "showtime");
   revk_web_setting (req, "Reference date", "refdate");
   revk_web_setting (req, "Day size", "showday");
   revk_web_setting (req, "Light pattern", "lights");
   revk_web_setting (req, "Light on", "lighton");
   revk_web_setting (req, "Light off", "lightoff");
   revk_web_setting (req, "Image invert", "gfxinvert");
}
