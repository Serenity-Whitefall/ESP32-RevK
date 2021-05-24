/*
 * Main control code, working with WiFi, MQTT, and managing settings and OTA Copyright � �2019 Adrian Kennard Andrews & Arnold Ltd
 */
static const char
__attribute__((unused)) * TAG = "RevK";

#include "revk.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_tls.h"
#ifdef	CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#else
#include "lecert.h"
#endif
#include "esp_int_wdt.h"
#include "esp_task_wdt.h"
#include "esp_sntp.h"
#include "esp_phy_init.h"
#ifdef	CONFIG_REVK_APCONFIG
#include "esp_http_server.h"
#endif
#include <driver/gpio.h>

#ifndef CONFIG_TASK_WDT_PANIC
#warning Set CONFIG_TASK_WDT_PANIC
#endif

#define	settings	\
		s(otahost,CONFIG_REVK_OTAHOST);		\
		s(otacert,CONFIG_REVK_OTACERT);		\
		s(ntphost,CONFIG_REVK_NTPHOST);		\
		s(tz,CONFIG_REVK_TZ);			\
		u32(watchdogtime,10);			\
		s(appname,CONFIG_REVK_APPNAME);		\
		snl(hostname,NULL);			\
		p(command);				\
		p(setting);				\
		p(state);				\
		p(event);				\
		p(info);				\
		p(error);				\
		io(blink);				\

#define	apconfigsettings	\
		u32(apport,CONFIG_REVK_APPORT);		\
		u32(aptime,CONFIG_REVK_APTIME);		\
		u32(apwait,CONFIG_REVK_APWAIT);		\
		io(apgpio);		\

#define	mqttsettings	\
		u32(mqttreset,0);			\
		sa(mqtthost,3,CONFIG_REVK_MQTTHOST);	\
		sa(mqttuser,3,CONFIG_REVK_MQTTUSER);	\
		sa(mqttpass,3,CONFIG_REVK_MQTTPASS);	\
		u16(mqttport,3,CONFIG_REVK_MQTTPORT);	\
		u32(mqttsize,CONFIG_REVK_MQTTSIZE);	\
		sa(mqttcert,3,CONFIG_REVK_MQTTCERT);	\
    		b(mqttquiet,CONFIG_REVK_MQTTQUIET);	\

#define	wifisettings	\
		u32(wifireset,0);			\
		sa(wifissid,3,CONFIG_REVK_WIFISSID);	\
		sa(wifiip,3,CONFIG_REVK_WIFIIP);	\
		sa(wifigw,3,CONFIG_REVK_WIFIGW);	\
		sa(wifidns,3,CONFIG_REVK_WIFIDNS);	\
		f(wifibssid,3,6,CONFIG_REVK_WIFIBSSID);	\
		u8a(wifichan,3,CONFIG_REVK_WIFICHAN);	\
		sa(wifipass,3,CONFIG_REVK_WIFIPASS);	\
		s(apssid,CONFIG_REVK_APSSID);		\
		s(appass,CONFIG_REVK_APPASS);		\
		s(apip,CONFIG_REVK_APIP);		\
		b(aplr,CONFIG_REVK_APLR);		\
		b(aphide,CONFIG_REVK_APHIDE);		\

#define	meshsettings	\
		s(wifissid,CONFIG_REVK_WIFISSID);	\
		s(wifiip,CONFIG_REVK_WIFIIP);		\
		s(wifigw,CONFIG_REVK_WIFIGW);		\
		s(wifidns,CONFIG_REVK_WIFIDNS);		\
		h(wifibssid,6,CONFIG_REVK_WIFIBSSID);	\
		u8(wifichan,CONFIG_REVK_WIFICHAN);	\
		s(wifipass,CONFIG_REVK_WIFIPASS);	\
		h(meshid,6,CONFIG_REVK_MESHID);		\
		s(meshpass,CONFIG_REVK_MESHPASS);	\

#define s(n,d)		static char *n;
#define snl(n,d)	static char *n;
#define sa(n,a,d)	static char *n[a];
#define f(n,a,s,d)	static char n[a][s];
#define	u32(n,d)	static uint32_t n;
#define	u16(n,a,d)	static uint16_t n[a];
#define	i16(n)		static int16_t n;
#define	u8a(n,a,d)	static uint8_t n[a];
#define	u8(n,d)		static uint8_t n;
#define	b(n,d)		static uint8_t n;
#define	s8(n,d)		static int8_t n;
#define	io(n)		static uint8_t n;
#define p(n)		char *prefix##n;
#define h(n,l,d)	static char n[l];
settings
#ifdef	CONFIG_REVK_WIFI
wifisettings
#endif
#ifdef	CONFIG_REVK_MQTT
mqttsettings
#endif
#ifdef	CONFIG_REVK_APCONFIG
apconfigsettings
#endif
#ifdef	CONFIG_REVK_MESH
meshsettings
#endif
#undef s
#undef snl
#undef sa
#undef f
#undef u32
#undef u16
#undef i16
#undef u8
#undef b
#undef u8a
#undef s8
#undef io
#undef p
#undef h
/* Local types */
   typedef struct setting_s setting_t;
   struct setting_s
   {
      nvs_handle      nvs;
      setting_t      *next;
      const char     *name;
      const char     *defval;
      void           *data;
      uint16_t        size;
      uint8_t         array;
      uint8_t         flags;
   };
/* Public */
   const char     *revk_version = "";   /* Git version */
   const char     *revk_app = "";       /* App name */
   char            revk_id[7];  /* Chip ID as hex (derived from MAC) */
   uint32_t        revk_binid = 0;      /* Binary chip ID */

/* Local */
   static EventGroupHandle_t revk_group;
#ifdef	CONFIG_REVK_WIFI
   const static int GROUP_WIFI = BIT0;
   const static int GROUP_WIFI_DONE = BIT1;
   const static int GROUP_WIFI_TRY = BIT2;
#endif
#ifdef	CONFIG_REVK_MQTT
   const static int GROUP_MQTT = BIT3;
   const static int GROUP_MQTT_DONE = BIT4;
   const static int GROUP_MQTT_TRY = BIT5;
#endif
#ifdef	CONFIG_REVK_APCONFIG
   const static int GROUP_APCONFIG = BIT6;
   const static int GROUP_APCONFIG_DONE = BIT7;
#endif
   static TaskHandle_t ota_task_id = NULL;
#ifdef	CONFIG_REVK_APCONFIG
   static TaskHandle_t ap_task_id = NULL;
#endif
   static app_command_t *app_command = NULL;
   esp_mqtt_client_handle_t mqtt_client = NULL;
   static int64_t  restart_time = 0;
   static int64_t  nvs_time = 0;
   static int64_t  slow_connect = 0;
   static const char *restart_reason = "Unknown";
   static nvs_handle nvs = -1;
   static setting_t *setting = NULL;
#ifdef	CONFIG_REVK_WIFI
   static int      wifi_count = 0;
   static int      wifi_index = -1;
   static esp_netif_t *sta_netif = NULL;
#endif
#ifdef	CONFIG_REVK_MQTT
   static int      mqtt_count = 0;
   static int      mqtt_index = -1;
#endif
   static int64_t  lastonline = 0;
   static char     wdt_test = 0;
#ifdef	CONFIG_REVK_APCONFIG
   static esp_netif_t *ap_netif = NULL;
#endif
   static uint8_t  blink_on = 0,
                   blink_off = 0;

/* Local functions */
#ifdef	CONFIG_REVK_APCONFIG
   static void     ap_task(void *pvParameters);
#endif

#ifdef	CONFIG_REVK_MQTT
   static void     mqtt_next(void);
#endif

#ifdef	CONFIG_REVK_WIFI
   static void     makeip(esp_netif_ip_info_t * info, const char *ip, const char *gw)
{
   char           *i = strdup(ip);
   int             cidr = 24;
   char           *n = strrchr(i, '/');
   if (n)
   {
      *n++ = 0;
      cidr = atoi(n);
   }
   esp_netif_set_ip4_addr(&info->netmask, (0xFFFFFFFF << (32 - cidr)) >> 24, (0xFFFFFFFF << (32 - cidr)) >> 16, (0xFFFFFFFF << (32 - cidr)) >> 8, (0xFFFFFFFF << (32 - cidr)));
   REVK_ERR_CHECK(esp_netif_str_to_ip4(i, &info->ip));
   if (!gw || !*gw)
      info->gw = info->ip;
   else
      REVK_ERR_CHECK(esp_netif_str_to_ip4(gw, &info->gw));
   free(i);
}
#endif

#ifdef	CONFIG_REVK_WIFI
static void
wifi_next(void)
{
   if (wifi_index < -1)
      return;
   if (xEventGroupGetBits(revk_group) & GROUP_APCONFIG)
      return;
   if (esp_wifi_set_mode(*apssid ? WIFI_MODE_APSTA : WIFI_MODE_STA))
      return;
   int             last = wifi_index;
   wifi_index++;
   if (wifi_index >= sizeof(wifissid) / sizeof(*wifissid) || !*wifissid[wifi_index])
      wifi_index = 0;
   if (last == wifi_index && (xEventGroupGetBits(revk_group) & GROUP_WIFI_TRY))
      return;                   /* No change */
   if (last != wifi_index && last >= 0 && app_command)
      app_command("change", 0, NULL);
   if (last < 0)
      esp_wifi_start();         /* first time */
   ESP_LOGI(TAG, "WIFi [%s]%s %s", wifissid[wifi_index], last == wifi_index ? "" : " (new)", apssid);
   if (last >= 0 || wifi_index || esp_reset_reason() != ESP_RST_DEEPSLEEP || *apssid)
   {
      if (last >= 0 && wifi_index != last && (xEventGroupGetBits(revk_group) & GROUP_WIFI))
         esp_wifi_disconnect();
      wifi_config_t   wifi_config = {};
      if (wifibssid[wifi_index][0] || wifibssid[wifi_index][1] || wifibssid[wifi_index][2])
      {
         memcpy(wifi_config.sta.bssid, wifibssid[wifi_index], sizeof(wifi_config.sta.bssid));
         wifi_config.sta.bssid_set = 1;
      }
      xEventGroupSetBits(revk_group, GROUP_WIFI_TRY);
      wifi_config.sta.channel = wifichan[wifi_index];
      wifi_config.sta.scan_method = ((esp_reset_reason() == ESP_RST_DEEPSLEEP) ? WIFI_FAST_SCAN : WIFI_ALL_CHANNEL_SCAN);
      strncpy((char *)wifi_config.sta.ssid, wifissid[wifi_index], sizeof(wifi_config.sta.ssid));
      strncpy((char *)wifi_config.sta.password, wifipass[wifi_index], sizeof(wifi_config.sta.password));
      REVK_ERR_CHECK(esp_wifi_set_protocol(ESP_IF_WIFI_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR));
      REVK_ERR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
   }
   if (!(xEventGroupGetBits(revk_group) & GROUP_WIFI))
      esp_wifi_connect();
   /* DNS(not per wifi_index, but main, backup and fallback) */
   void            dns(const char *ip, esp_netif_dns_type_t type)
   {
      if (!*ip)
         return;
      char           *i = strdup(ip);
      char           *c = strrchr(i, '/');
      if              (c)
                        *c = 0;
      esp_netif_dns_info_t dns = {};
      if              (!esp_netif_str_to_ip4(i, &dns.ip.u_addr.ip4))
                         dns.ip.type = AF_INET;
      else if         (!esp_netif_str_to_ip6(i, &dns.ip.u_addr.ip6))
                         dns.ip.type = AF_INET6;
      else
      {
         ESP_LOGE(TAG, "Bad DNS IP %s", i);
         return;
      }
      if              (esp_netif_set_dns_info(sta_netif, type, &dns))
                         ESP_LOGE(TAG, "Bad DNS %s", i);
      else
         ESP_LOGI(TAG, "Set DNS IP %s", i);
      free(i);
   }
   /* Static IP(per wifi_index) */
   if (*wifiip[wifi_index])
   {
      esp_netif_dhcpc_stop(sta_netif);
      esp_netif_ip_info_t info = {0,};
      makeip(&info, wifiip[wifi_index], wifigw[wifi_index]);
      REVK_ERR_CHECK(esp_netif_set_ip_info(sta_netif, &info));
      ESP_LOGI(TAG, "Fixed IP %s GW %s", wifiip[wifi_index], wifigw[wifi_index]);
      if (!*wifidns[0])
         dns(wifiip[wifi_index], ESP_NETIF_DNS_MAIN);   /* Fallback to using gateway for DNS */
   } else
      esp_netif_dhcpc_start(sta_netif); /* Dynamic IP */
   dns(wifidns[0], ESP_NETIF_DNS_MAIN);
   dns(wifidns[1], ESP_NETIF_DNS_BACKUP);
   dns(wifidns[2], ESP_NETIF_DNS_FALLBACK);
}
#endif

#ifdef	CONFIG_REVK_MQTT
static          esp_err_t
mqtt_event_handler(esp_mqtt_event_t * event)
{
   switch (event->event_id)
   {
   case MQTT_EVENT_CONNECTED:
      ESP_LOGI(TAG, "MQTT connect");
      slow_connect = 0;
      if (mqttreset)
         revk_restart(NULL, -1);
      xEventGroupClearBits(revk_group, GROUP_MQTT_TRY | GROUP_MQTT_DONE);
      xEventGroupSetBits(revk_group, GROUP_MQTT);
      void            sub(const char *prefix)
      {
         char           *topic;
         if              (asprintf(&topic, "%s/%s/%s/#", prefix, appname, revk_id) < 0)
                            return;
                         esp_mqtt_client_subscribe(mqtt_client, topic, 0);
                         free(topic);
         if              (asprintf(&topic, "%s/%s/*/#", prefix, appname) < 0)
                            return;
                         esp_mqtt_client_subscribe(mqtt_client, topic, 0);
         if              (*hostname)
         {
            if (asprintf(&topic, "%s/%s/%s/#", prefix, appname, hostname) < 0)
               return;
            esp_mqtt_client_subscribe(mqtt_client, topic, 0);
         }
                         free(topic);
      }
      sub(prefixcommand);
      sub(prefixsetting);       /* Version, up */
      revk_state(NULL, "1 ESP32 %s %s", revk_version, revk_id); /* Up */
      /* Info */
      const esp_partition_t *p = esp_ota_get_running_partition();
      wifi_ap_record_t ap = {};
      esp_wifi_sta_get_ap_info(&ap);
      if (!mqttquiet)
         revk_info(NULL, "MQTT%d(%d) %s ota=%u mem=%u %ums log=%u rst=%u", mqtt_index + 1, mqtt_count, p->label, p->size, esp_get_free_heap_size(), portTICK_PERIOD_MS, CONFIG_LOG_DEFAULT_LEVEL, esp_reset_reason());
      if (esp_get_free_heap_size() < 20 * 1024)
         revk_error(TAG, "WARNING LOW MEMORY - OTA MAY FAIL");
      if (app_command)
         app_command("connect", strlen(mqtthost[mqtt_index]), (unsigned char *)mqtthost[mqtt_index]);
      break;
   case MQTT_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "MQTT disconnect");
      if (mqttreset)
         revk_restart("MQTT lost", mqttreset);
      mqtt_count++;
      xEventGroupClearBits(revk_group, GROUP_MQTT | GROUP_MQTT_TRY);
      xEventGroupSetBits(revk_group, GROUP_MQTT_DONE);
      if (app_command)
         app_command("disconnect", strlen(mqtthost[mqtt_index]), (unsigned char *)mqtthost[mqtt_index]);
      break;
   case MQTT_EVENT_DATA:
      {
         const char     *e = NULL;
         int             p;
         for (p = event->topic_len; p && event->topic[p - 1] != '/'; p--);
         char           *tag = malloc(event->topic_len + 1 - p);
         memcpy(tag, event->topic + p, event->topic_len - p);
         tag[event->topic_len - p] = 0;
         char           *value = malloc(event->data_len + 1);
         if (event->data_len)
            memcpy(value, event->data, event->data_len);
         value[event->data_len] = 0;    /* Safe */
         for (p = 0; p < event->topic_len && event->topic[p] != '/'; p++);
         if (p == 7 && !memcmp(event->topic, prefixcommand, p))
            e = revk_command(tag, event->data_len, (const unsigned char *)value);
         else if (p == 7 && !memcmp(event->topic, "setting", p))
            e = (revk_setting(tag, event->data_len, (const unsigned char *)value) ? : "");      /* Returns NULL if OK */
         else
            e = "";
         if (!e || *e)
            revk_error(tag, "Failed %s", e ? : "Unknown");
         free(tag);
         free(value);
      }
      break;
   case MQTT_EVENT_ERROR:
      break;
   default:
      break;
   }
   return ESP_OK;
}
#endif

#ifdef	CONFIG_REVK_MQTT
static void
mqtt_next(void)
{
   if (mqtt_index < -1)
      return;
   int             last = mqtt_index;
   mqtt_index++;
   if (mqtt_index >= sizeof(mqtthost) / sizeof(*mqtthost) || !*mqtthost[mqtt_index])
      mqtt_index = 0;
   ESP_LOGI(TAG, "MQTT [%s]%s", mqtthost[mqtt_index], last == mqtt_index ? "" : " (new)");
   if (last == mqtt_index && mqtt_client)
   {
      esp_mqtt_client_reconnect(mqtt_client);
      return;                   /* No change */
   }
   if (last != mqtt_index && last >= 0 && app_command)
      app_command("change", 0, NULL);
   if (!*mqtthost[mqtt_index] || *mqtthost[mqtt_index] == '-')  /* No MQTT */
      return;
   char           *topic;
   if (asprintf(&topic, "%s/%s/%s", prefixstate, appname, *hostname ? hostname : revk_id) < 0)
      return;
   char           *url;
   if (asprintf(&url, "%s://%s/", *mqttcert[mqtt_index] ? "mqtts" : "mqtt", mqtthost[mqtt_index]) < 0)
   {
      free(topic);
      return;
   }
   esp_mqtt_client_config_t config = {
      .uri = url,
      .lwt_topic = topic,
      .lwt_qos = 1,
      .lwt_retain = 1,
      .lwt_msg_len = 8,
      .lwt_msg = "0 Failed",
      .event_handle = mqtt_event_handler,
      .buffer_size = mqttsize,
      /* .disable_auto_reconnect = true, */
   };
   if (*mqttcert[mqtt_index])
   {
#if 0                           /* When MQTT supports this! */
#ifdef  CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
      if (!strcmp(mqttcert[mqtt_index], "*"))
         config.crt_bundle_attach = esp_crt_bundle_attach;
      else
#endif
#endif
         config.cert_pem = mqttcert[mqtt_index];
   }
   if (*mqttuser[mqtt_index])
      config.username = mqttuser[mqtt_index];
   if (*mqttpass[mqtt_index])
      config.password = mqttpass[mqtt_index];
   if (mqttport[mqtt_index])
      config.port = mqttport[mqtt_index];
   if (!mqtt_client)
      mqtt_client = esp_mqtt_client_init(&config);
   else
   {
      esp_mqtt_client_stop(mqtt_client);
      xEventGroupWaitBits(revk_group, GROUP_MQTT_DONE, false, true, 1000 / portTICK_PERIOD_MS);
      esp_mqtt_set_config(mqtt_client, &config);
   }
   xEventGroupSetBits(revk_group, GROUP_MQTT_TRY);
   esp_mqtt_client_start(mqtt_client);
   free(topic);
   free(url);
}
#endif

static void
ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
#ifdef	CONFIG_REVK_WIFI
   if (event_base == WIFI_EVENT)
   {
      switch (event_id)
      {
      case WIFI_EVENT_AP_START:
         ESP_LOGI(TAG, "AP Start %s%s", apssid, aphide ? " (hidden)" : "");
         break;
      case WIFI_EVENT_STA_START:
         ESP_LOGI(TAG, "STA Start");
         esp_wifi_connect();
         break;
      case WIFI_EVENT_AP_STOP:
         ESP_LOGI(TAG, "AP Stop %s%s", apssid, aphide ? " (hidden)" : "");
         break;
      case WIFI_EVENT_STA_STOP:
         ESP_LOGI(TAG, "STA Stop");
         break;
      case WIFI_EVENT_STA_CONNECTED:
         ESP_LOGI(TAG, "STA Connect");
         slow_connect = esp_timer_get_time() + 300000000LL;     /* If no DHCP && MQTT we disconnect WiFi */
         if (wifireset)
            esp_phy_erase_cal_data_in_nvs();    /* Lets calibrate on boot */
         break;
      case WIFI_EVENT_AP_STACONNECTED:
         ESP_LOGI(TAG, "AP STA Connect");
         break;
      case WIFI_EVENT_STA_DISCONNECTED:
         ESP_LOGI(TAG, "STA Disconnect");
         if (wifireset)
            revk_restart("WiFi lost", wifireset);
         xEventGroupClearBits(revk_group, GROUP_WIFI | GROUP_WIFI_TRY);
         xEventGroupSetBits(revk_group, GROUP_WIFI_DONE);
         wifi_count++;
         break;
      case WIFI_EVENT_AP_STADISCONNECTED:
         ESP_LOGI(TAG, "AP STA Disconnect");
         break;
      case WIFI_EVENT_AP_PROBEREQRECVED:
         ESP_LOGE(TAG, "AP PROBEREQRECVED");
         break;
      default:
         break;
      }
   }
#endif
   if (event_base == IP_EVENT)
   {
      switch (event_id)
      {
      case IP_EVENT_STA_LOST_IP:
         ESP_LOGI(TAG, "Lost IP");
#ifdef	CONFIG_REVK_WIFI
         wifi_next();
#endif
         break;
      case IP_EVENT_STA_GOT_IP:
         ESP_LOGI(TAG, "Got IP");
#ifdef	CONFIG_REVK_MQTT
         if (mqtt_index >= 0 && (!*mqtthost[mqtt_index] || *mqtthost[mqtt_index] == '-'))
            slow_connect = 0;
#endif
#ifdef  CONFIG_REVK_WIFI
         if (wifireset)
            revk_restart(NULL, -1);
#endif
         sntp_stop();
         sntp_init();
#ifdef	CONFIG_REVK_MQTT
         if (mqtt_client)
            esp_mqtt_client_reconnect(mqtt_client);
#endif
#ifdef  CONFIG_REVK_WIFI
         xEventGroupClearBits(revk_group, GROUP_WIFI_TRY | GROUP_WIFI_DONE);
         xEventGroupSetBits(revk_group, GROUP_WIFI);
         if (app_command)
            app_command("wifi", strlen(wifissid[wifi_index]), (unsigned char *)wifissid[wifi_index]);
#endif
         break;
      case IP_EVENT_GOT_IP6:
         ESP_LOGI(TAG, "Got IPv6");
         break;
      }
   }
}

static void
task(void *pvParameters)
{                               /* Main RevK task */
   if (watchdogtime)
      esp_task_wdt_add(NULL);
   pvParameters = pvParameters;
   /* Log if unexpected restart */
   int64_t         tick = 0,
                   blinker = 0;
   while (1)
   {                            /* Idle - some basic checks that all is well... */
      int64_t         now = esp_timer_get_time();
      if (tick < now)
      {                         /* Every second */
         tick += 1000000ULL;
         ESP_LOGD(TAG, "Idle %d.%06d%s", (uint32_t) (now / 1000000LL), (uint32_t) (now % 1000000LL), wdt_test ? " (testing watchdog)" : "");
         if (!wdt_test && watchdogtime)
            esp_task_wdt_reset();
      }
      if (blink && blinker < now)
      {                         /* Every 1 / 10 th second */
         blinker += 100000ULL;
         static uint8_t  lit = 0,
                         count = 0;
         if (count)
            count--;
         else
         {
            uint8_t         on = blink_on,
                            off = blink_off;
#if     defined(CONFIG_REVK_WIFI) || defined(CONFIG_REVK_MQTT)
            if (!on && !off)
               on = off = (revk_offline() ? 6 : 3);
#endif
            lit = 1 - lit;
            count = (lit ? on : off);
            if (count)
               gpio_set_level(blink & 0x3F, lit ^ ((blink & 0x40) ? 1 : 0));
         }
      }
      if (slow_connect && slow_connect < now)
      {
         ESP_LOGI(TAG, "Slow connect, disconnecting");
         slow_connect = 0;
         esp_wifi_disconnect();
      }
      if (restart_time && restart_time < now && !ota_task_id)
      {                         /* Restart */
         if (!restart_reason)
            restart_reason = "Unknown";
         revk_mqtt_close(restart_reason);
         if (app_command)
            app_command("shutdown", strlen(restart_reason), (unsigned char *)restart_reason);
         REVK_ERR_CHECK(nvs_commit(nvs));
         sleep(1);
         esp_restart();
         restart_time = 0;
      }
      if (nvs_time && nvs_time < now)
      {
         REVK_ERR_CHECK(nvs_commit(nvs));
         nvs_time = 0;
      }
#ifdef	CONFIG_REVK_MQTT
      if (xEventGroupGetBits(revk_group) & GROUP_MQTT)
      {                         /* on line */
         lastonline = esp_timer_get_time() + 3000000LL;
#ifdef	CONFIG_REVK_WIFI
         static int      lastch = 0;
         static uint8_t  lastbssid[6];
         static int      lastindex = 0;
         wifi_ap_record_t ap = {
         };
         esp_wifi_sta_get_ap_info(&ap);
         if (lastch != ap.primary || memcmp(lastbssid, ap.bssid, 6) || lastindex != wifi_index)
         {
            if (!mqttquiet)
               revk_info(NULL, "WiFi%d(%d) %02X%02X%02X:%02X%02X%02X %s (%ddB) ch%d%s", wifi_index + 1, wifi_count, ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5], ap.ssid, ap.rssi, ap.primary, ap.country.cc);
            lastindex = wifi_index;
            lastch = ap.primary;
            memcpy(lastbssid, ap.bssid, 6);
         }
#endif
      }
#endif
#ifdef	CONFIG_REVK_WIFI
      if ((xEventGroupGetBits(revk_group) & (GROUP_WIFI
#ifdef	CONFIG_REVK_MQTT
                                             | GROUP_MQTT | GROUP_MQTT_TRY
#endif
#ifdef	CONFIG_REVK_APCONFIG
                                             | GROUP_APCONFIG
#endif
                                             )) == (GROUP_WIFI))
#endif
         mqtt_next();           /* reconnect */
#ifdef	CONFIG_REVK_WIFI
      if (!(xEventGroupGetBits(revk_group) & (GROUP_WIFI | GROUP_WIFI_TRY)))
         wifi_next();
#endif
#ifdef	CONFIG_REVK_APCONFIG
      if (!ap_task_id && ((apgpio && (gpio_get_level(apgpio & 0x3F) ^ (apgpio & 0x40 ? 1 : 0)))
#if     defined(CONFIG_REVK_WIFI) || defined(CONFIG_REVK_MQTT)
                          || (apwait && revk_offline() > apwait)
#endif
#ifdef	CONFIG_REVK_WIFI
                          || !*wifissid[0]
#endif
                          ))
         ap_task_id = revk_task("AP", ap_task, NULL);   /* Start AP mode */
#endif
   }
}

/* External functions */
void
revk_init(app_command_t * app_command_cb)
{                               /* Start the revk task, use __FILE__ and __DATE__ and __TIME__ to set task name and version ID */
#ifdef	CONFIG_REVK_PARTITION_CHECK
   extern const uint8_t part_start[] asm("_binary_partitions_4m_bin_start");
   extern const uint8_t part_end[] asm("_binary_partitions_4m_bin_end");
   /* Check and update partition table - expects some code to stay where it can run, i.e.0x10000, but may clear all settings */
   if ((part_end - part_start) > SPI_FLASH_SEC_SIZE)
   {
      ESP_LOGE(TAG, "Block size error (%d>%d)", part_end - part_start, SPI_FLASH_SEC_SIZE);
      return;
   }
   uint8_t        *mem = malloc(SPI_FLASH_SEC_SIZE);
   if (!mem)
   {
      ESP_LOGE(TAG, "Malloc fail: %d", SPI_FLASH_SEC_SIZE);
      return;
   }
   REVK_ERR_CHECK(spi_flash_read(CONFIG_PARTITION_TABLE_OFFSET, mem, SPI_FLASH_SEC_SIZE));
   if (memcmp(mem, part_start, part_end - part_start))
   {
#ifndef CONFIG_SPI_FLASH_DANGEROUS_WRITE_ALLOWED
#error Set CONFIG_SPI_FLASH_DANGEROUS_WRITE_ALLOWED
#endif
      ESP_LOGI(TAG, "Updating partition table");
      memset(mem, 0, SPI_FLASH_SEC_SIZE);
      memcpy(mem, part_start, part_end - part_start);
      REVK_ERR_CHECK(spi_flash_erase_range(CONFIG_PARTITION_TABLE_OFFSET, SPI_FLASH_SEC_SIZE));
      REVK_ERR_CHECK(spi_flash_write(CONFIG_PARTITION_TABLE_OFFSET, mem, SPI_FLASH_SEC_SIZE));
      esp_restart();
   }
   free(mem);
#endif
   nvs_flash_init();
   nvs_flash_init_partition(TAG);
   const esp_app_desc_t *app = esp_ota_get_app_description();
   if (nvs_open_from_partition(TAG, TAG, NVS_READWRITE, &nvs))
      REVK_ERR_CHECK(nvs_open(TAG, NVS_READWRITE, &nvs));
   /* Fallback if no dedicated partition */
#define str(x) #x
#define snl(n,d)	revk_register(#n,0,0,&n,d,0)
#define s(n,d)		revk_register(#n,0,0,&n,d,0)
#define sa(n,a,d)	revk_register(#n,a,0,&n,d,0)
#define f(n,a,s,d)	revk_register(#n,a,s,&n,d,SETTING_BINARY)
#define	u32(n,d)	revk_register(#n,0,4,&n,str(d),0)
#define	u16(n,a,d)	revk_register(#n,a,2,&n,str(d),0)
#define	i16(n)		revk_register(#n,0,2,&n,0,SETTING_SIGNED)
#define	u8a(n,a,d)	revk_register(#n,a,1,&n,str(d),0)
#define	u8(n,d)		revk_register(#n,0,1,&n,str(d),0)
#define	b(n,d)		revk_register(#n,0,1,&n,str(d),SETTING_BOOLEAN)
#define	s8(n,d)		revk_register(#n,0,1,&n,str(d),SETTING_SIGNED)
#define io(n)		revk_register(#n,0,sizeof(n),&n,"-",SETTING_SET|SETTING_BITFIELD)
#define p(n)		revk_register("prefix"#n,0,0,&prefix##n,#n,0)
#define h(n,l,d)	revk_register(#n,0,l,&n,d,SETTING_BINARY|SETTING_HEX)
   settings
#ifdef	CONFIG_REVK_WIFI
      wifisettings
#endif
#ifdef	CONFIG_REVK_MQTT
      mqttsettings
#endif
#ifdef	CONFIG_REVK_APCONFIG
      apconfigsettings
#endif
#ifdef	CONFIG_REVK_MESH
      meshsettings
#endif
#undef s
#undef snl
#undef sa
#undef f
#undef u32
#undef u16
#undef i16
#undef u8a
#undef u8
#undef b
#undef s8
#undef io
#undef p
#undef str
#undef h
      REVK_ERR_CHECK(nvs_open(app->project_name, NVS_READWRITE, &nvs));
   /* Application specific settings */
   if (!*appname)
      appname = strdup(app->project_name);
   /* Default is from build */
   if (blink)
   {
      gpio_reset_pin(blink & 0x3F);
      gpio_set_direction(blink & 0x3F, GPIO_MODE_OUTPUT);       /* Blinky LED */
   }
#ifdef	CONFIG_REVK_APCONFIG
   if (apgpio)
   {
      gpio_reset_pin(apgpio & 0x3F);
      gpio_set_direction(apgpio & 0x3F, GPIO_MODE_INPUT);       /* AP mode button */
   }
#endif
   restart_time = 0;
   /* If settings change at start up we can ignore. */
   esp_netif_init();
#ifndef	CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
   REVK_ERR_CHECK(esp_tls_set_global_ca_store(LECert, sizeof(LECert)));
#endif
   revk_version = app->version;
   revk_app = appname;
   char           *d = strstr(revk_version, "dirty");
   if (d)
      asprintf((char **)&revk_version, "%.*s%s", d - revk_version, app->version, app->time);
   sntp_setoperatingmode(SNTP_OPMODE_POLL);
   sntp_setservername(0, ntphost);
   setenv("TZ", tz, 1);
   tzset();
   app_command = app_command_cb;
   {                            /* Chip ID from MAC */
      unsigned char   mac[6];
      REVK_ERR_CHECK(esp_efuse_mac_get_default(mac));
      revk_binid = ((mac[0] << 16) + (mac[1] << 8) + mac[2]) ^ ((mac[3] << 16) + (mac[4] << 8) + mac[5]);
      snprintf(revk_id, sizeof(revk_id), "%06X", revk_binid);
   }
   /* WiFi */
   revk_group = xEventGroupCreate();
#ifdef	CONFIG_REVK_WIFI
   wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
#endif
   REVK_ERR_CHECK(esp_event_loop_create_default());
#ifdef	CONFIG_REVK_WIFI
   REVK_ERR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL));
#endif
   REVK_ERR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));
   REVK_ERR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &ip_event_handler, NULL));
   REVK_ERR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, &ip_event_handler, NULL));
#ifdef	CONFIG_REVK_WIFI
   REVK_ERR_CHECK(esp_wifi_init(&cfg));
   REVK_ERR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
   REVK_ERR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
   sta_netif = esp_netif_create_default_wifi_sta();
   ap_netif = esp_netif_create_default_wifi_ap();
   wifi_next();
   if (*apssid)
   {
      wifi_config_t   wifi_config = {0,};
      if (strlen(apssid) >= sizeof(wifi_config.ap.ssid))
      {
         memcpy((char *)wifi_config.ap.ssid, apssid, sizeof(wifi_config.ap.ssid));
         wifi_config.ap.ssid_len = sizeof(wifi_config.ap.ssid);
      } else
      {
         strcpy((char *)wifi_config.ap.ssid, apssid);
         wifi_config.ap.ssid_len = strlen(apssid);
      }
      if (*appass)
      {
         strncpy((char *)wifi_config.ap.password, appass, sizeof(wifi_config.ap.password));
         wifi_config.ap.authmode = WIFI_AUTH_WPA2_WPA3_PSK;
      }
      wifi_config.ap.ssid_hidden = aphide;
      wifi_config.ap.max_connection = 255;
      esp_netif_ip_info_t info = {0,};
      makeip(&info, *apip ? apip : "10.0.0.1/24", NULL);
      REVK_ERR_CHECK(esp_wifi_set_protocol(ESP_IF_WIFI_AP, aplr ? WIFI_PROTOCOL_LR : (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N)));
      REVK_ERR_CHECK(esp_netif_dhcps_stop(ap_netif));
      REVK_ERR_CHECK(esp_netif_set_ip_info(ap_netif, &info));
      REVK_ERR_CHECK(esp_netif_dhcps_start(ap_netif));
      REVK_ERR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
   }
   /* REVK_ERR_CHECK(esp_netif_create_ip6_linklocal(ap_netif)); */
   /* REVK_ERR_CHECK(esp_netif_create_ip6_linklocal(sta_netif)); */
   REVK_ERR_CHECK(esp_wifi_start());
   esp_wifi_connect();
#endif
#ifdef	CONFIF_REVK_WIFI
   /* DHCP */
   char           *id;
   asprintf(&id, "%s-%s", appname, *hostname ? hostname : revk_id);
   esp_netif_set_hostname(sta_netif, id);
   esp_netif_create_ip6_linklocal(sta_netif);
   free(id);
#endif
   /* Watchdog */
   if (watchdogtime)
      esp_task_wdt_init(watchdogtime, true);
   revk_task(TAG, task, NULL);
}

TaskHandle_t
revk_task(const char *tag, TaskFunction_t t, const void *param)
{                               /* General user task make */
   TaskHandle_t    task_id = NULL;
#ifdef REVK_LOCK_CPU1
   xTaskCreatePinnedToCore(t, tag, 8 * 1024, (void *)param, 2, &task_id, 1);
#else
   xTaskCreate(t, tag, 8 * 1024, (void *)param, 2, &task_id);
#endif
   return task_id;
}

#ifdef	CONFIG_REVK_MQTT
/* MQTT reporting */
void
revk_mqtt_ap(const char *prefix, int qos, int retain, const char *tag, const char *fmt, va_list ap)
{                               /* Send formatted mqtt message */
   if (!mqtt_client)
      return;
   char           *topic;
   if (!prefix)
      topic = (char *)tag;      /* Set fixed topic */
   if (asprintf(&topic, tag ? "%s/%s/%s/%s" : "%s/%s/%s", prefix, appname, *hostname ? hostname : revk_id, tag) < 0)
      return;
   char           *buf;
   int             l;
   if ((l = vasprintf(&buf, fmt, ap)) < 0)
   {
      free(topic);
      return;
   }
   ESP_LOGD(TAG, "MQTT publish %s %s", topic ? : "-", buf);
   if (xEventGroupGetBits(revk_group) & GROUP_MQTT)
      esp_mqtt_client_publish(mqtt_client, topic, buf, l, qos, retain);
   free(buf);
   if (topic != tag)
      free(topic);
}
#endif

#ifdef	CONFIG_REVK_MQTT
void
revk_raw(const char *prefix, const char *tag, int len, void *data, int retain)
{                               /* Send raw MQTT message */
   if (!mqtt_client)
      return;
   char           *topic;
   if (!prefix)
      topic = (char *)tag;      /* Set fixed topic */
   else if (asprintf(&topic, tag ? "%s/%s/%s/%s" : "%s/%s/%s", prefix, appname, *hostname ? hostname : revk_id, tag) < 0)
      return;
   if (!topic)
      return;
   ESP_LOGD(TAG, "MQTT publish %s (%d)", topic ? : "-", len);
   if (xEventGroupGetBits(revk_group) & GROUP_MQTT)
      esp_mqtt_client_publish(mqtt_client, topic, data, len, 2, retain);
   if (topic != tag)
      free(topic);
}
#endif

void
revk_state(const char *tag, const char *fmt,...)
{                               /* Send status */
#ifdef	CONFIG_REVK_MQTT
   va_list         ap;
   va_start(ap, fmt);
   revk_mqtt_ap(prefixstate, 1, 1, tag, fmt, ap);
   va_end(ap);
#endif
}

void
revk_event(const char *tag, const char *fmt,...)
{                               /* Send event */
#ifdef	CONFIG_REVK_MQTT
   va_list         ap;
   va_start(ap, fmt);
   revk_mqtt_ap(prefixevent, 0, 0, tag, fmt, ap);
   va_end(ap);
#endif
}

void
revk_error(const char *tag, const char *fmt,...)
{                               /* Send error */
#ifdef	CONFIG_REVK_MQTT
   xEventGroupWaitBits(revk_group,
#ifdef	CONFIG_REVK_WIFI
                       GROUP_WIFI |
#endif
                       GROUP_MQTT, false, true, 20000 / portTICK_PERIOD_MS);
   /* Chance of reporting issues */
   va_list         ap;
   va_start(ap, fmt);
   revk_mqtt_ap(prefixerror, 0, 0, tag, fmt, ap);
   va_end(ap);
#endif
}

void
revk_info(const char *tag, const char *fmt,...)
{                               /* Send info */
#ifdef	CONFIG_REVK_MQTT
   va_list         ap;
   va_start(ap, fmt);
   revk_mqtt_ap(prefixinfo, 0, 0, tag, fmt, ap);
   va_end(ap);
#endif
}

const char     *
revk_restart(const char *reason, int delay)
{                               /* Restart cleanly */
   restart_reason = reason;
   if (delay < 0)
      restart_time = 0;         /* Cancelled */
   else
   {
      restart_time = esp_timer_get_time() + 1000000LL * (int64_t) delay;        /* Reboot now */
      if (app_command)
         app_command("restart", strlen(reason ? : ""), (void *)reason); /* Warn of reset */
   }
   return "";                   /* Done */
}

static          esp_err_t
ota_handler(esp_http_client_event_t * evt)
{
   static int      ota_size = 0;
   static int      ota_running = 0;
   static int      ota_progress = 0;
   static esp_ota_handle_t ota_handle;
   static const esp_partition_t *ota_partition = NULL;
   switch (evt->event_id)
   {
   case HTTP_EVENT_ERROR:
      break;
   case HTTP_EVENT_ON_CONNECTED:
      ota_size = 0;
      if (ota_running)
         esp_ota_end(ota_handle);
      ota_running = 0;
      break;
   case HTTP_EVENT_HEADER_SENT:
      break;
   case HTTP_EVENT_ON_HEADER:
      if (!strcmp(evt->header_key, "Content-Length"))
         ota_size = atoi(evt->header_value);
      break;
   case HTTP_EVENT_ON_DATA:
      if (ota_size)
      {
         int64_t         now = esp_timer_get_time();
         static int64_t  next = 0;
         if (esp_http_client_get_status_code(evt->client) / 100 != 2)
            ota_size = 0;       /* Failed */
         if (!ota_running && ota_size)
         {                      /* Start */
            ota_progress = 0;
            if (!ota_partition)
               ota_partition = esp_ota_get_running_partition();
            ota_partition = esp_ota_get_next_update_partition(ota_partition);
            if (!ota_partition)
            {
               revk_error("upgrade", "No OTA parition available");      /* TODO if running in OTA, boot to factory to allow OTA */
               ota_size = 0;
            } else
            {
               if (REVK_ERR_CHECK(esp_ota_begin(ota_partition, ota_size, &ota_handle)))
               {
                  ota_size = 0;
                  ota_partition = NULL;
               } else
               {
                  revk_info("upgrade", "Loading %d", ota_size);
                  ota_running = 1;
                  next = now + 5000000LL;
               }
            }
         }
         if (ota_running && ota_size)
         {
            if (REVK_ERR_CHECK(esp_ota_write(ota_handle, evt->data, evt->data_len)))
            {
               ota_size = 0;
            } else
            {
               ota_running += evt->data_len;
               int             percent = ota_running * 100 / ota_size;
               if (percent != ota_progress && (percent == 100 || next < now || percent / 10 != ota_progress / 10))
               {
                  revk_info("upgrade", "%3d%%", ota_progress = percent);
                  next = now + 5000000LL;
               }
            }
         }
      }
      break;
   case HTTP_EVENT_ON_FINISH:
      if (!ota_running && esp_http_client_get_status_code(evt->client) / 100 > 3)
         revk_error("Upgrade", "Failed to start %d (%d)", esp_http_client_get_status_code(evt->client), ota_size);
      if (ota_running)
      {
         if (!REVK_ERR_CHECK(esp_ota_end(ota_handle)))
         {
            revk_info("upgrade", "Updated %s %d", ota_partition->label, ota_running - 1);
            esp_ota_set_boot_partition(ota_partition);
            revk_restart("OTA", 5);
         }
      }
      ota_running = 0;
      break;
   case HTTP_EVENT_DISCONNECTED:
      break;
   }
   return ESP_OK;
}

#ifdef	CONFIG_REVK_APCONFIG
static          esp_err_t
ap_get(httpd_req_t * req)
{
   if (httpd_req_get_url_query_len(req))
   {
      char            query[200];
      if (!httpd_req_get_url_query_str(req, query, sizeof(query)))
      {
         {
            char            ssid[33],
                            pass[33];
            if (!httpd_query_key_value(query, "ssid", ssid, sizeof(ssid)) && *ssid && !httpd_query_key_value(query, "pass", pass, sizeof(pass)))
            {
               revk_setting("wifissid", strlen(ssid), ssid);
               revk_setting("wifipass", strlen(pass), pass);
            }
         }
         {
            char            host[129];
            if (!httpd_query_key_value(query, "host", host, sizeof(host)) && *host)
            {
               revk_setting("mqtthost", strlen(host), host);
               revk_setting("mqttuser", 0, NULL);
               revk_setting("mqttpass", 0, NULL);
               revk_setting("mqttcert", 0, NULL);
               revk_setting("mqttport", 0, NULL);
            }
         }
         const char      resp[] = "Done";
         httpd_resp_send(req, resp, strlen(resp));
         xEventGroupSetBits(revk_group, GROUP_APCONFIG_DONE);
         return ESP_OK;
      }
   }
   /* httpd_resp_sendstr_chunk */
   const char      resp[] = "<form><input name=ssid placeholder='SSID'><br/><input name=pass placeholder='Password'></br><input name=host placeholder='MQTT host'></br><input type=submit value='Set'></form>";
   httpd_resp_send(req, resp, strlen(resp));
   return ESP_OK;
}
#endif

#ifdef	CONFIG_REVK_APCONFIG
static void
ap_task(void *pvParameters)
{
   ESP_LOGI(TAG, "AP mode start");
   xEventGroupSetBits(revk_group, GROUP_APCONFIG);
   {                            /* IP */
      esp_netif_ip_info_t info = {
         0,
      };
      if (*apip)
         makeip(&info, apip, NULL);
      else
      {
         IP4_ADDR(&info.ip, 10, revk_binid >> 8, revk_binid, 1);
         info.gw = info.ip;     /* We are the gateway */
         IP4_ADDR(&info.netmask, 255, 255, 255, 0);
      }
      REVK_ERR_CHECK(esp_netif_dhcps_stop(ap_netif));
      REVK_ERR_CHECK(esp_netif_set_ip_info(ap_netif, &info));
      REVK_ERR_CHECK(esp_netif_dhcps_start(ap_netif));
   }
   wifi_config_t   wifi_config = {};
#ifdef	CONFIG_REVK_WIFI
   if (*apssid)
      strncpy((char *)wifi_config.ap.ssid, apssid, sizeof(wifi_config.ap.ssid));
   else
#endif
      snprintf((char *)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid), "%s-10.%d.%d.1", appname, (revk_binid >> 8) & 255, revk_binid & 255);
   wifi_config.ap.max_connection = 255;
   revk_state(NULL, "0 AP mode started %s", wifi_config.ap.ssid);
   sleep(2);                    /* MQTT close cleanly */
#ifdef	CONFIG_REVK_WIFI
   if (xEventGroupGetBits(revk_group) & (GROUP_WIFI | GROUP_WIFI_TRY))
      esp_wifi_disconnect();
#endif
   REVK_ERR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
   REVK_ERR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
   httpd_config_t  config = HTTPD_DEFAULT_CONFIG();
   if (apport)
      config.server_port = apport;
   /* Empty handle to esp_http_server */
   httpd_handle_t  server = NULL;
   if (!httpd_start(&server, &config))
   {
      httpd_uri_t     uri = {
         .uri = "/",
         .method = HTTP_GET,
         .handler = ap_get,
         .user_ctx = NULL
      };
      REVK_ERR_CHECK(httpd_register_uri_handler(server, &uri));
      xEventGroupWaitBits(revk_group, GROUP_APCONFIG_DONE, true, true, (aptime ? : 3600) * 1000LL / portTICK_PERIOD_MS);
      httpd_stop(server);
      lastonline = esp_timer_get_time() + 3000000LL;
   }
   xEventGroupClearBits(revk_group, GROUP_APCONFIG | GROUP_APCONFIG_DONE);
#ifdef	CONFIG_REVK_WIFI
   wifi_next();
#endif
   ESP_LOGI(TAG, "AP mode end");
   ap_task_id = NULL;
   vTaskDelete(NULL);
}
#endif

static void
ota_task(void *pvParameters)
{
   char           *url = pvParameters;
   revk_info("upgrade", "%s", url);
   esp_http_client_config_t config = {
      .url = url,.event_handler = ota_handler,
   };
   /* Set the TLS in case redirect to TLS even if http */
   if (*otacert)
      config.cert_pem = otacert;/* Pinned cert */
   else
#ifdef	CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
      config.crt_bundle_attach = esp_crt_bundle_attach;
#else
      config.use_global_ca_store = true;        /* Global cert */
#endif
   esp_http_client_handle_t client = esp_http_client_init(&config);
   if (!client)
      revk_error("upgrade", "HTTP client failed");
   else
   {
      esp_err_t       err = REVK_ERR_CHECK(esp_http_client_perform(client));
      int             status = esp_http_client_get_status_code(client);
      esp_http_client_cleanup(client);
      free(url);
      if (!err && status / 100 != 2)
         revk_error("upgrade", "HTTP code %d", status);
   }
   ota_task_id = NULL;
   vTaskDelete(NULL);
}

const char     *
revk_ota(const char *url)
{                               /* OTA and restart cleanly */
   if (ota_task_id)
      return "OTA running";
   ota_task_id = revk_task("OTA", ota_task, url);
   return "";
}

static int
nvs_get(setting_t * s, const char *tag, void *data, size_t len)
{                               /* Low level get logic, returns < 0 if error.Calls the right nvs get function for type of setting */
   esp_err_t       err;
   if (s->flags & SETTING_BINARY)
   {
      if ((err = nvs_get_blob(s->nvs, tag, data, &len)) != ERR_OK)
         return -err;
      return len;
   }
   if (s->size == 0)
   {                            /* String */
      if ((err = nvs_get_str(s->nvs, tag, data, &len)) != ERR_OK)
         return -err;
      return len;
   }
   uint64_t        temp;
   if (!data)
      data = &temp;
   if (s->flags & SETTING_SIGNED)
   {
      if (s->size == 8)
      {                         /* int64 */
         if ((err = nvs_get_i64(s->nvs, tag, data)) != ERR_OK)
            return -err;
         return 8;
      }
      if (s->size == 4)
      {                         /* int32 */
         if ((err = nvs_get_i32(s->nvs, tag, data)) != ERR_OK)
            return -err;
         return 4;
      }
      if (s->size == 2)
      {                         /* int16 */
         if ((err = nvs_get_i16(s->nvs, tag, data)) != ERR_OK)
            return -err;
         return 2;
      }
      if (s->size == 1)
      {                         /* int8 */
         if ((err = nvs_get_i8(s->nvs, tag, data)) != ERR_OK)
            return -err;
         return 1;
      }
   } else
   {
      if (s->size == 8)
      {                         /* uint64 */
         if ((err = nvs_get_u64(s->nvs, tag, data)) != ERR_OK)
            return -err;
         return 8;
      }
      if (s->size == 4)
      {                         /* uint32 */
         if ((err = nvs_get_u32(s->nvs, tag, data)) != ERR_OK)
            return -err;
         return 4;
      }
      if (s->size == 2)
      {                         /* uint16 */
         if ((err = nvs_get_u16(s->nvs, tag, data)) != ERR_OK)
            return -err;
         return 2;
      }
      if (s->size == 1)
      {                         /* uint8 */
         if ((err = nvs_get_u8(s->nvs, tag, data)) != ERR_OK)
            return -err;
         return 1;
      }
   }
   return -999;
}

static          esp_err_t
nvs_set(setting_t * s, const char *tag, void *data)
{                               /* Low level get logic, returns < 0 if error.Calls the right nvs get function for type of setting */
   if (s->flags & SETTING_BINARY)
   {
      if (s->size)
         return nvs_set_blob(s->nvs, tag, data, s->size);       /* Fixed */
      return nvs_set_blob(s->nvs, tag, data, 1 + *((unsigned char *)data));     /* Variable */
   }
   if (s->size == 0)
   {
      ESP_LOGD(TAG, "Written %s=%s", tag, (char *)data);
      return nvs_set_str(s->nvs, tag, data);
   }
   if (s->flags & SETTING_SIGNED)
   {
      if (s->size == 8)
         return nvs_set_i64(s->nvs, tag, *((int64_t *) data));
      if (s->size == 4)
         return nvs_set_i32(s->nvs, tag, *((int32_t *) data));
      if (s->size == 2)
         return nvs_set_i16(s->nvs, tag, *((int16_t *) data));
      if (s->size == 1)
         return nvs_set_i8(s->nvs, tag, *((int8_t *) data));
   } else
   {
      if (s->size == 8)
         return nvs_set_u64(s->nvs, tag, *((uint64_t *) data));
      if (s->size == 4)
         return nvs_set_u32(s->nvs, tag, *((uint32_t *) data));
      if (s->size == 2)
         return nvs_set_u16(s->nvs, tag, *((uint16_t *) data));
      if (s->size == 1)
         return nvs_set_u8(s->nvs, tag, *((uint8_t *) data));
   }
   return -1;
}

static const char *
revk_setting_internal(setting_t * s, unsigned int len, const unsigned char *value, unsigned char index, unsigned char flags)
{
   flags |= s->flags;
   void           *data = s->data;
   if (s->array)
   {
      if (index > s->array)
         return "Bad index";
      if (s->array && index > 1 && !(flags & SETTING_BOOLEAN))
         data += (index - 1) * (s->size ? : sizeof(void *));
   }
   if (!value)
      value = (const unsigned char *)"";
   char            tag[16];     /* Max NVS name size */
   if (snprintf(tag, sizeof(tag), s->array ? "%s%u" : "%s", s->name, index ? : 1) >= sizeof(tag))
      return "Setting name too long";
   ESP_LOGD(TAG, "MQTT setting %s (%d)", tag, len);
   char            erase = 0;
   /* Using default, so remove from flash(as defaults may change later, don 't store the default in flash) */
   if (!len && s->defval && !(flags & SETTING_BITFIELD) && index <= 1)
   {                            /* Use default value */
      len = strlen(s->defval);
      value = (const unsigned char *)s->defval;
      if (flags & SETTING_BINARY)
         flags |= SETTING_HEX;
      erase = 1;
   }
   /* Parse new setting */
   unsigned char  *n = NULL;
   int             l = 0;
   if (flags & SETTING_HEX)
   {                            /* Count length */
      int             p = 0;
      while (p < len && !isalnum(value[p]))
         p++;
      /* Separator */
      while (p < len)
      {                         /* get hex length */
         if (!isxdigit(value[p]))
            break;
         p++;
         if (p < len && isxdigit(value[p]))
            p++;                /* Second hex digit in byte */
         while (p < len && !isalnum(value[p]))
            p++;                /* Separator */
         l++;
      }
   } else
      l = len;
   if (flags & SETTING_BINARY)
   {                            /* Blob */
      unsigned char  *o;
      if (!s->size)
      {                         /* Dynamic */
         if (l > 255)
            return "Data too long";
         o = n = malloc(l + 1); /* One byte for length */
         *o++ = l;
         l++;
      } else if (l && l != s->size)
         return "Wrong size";
      else
      {
         o = n = malloc(s->size);
         if (!l)
            memset(n, 0, s->size);      /* Default */
      }
      if (l)
      {
         if (flags & SETTING_HEX)
         {                      /* hex */
            int             p = 0;
            while (p < len && !isalnum(value[p]))
               p++;             /* Separator */
            while (p < len)
            {                   /* store hex length */
               int             v = (isalpha(value[p]) ? 9 : 0) + (value[p] & 15);
               p++;
               if (p < len && isxdigit(value[p]))
               {
                  v = v * 16 + (isalpha(value[p]) ? 9 : 0) + (value[p] & 15);
                  p++;          /* Second hex digit in byte */
               }
               *o++ = v;
               while (p < len && !isalnum(value[p]))
                  p++;          /* Separator */
            }
         } else
            memcpy(o, value, len);      /* Binary */
      }
   } else if (!s->size)
   {                            /* String */
      n = malloc(len + 1);      /* One byte for null termination */
      if (len)
         memcpy(n, value, len);
      n[len] = 0;
      l = len + 1;
   } else
   {                            /* Numeric */
      uint64_t        v = 0;
      if (flags & SETTING_BOOLEAN)
      {                         /* Boolean */
         if (s->size == 1)
            v = *(uint8_t *) data;
         else if (s->size == 2)
            v = *(uint16_t *) data;
         else if (s->size == 4)
            v = *(uint32_t *) data;
         else if (s->size == 8)
            v = *(uint64_t *) data;
         if (len && strchr("YytT1", *value))
         {
            if (s->array && index)
               v |= (1ULL << (index - 1));
            else
               v |= 1;
         } else
         {
            if (s->array && index)
               v &= ~(1ULL << (index - 1));
            else
               v &= ~1;
         }
      } else
      {
         char            neg = 0;
         int             bits = s->size * 8;
         uint64_t        bitfield = 0;
         if (flags & SETTING_SET)
         {                      /* Set top bit if a value is present */
            bits--;
            if (len && value != (const unsigned char *)s->defval)
               bitfield |= (1ULL << bits);      /* Value is set (not so if using default value) */
         }
         if (flags & SETTING_BITFIELD && s->defval)
         {                      /* Bit fields */
            while (len)
            {
               char           *c = strchr(s->defval, *value);
               if (!c)
                  break;
               uint64_t        m = (1ULL << (bits - 1 - (c - s->defval)));
               if (bitfield & m)
                  break;
               bitfield |= m;
               len--;
               value++;
            }
            bits -= strlen(s->defval);
         }
         if (len && bits <= 0)
            return "Extra data on end";
         if (len > 2 && *value == '0' && value[1] == 'x')
         {
            flags |= SETTING_HEX;
            len -= 2;
            value += 2;
         }
         if (len && *value == '-' && (flags & SETTING_SIGNED))
         {                      /* Decimal */
            len--;
            value++;
            neg = 1;
         }
         if (flags & SETTING_HEX)
            while (len && isxdigit(*value))
            {                   /* Hex */
               uint64_t        n = v * 16 + (isalpha(*value) ? 9 : 0) + (*value & 15);
               if (n < v)
                  return "Silly number";
               v = n;
               value++;
               len--;
         } else
            while (len && isdigit(*value))
            {
               uint64_t        n = v * 10 + (*value++ - '0');
               if (n < v)
                  return "Silly number";
               v = n;
               len--;
            }
         if (len)
            return "Bad number";
         if (flags & SETTING_SIGNED)
            bits--;
         if (bits < 0 || (bits < 64 && ((v - (v && neg ? 1 : 0)) >> bits)))
            return "Number too big";
         if (neg)
            v = -v;
         if (flags & SETTING_SIGNED)
            bits++;
         if (bits < 64)
            v &= (1ULL << bits) - 1;
         v |= bitfield;
      }
      if (flags & SETTING_SIGNED)
      {
         if (s->size == 8)
            *((int64_t *) (n = malloc(l = 8))) = v;
         else if (s->size == 4)
            *((int32_t *) (n = malloc(l = 4))) = v;
         else if (s->size == 2)
            *((int16_t *) (n = malloc(l = 2))) = v;
         else if (s->size == 1)
            *((int8_t *) (n = malloc(l = 1))) = v;
      } else
      {
         if (s->size == 8)
            *((int64_t *) (n = malloc(l = 8))) = v;
         else if (s->size == 4)
            *((int32_t *) (n = malloc(l = 4))) = v;
         else if (s->size == 2)
            *((int16_t *) (n = malloc(l = 2))) = v;
         else if (s->size == 1)
            *((int8_t *) (n = malloc(l = 1))) = v;
      }
   }
   if (!n)
      return "Bad setting type";
   /* See if setting has changed */
   int             o = nvs_get(s, tag, NULL, 0);
   if (o < 0 && erase)
      o = 0;
   else if (o != l)
      o = -1;                   /* Different size */
   if (o > 0)
   {
      void           *d = malloc(l);
      if (nvs_get(s, tag, d, l) != o)
      {
         free(n);
         free(d);
         return "Bad setting get";
      }
      if (memcmp(n, d, o))
         o = -1;                /* Different content */
      free(d);
   }
   if (o < 0)
   {                            /* Flash changed */
      if (erase)
         nvs_erase_key(s->nvs, tag);
      else if (nvs_set(s, tag, n) != ERR_OK && (nvs_erase_key(s->nvs, tag) != ERR_OK || nvs_set(s, tag, n) != ERR_OK))
      {
         free(n);
         return "Unable to store";
      }
      if (flags & SETTING_BINARY)
         ESP_LOGD(TAG, "Setting %s changed (%d)", tag, len);
      else
         ESP_LOGD(TAG, "Setting %s changed %.*s", tag, len, value);
      nvs_time = esp_timer_get_time() + 60000000LL;
   }
   if (flags & SETTING_LIVE)
   {                            /* Store changed value in memory live */
      if (!s->size)
      {                         /* Dynamic */
         void           *o = *((void **)data);
         /* See if different */
         if (!o || ((flags & SETTING_BINARY) ? memcmp(o, n, 1 + *(uint8_t *) o) : strcmp(o, (char *)n)))
         {
            *((void **)data) = n;
            if (o)
               free(o);
         } else
            free(n);            /* No change */
      } else
      {                         /* Static (try and make update atomic) */
         if (s->size == 1)
            *(uint8_t *) data = *(uint8_t *) n;
         else if (s->size == 2)
            *(uint16_t *) data = *(uint16_t *) n;
         else if (s->size == 4)
            *(uint32_t *) data = *(uint32_t *) n;
         else if (s->size == 8)
            *(uint64_t *) data = *(uint64_t *) n;
         else
            memcpy(data, n, s->size);
         free(n);
      }
   } else if (o < 0)
      revk_restart("Settings changed", 5);
   return NULL;                 /* OK */
}

const char     *
revk_setting(const char *tag, unsigned int len, const void *value)
{
   unsigned char   flags = 0;
   if (*tag == '0' && tag[1] == 'x')
   {                            /* Store hex */
      flags |= SETTING_HEX;
      tag += 2;
   }
   int             index = 0;
   int             match(setting_t * s)
   {
      const char     *a = s->name;
      const char     *b = tag;
      while           (*a && *a == *b)
      {
         a++;
         b++;
      }
      if              (*a)
                         return 1;      /* not matched whole name, no match */
      if (!*b)
         return 0;              /* Match, no index */
      if (!s->array && *b)
         return 2;              /* not array, and more characters, no match */
      int             v = 0;
      while (isdigit((int)(*b)))
         v = v * 10 + (*b++) - '0';
      if (*b)
         return 3;              /* More on end after any digits, no match */
      if (!v || v > s->array)
         return 4;              /* Invalid index, no match */
      index = v;
      return 0;                 /* Match, index */
   }
   setting_t      *s;
   for (s = setting; s && match(s); s = s->next);
   if (!s)
      return "Unknown setting";
   return revk_setting_internal(s, len, value, index, flags);
}

const char     *
revk_command(const char *tag, unsigned int len, const void *value)
{
   ESP_LOGD(TAG, "MQTT command [%s]", tag);
   const char     *e = NULL;
   /* My commands */
   if (!e && !strcmp(tag, "upgrade"))
   {
      char           *url;      /* TODO, yeh, not freed, but we are rebooting */
      if (len && (!strncmp((char *)value, "https://", 8) || !strncmp((char *)value, "http://", 7)))     /* Yeh allowing http as
                                                                                                         * code is signed anyway */
         url = strdup((char *)value);
      else
         asprintf(&url, "%s://%s/%s.bin",
#ifdef CONFIG_SECURE_SIGNED_ON_UPDATE
                  *otacert ? "https" : "http",
#else
                  "https",      /* If not signed, use https even if no cert pinned */
#endif
                  len ? (char *)value : otahost, appname);
      e = revk_ota(url);
   }
   if (!e && watchdogtime && !strcmp(tag, "watchdog"))
   {                            /* Test watchdog */
      wdt_test = 1;
      return "";
   }
   if (!e && !strcmp(tag, "restart"))
      e = revk_restart("Restart command", 5);
   if (!e && !strcmp(tag, "factory") && len == strlen(revk_id) + strlen(appname) && !strncmp((char *)value, revk_id, strlen(revk_id)) && !strcmp((char *)value + strlen(revk_id), appname))
   {
      esp_err_t       e = nvs_flash_erase();
      if (!e)
         e = nvs_flash_erase_partition(TAG);
      if (e)
         return "Erase failed";
      revk_restart("Factory reset", 5);
      return "";
   }
   if (!e && !strcmp(tag, "uptime"))
   {
      uint64_t        t = esp_timer_get_time();
      revk_info(tag, "%d.%06d", (uint32_t) (t / 1000000LL), (uint32_t) (t % 1000000LL));
      return "";
   }
#ifdef	CONFIG_REVK_APCONFIG
   if (!e && !strcmp(tag, "apconfig") && !ap_task_id)
   {
      ap_task_id = revk_task("AP", ap_task, NULL);
      return "";
   }
#endif
   /* App commands */
   if ((!e || !*e) && app_command)
   {                            /* Pass to app, even if we handled with no error */
      const char     *e2 = app_command(tag, len, value);
      if (e2 && (*e2 || !e))
         e = e2;                /* Overwrite error if we did not have one */
   }
   return e;
}

void
revk_register(const char *name, uint8_t array, uint16_t size, void *data, const char *defval, uint8_t flags)
{                               /* Register setting (not expected to be thread safe, should be called from init) */
   if (flags & SETTING_BITFIELD && !defval)
      ESP_LOGE(TAG, "%s missing defval on bitfield", name);
   else if (flags & SETTING_BITFIELD && !size)
      ESP_LOGE(TAG, "%s missing size on bitfield", name);
   else if (flags & SETTING_BITFIELD && strlen(defval) > 8 * size)
      ESP_LOGE(TAG, "%s too small for bitfield", name); /* TODO other checks, maybe as asserts */
   setting_t      *s;
   for (s = setting; s && strcmp(s->name, name); s = s->next);
   if (s)
      ESP_LOGE(TAG, "%s duplicate", name);
   s = malloc(sizeof(*s));
   s->nvs = nvs;
   s->name = name;
   s->array = array;
   s->size = size;
   s->data = data;
   s->flags = flags;
   s->defval = defval;
   s->next = setting;
   setting = s;
   memset(data, 0, (size ? : sizeof(void *)) * (!(flags & SETTING_BOOLEAN) && array ? array : 1));      /* Initialise memory */
   /* Get value */
   int             get_val(const char *tag, int index)
   {
      void           *data = s->data;
      if              (s->array && index > 1 && !(flags & SETTING_BOOLEAN))
                         data += (s->size ? : sizeof(void *)) * (index - 1);
      int             l = -1;
      if              (!s->size)
      {                         /* Dynamic */
         void           *d = NULL;
                         l = nvs_get(s, tag, NULL, 0);
         if              (l > 1)
         {                      /* 1 byte means zero len or zero terminated so use default */
            d = malloc(l);
            l = nvs_get(s, tag, d, l);
            if (l > 0)
               *((void **)data) = d;
            else
               free(d);         /* Should not happen */
         } else
                            l = -1;     /* default */
      } else
         l = nvs_get(s, tag, data, s->size);    /* Stored static */
      return l;
   }
   const char     *e;
   if (array)
   {                            /* Work through tags */
      int             i;
      for (i = 1; i <= array; i++)
      {
         char            tag[16];       /* NVS tag size */
         if (snprintf(tag, sizeof(tag), "%s%u", s->name, i) < sizeof(tag) && get_val(tag, i) < 0)
         {
            e = revk_setting_internal(s, 0, NULL, i, SETTING_LIVE);     /* Defaulting logic */
            if (e && *e)
               ESP_LOGE(TAG, "Setting %s failed %s", tag, e);
            else
               ESP_LOGD(TAG, "Setting %s created", tag);
         }
      }
   } else
    /* Simple setting, not array */ if (get_val(s->name, 0) < 0)
   {
      e = revk_setting_internal(s, 0, NULL, 0, SETTING_LIVE);   /* Defaulting logic */
      if (e && *e)
         ESP_LOGE(TAG, "Setting %s failed %s", s->name, e);
      else
         ESP_LOGD(TAG, "Setting %s created", s->name);
   }
}

esp_err_t
revk_err_check(esp_err_t e, const char *file, int line)
{
   if (e != ERR_OK)
   {
      ESP_LOGE(TAG, "Error at line %d in %s (%s)", line, file, esp_err_to_name(e));
      revk_error("error", "Error at line %d in %s (%s)", line, file, esp_err_to_name(e));
   }
   return e;
}

#ifdef	CONFIG_REVK_MQTT
const char     *
revk_mqtt(void)
{
   if (mqtt_index < 0)
      return "";
   return mqtthost[mqtt_index];
}
#endif

#ifdef	CONFIG_REVK_WIFI
const char     *
revk_wifi(void)
{
   if (wifi_index < 0)
      return "";
   return wifissid[wifi_index];
}
#endif

void
revk_blink(uint8_t on, uint8_t off)
{
   blink_on = on;
   blink_off = off;
}

#if     defined(CONFIG_REVK_WIFI) || defined(CONFIG_REVK_MQTT)
uint32_t
revk_offline(void)
{                               /* How long off  line */
   if (!lastonline)
      return 1;                 /* Not online yet */
   int64_t         now = esp_timer_get_time();
   if (now < lastonline)
      return 0;
   return (now - lastonline) / 1000000LL;
}
#endif

#ifdef	CONFIG_REVK_MQTT
void
revk_mqtt_close(const char *reason)
{
   if (!mqtt_client)
      return;
   revk_state(NULL, "0 %s", reason);
   mqtt_index = -2;
   //Don 't reconnect
      esp_mqtt_client_stop(mqtt_client);
   xEventGroupWaitBits(revk_group, GROUP_MQTT_DONE, false, true, 1000 / portTICK_PERIOD_MS);
}
#endif

#ifdef	CONFIG_REVK_WIFI
int
revk_wait_wifi(int seconds)
{
   ESP_LOGD(TAG, "Wait WiFi %d", seconds);
   return xEventGroupWaitBits(revk_group, GROUP_WIFI, false, true, seconds * 1000 / portTICK_PERIOD_MS) | GROUP_WIFI;
}
#endif

#ifdef	CONFIG_REVK_MQTT
int
revk_wait_mqtt(int seconds)
{
   ESP_LOGD(TAG, "Wait MQTT %d", seconds);
   return xEventGroupWaitBits(revk_group, GROUP_MQTT, false, true, seconds * 1000 / portTICK_PERIOD_MS) | GROUP_MQTT;
}
#endif
