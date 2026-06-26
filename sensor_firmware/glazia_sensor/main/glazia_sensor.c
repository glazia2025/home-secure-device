/**
 * glazia_sensor.c — Glazia Sensor Node (ESP32-C3)
 *
 * Pairing flow (fresh sensor):
 *   1. Press button on GPIO6 → BLE advertising "GlaziaSensor" (60s)
 *   2. Phone/app writes hub_mac (char 0xFF10) and provision_key hex (char
 * 0xFF11)
 *   3. Save to provisional NVS "glz_prov", stop BLE
 *   4. Init WiFi → ESP-NOW (set PMK), add hub as encrypted peer (LMK =
 * provision_key)
 *   5. Channel-scan and wait for HELLO from hub
 *   6. HELLO → validate MACs, send nonce ACK, wait for COMMIT, promote NVS
 *   7. Start periodic sensor event loop
 *
 * On reboot with "glz_main" NVS:
 *   Skip BLE. Init WiFi/ESP-NOW, add hub as encrypted peer, channel-scan
 *   indefinitely until hub's HELLO re-establishes the link, then send events.
 *
 * On reboot with "glz_prov" NVS (interrupted pairing):
 *   Skip BLE. Retry step 4-6 with the stored provisional credentials.
 *   On timeout: erase "glz_prov" and restart so the user can try again.
 *
 * PMK: compile-time constant — must match hub firmware exactly.
 * LMK: provision_key delivered out-of-band (BLE → server → hub HTTP → ESP-NOW).
 *       Never transmitted over the air; AES-128 encrypts every ESP-NOW frame.
 */

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* NimBLE */
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "SENSOR";

/* ── Constants ──────────────────────────────────────────────────────────────
 */
#define BUTTON_GPIO 6
#define REED_GPIO GPIO_NUM_4
#define REED_EVENT_QUEUE_DEPTH 8
#define REED_DEBOUNCE_MS 50
#define VIBRATION_GPIO             GPIO_NUM_5
#define VIBRATION_EVENT_QUEUE_DEPTH  8
#define VIBRATION_COOLDOWN_MS      2000
#define BLE_ADV_TIMEOUT_MS (60 * 1000)
#define HELLO_PROV_TIMEOUT_MS (2 * 60 * 1000)
#define NVS_MAIN_NS "glz_main"
#define NVS_PROV_NS "glz_prov"
// PMK must match hub firmware's GLAZIA_ESP_NOW_PMK (exactly 16 bytes)
#define GLAZIA_ESP_NOW_PMK "glz!dev.pmk.2024"

#define PKT_HELLO 0x01
#define PKT_ACK 0x02
#define PKT_EVENT 0x03
#define PKT_COMMIT 0x04
#define PKT_RESET 0x05

typedef struct {
  uint8_t type;
  char payload[128];
} __attribute__((packed)) espnow_packet_t;

/* ── Sensor state ───────────────────────────────────────────────────────────
 */
static char s_own_mac_str[18] = {0};
static uint8_t s_hub_mac_bytes[6] = {0};
static uint8_t s_lmk[16] = {0};
static volatile bool s_hub_paired = false;
static volatile bool s_scan_stop = false;
static char s_pair_nonce[17] = {0};
static TaskHandle_t s_ack_task_handle = NULL;
static QueueHandle_t s_reed_event_queue = NULL;
static QueueHandle_t s_vibration_event_queue = NULL;

typedef enum {
  PAIR_WAITING_HELLO = 0,
  PAIR_WAITING_COMMIT,
  PAIR_COMPLETE,
} pairing_state_t;

static volatile pairing_state_t s_pair_state = PAIR_WAITING_HELLO;

typedef struct {
  char hub_mac[18];
  char sensor_mac[18];
  char nonce[17];
  uint8_t hub_channel;
} pending_ack_t;

static pending_ack_t s_pending_ack = {0};

/* ── BLE state ──────────────────────────────────────────────────────────────
 */
static SemaphoreHandle_t s_ble_done_sem = NULL;
static volatile bool s_ble_got_creds = false;
static char s_ble_hub_mac[18] = {0};
static char s_ble_prov_key[33] = {0};
static uint16_t s_ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_hub_mac_val_handle = 0;
static uint16_t s_prov_key_val_handle = 0;

/* ── Helpers ────────────────────────────────────────────────────────────────
 */
static void hex_to_bytes(const char *hex, uint8_t *out, int len) {
  for (int i = 0; i < len; i++) {
    char b[3] = {hex[i * 2], hex[i * 2 + 1], '\0'};
    out[i] = (uint8_t)strtol(b, NULL, 16);
  }
}

static bool mac_str_to_bytes(const char *s, uint8_t *b) {
  int matched = sscanf(s, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &b[0], &b[1], &b[2],
                       &b[3], &b[4], &b[5]);
  if (matched != 6) {
    ESP_LOGW(TAG, "mac_str_to_bytes: invalid MAC (matched %d/6 for '%s')", matched, s);
    memset(b, 0, 6);
    return false;
  }
  return true;
}

static void mac_bytes_to_str(const uint8_t *mac, char *out) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
}

static bool is_valid_mac_format(const char *mac) {
  if (!mac || strlen(mac) != 17) return false;
  for (int i = 0; i < 17; i++) {
    if (i == 2 || i == 5 || i == 8 || i == 11 || i == 14) {
      if (mac[i] != ':') return false;
    } else {
      if (!isxdigit((unsigned char)mac[i])) return false;
    }
  }
  return true;
}

static void normalize_mac_string(char *mac) {
  if (!mac) {
    return;
  }
  for (size_t i = 0; mac[i] != '\0'; i++) {
    mac[i] = (char)toupper((unsigned char)mac[i]);
  }
}

static void mask_prov_key(const char *key, char *out, size_t out_len) {
  size_t len = key ? strlen(key) : 0;
  if (!out || out_len == 0) {
    return;
  }
  if (len >= 8) {
    snprintf(out, out_len, "%.4s...%.4s", key, key + len - 4);
  } else {
    snprintf(out, out_len, "<too-short>");
  }
}

static void log_sensor_identity(const char *context) {
  ESP_LOGI(TAG, "===========================================");
  ESP_LOGI(TAG, "%s", context ? context : "Sensor identity");
  ESP_LOGI(TAG, " Sensor MAC: %s", s_own_mac_str);
  ESP_LOGI(TAG, " Use this MAC in Postman pair_sensor");
  ESP_LOGI(TAG, " BLE name: GlaziaSensor");
  ESP_LOGI(TAG, "===========================================");
}

static const char *pair_state_str(pairing_state_t state) {
  switch (state) {
  case PAIR_WAITING_HELLO:
    return "WAITING_HELLO";
  case PAIR_WAITING_COMMIT:
    return "WAITING_COMMIT";
  case PAIR_COMPLETE:
    return "COMPLETE";
  default:
    return "UNKNOWN";
  }
}

static bool parse_pair_payload(const char *payload, char *hub_mac,
                               size_t hub_mac_len, char *sensor_mac,
                               size_t sensor_mac_len, char *nonce,
                               size_t nonce_len, int *channel) {
  char parsed_hub[18] = {0};
  char parsed_sensor[18] = {0};
  char parsed_nonce[17] = {0};
  int parsed_channel = 0;
  int matched = sscanf(payload, "H:%17[^;];S:%17[^;];N:%16[^;];C:%d",
                       parsed_hub, parsed_sensor, parsed_nonce,
                       &parsed_channel);
  if (matched < 3 || strlen(parsed_hub) != 17 ||
      strlen(parsed_sensor) != 17 || strlen(parsed_nonce) != 16) {
    return false;
  }
  if (hub_mac && hub_mac_len > 0) {
    strncpy(hub_mac, parsed_hub, hub_mac_len - 1);
    hub_mac[hub_mac_len - 1] = '\0';
  }
  if (sensor_mac && sensor_mac_len > 0) {
    strncpy(sensor_mac, parsed_sensor, sensor_mac_len - 1);
    sensor_mac[sensor_mac_len - 1] = '\0';
  }
  if (nonce && nonce_len > 0) {
    strncpy(nonce, parsed_nonce, nonce_len - 1);
    nonce[nonce_len - 1] = '\0';
  }
  if (channel) {
    *channel = (matched == 4) ? parsed_channel : 0;
  }
  return true;
}

/* ── NVS helpers ────────────────────────────────────────────────────────────
 */
static void nvs_save_pair(const char *ns, const char *hub_mac,
                          const char *prov_key) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK)
    return;
  nvs_set_str(h, "hub_mac", hub_mac);
  nvs_set_str(h, "prov_key", prov_key);
  nvs_commit(h);
  nvs_close(h);
  ESP_LOGI(TAG, "[DEV-LOG:REMOVE_BEFORE_PROD] NVS[%s] saved: hub=%s", ns, hub_mac);
}

static bool nvs_load_pair(const char *ns, char *hub_mac, char *prov_key) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK)
    return false;
  size_t len = 18;
  bool ok = (nvs_get_str(h, "hub_mac", hub_mac, &len) == ESP_OK);
  len = 33;
  ok = ok && (nvs_get_str(h, "prov_key", prov_key, &len) == ESP_OK);
  nvs_close(h);
  return ok;
}

static void nvs_erase_ns(const char *ns) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "NVS[%s] erased", ns);
  }
}

/* ── WiFi init ──────────────────────────────────────────────────────────────
 */
static void wifi_init_for_espnow(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                                         WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
  ESP_LOGI(TAG, "WiFi STA started for ESP-NOW");
}

/* ── ESP-NOW ────────────────────────────────────────────────────────────────
 */
static void espnow_send_cb(const esp_now_send_info_t *tx_info,
                           esp_now_send_status_t status) {
  const uint8_t *dest = tx_info ? tx_info->des_addr : NULL;
  char mac_str[18] = {0};
  if (dest && memcmp(dest, s_hub_mac_bytes, 6) == 0 &&
      s_pair_state == PAIR_WAITING_COMMIT) {
    mac_bytes_to_str(dest, mac_str);
    ESP_LOGI(TAG, "Pairing send callback to %s: %s", mac_str,
             status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
    return;
  }

  ESP_LOGD(TAG, "Send %s", status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

static void ack_send_task(void *arg) {
  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    pending_ack_t pending = s_pending_ack;
    espnow_packet_t ack = {.type = PKT_ACK};
    uint8_t current_channel = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    snprintf(ack.payload, sizeof(ack.payload), "H:%s;S:%s;N:%s",
             pending.hub_mac, pending.sensor_mac, pending.nonce);

    esp_wifi_get_channel(&current_channel, &second);
    uint8_t target_channel =
        pending.hub_channel > 0 ? pending.hub_channel : current_channel;
    ESP_LOGI(TAG, "ACK task: locking to ch%d for nonce %s",
             target_channel, pending.nonce);

    vTaskDelay(pdMS_TO_TICKS(450));

    esp_err_t chan_err =
        esp_wifi_set_channel(target_channel, WIFI_SECOND_CHAN_NONE);
    if (chan_err != ESP_OK) {
      ESP_LOGW(TAG, "ACK task: esp_wifi_set_channel(%u) failed: %s",
               target_channel, esp_err_to_name(chan_err));
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    for (int attempt = 1; attempt <= 4; attempt++) {
      esp_err_t err =
          esp_now_send(s_hub_mac_bytes, (uint8_t *)&ack, sizeof(ack));
      if (err == ESP_OK) {
        ESP_LOGI(TAG, "ACK attempt %d/4 queued", attempt);
      } else {
        ESP_LOGW(TAG, "ACK attempt %d/4 failed to queue: %s", attempt,
                 esp_err_to_name(err));
      }

      if (s_pair_state != PAIR_WAITING_COMMIT) {
        ESP_LOGI(TAG, "ACK burst stopped early, pairing state=%s",
                 pair_state_str(s_pair_state));
        break;
      }

      vTaskDelay(pdMS_TO_TICKS(120));
    }
  }
}

static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data,
                           int len) {
  if (len < (int)sizeof(espnow_packet_t))
    return;
  const espnow_packet_t *pkt = (const espnow_packet_t *)data;

  if (pkt->type == PKT_HELLO) {
    char src_mac[18] = {0};
    char hello_hub[18] = {0};
    char hello_sensor[18] = {0};
    char hello_nonce[17] = {0};
    int hub_ch = 0;
    mac_bytes_to_str(info->src_addr, src_mac);

    char safe_payload[128];
    memcpy(safe_payload, pkt->payload, sizeof(safe_payload) - 1);
    safe_payload[127] = '\0';

    if (!parse_pair_payload(safe_payload, hello_hub, sizeof(hello_hub),
                            hello_sensor, sizeof(hello_sensor), hello_nonce,
                            sizeof(hello_nonce), &hub_ch)) {
      ESP_LOGW(TAG, "Malformed HELLO from %s", src_mac);
      return;
    }
    if (strcmp(src_mac, hello_hub) != 0 ||
        strcmp(hello_hub, s_ble_hub_mac) != 0 ||
        strcmp(hello_sensor, s_own_mac_str) != 0) {
      ESP_LOGW(TAG, "HELLO proof mismatch src=%s hub=%s sensor=%s", src_mac,
               hello_hub, hello_sensor);
      return;
    }

    if (s_pair_state != PAIR_WAITING_HELLO) {
      if (strcmp(hello_nonce, s_pair_nonce) == 0) {
        ESP_LOGI(TAG, "Duplicate HELLO ignored for nonce %s while %s",
                 hello_nonce, pair_state_str(s_pair_state));
        return;
      }
      if (s_pair_state == PAIR_COMPLETE || s_pair_state == PAIR_WAITING_COMMIT) {
        ESP_LOGI(TAG, "Hub reconnect: new nonce %s from known hub (was %s) — resetting pairing state",
                 hello_nonce, pair_state_str(s_pair_state));
        s_pair_state = PAIR_WAITING_HELLO;
        s_hub_paired = false;
        // Fall through to process HELLO normally
      } else {
        ESP_LOGW(TAG, "Unexpected HELLO nonce=%s while %s — ignoring", hello_nonce,
                 pair_state_str(s_pair_state));
        return;
      }
    }

    ESP_LOGI(TAG, "HELLO validated from %s for sensor %s nonce=%s ch=%d",
             hello_hub, hello_sensor, hello_nonce, hub_ch);
    strncpy(s_pair_nonce, hello_nonce, sizeof(s_pair_nonce) - 1);
    s_pair_nonce[sizeof(s_pair_nonce) - 1] = '\0';
    s_scan_stop = true;
    s_pair_state = PAIR_WAITING_COMMIT;
    strncpy(s_pending_ack.hub_mac, s_ble_hub_mac, sizeof(s_pending_ack.hub_mac) - 1);
    strncpy(s_pending_ack.sensor_mac, s_own_mac_str, sizeof(s_pending_ack.sensor_mac) - 1);
    strncpy(s_pending_ack.nonce, s_pair_nonce, sizeof(s_pending_ack.nonce) - 1);
    s_pending_ack.hub_mac[sizeof(s_pending_ack.hub_mac) - 1] = '\0';
    s_pending_ack.sensor_mac[sizeof(s_pending_ack.sensor_mac) - 1] = '\0';
    s_pending_ack.nonce[sizeof(s_pending_ack.nonce) - 1] = '\0';
    if (hub_ch != 0 && (hub_ch < 1 || hub_ch > 13)) {
      ESP_LOGW(TAG, "HELLO: invalid channel %d from %s — using current channel", hub_ch, src_mac);
      hub_ch = 0;
    }
    s_pending_ack.hub_channel = hub_ch > 0 ? (uint8_t)hub_ch : 0;
    ESP_LOGI(TAG, "HELLO accepted — stopping scan and scheduling ACK");
    if (s_ack_task_handle) {
      xTaskNotifyGive(s_ack_task_handle);
    }
  } else if (pkt->type == PKT_COMMIT) {
    char src_mac[18] = {0};
    char commit_hub[18] = {0};
    char commit_sensor[18] = {0};
    char commit_nonce[17] = {0};
    mac_bytes_to_str(info->src_addr, src_mac);

    char safe_commit_payload[128];
    memcpy(safe_commit_payload, pkt->payload, sizeof(safe_commit_payload) - 1);
    safe_commit_payload[127] = '\0';

    if (!parse_pair_payload(safe_commit_payload, commit_hub, sizeof(commit_hub),
                            commit_sensor, sizeof(commit_sensor),
                            commit_nonce, sizeof(commit_nonce), NULL)) {
      ESP_LOGW(TAG, "Malformed COMMIT from %s", src_mac);
      return;
    }
    if (strcmp(src_mac, commit_hub) != 0 ||
        strcmp(commit_hub, s_ble_hub_mac) != 0 ||
        strcmp(commit_sensor, s_own_mac_str) != 0 ||
        strcmp(commit_nonce, s_pair_nonce) != 0) {
      ESP_LOGW(TAG, "COMMIT proof mismatch from %s", src_mac);
      return;
    }

    ESP_LOGI(TAG, "COMMIT validated — ESP-NOW pairing confirmed");
    s_pair_state = PAIR_COMPLETE;
    s_hub_paired = true;
    s_scan_stop = true;
  } else if (pkt->type == PKT_RESET) {
    if (memcmp(info->src_addr, s_hub_mac_bytes, 6) != 0) {
      char src_mac[18] = {0};
      mac_bytes_to_str(info->src_addr, src_mac);
      ESP_LOGW(TAG, "PKT_RESET from unknown sender %s — ignoring", src_mac);
      return;
    }
    ESP_LOGW(TAG, "PKT_RESET from hub — erasing NVS and restarting");
    nvs_erase_ns(NVS_MAIN_NS);
    nvs_erase_ns(NVS_PROV_NS);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
  }
}

static void espnow_init_and_add_hub(void) {
  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_set_pmk((const uint8_t *)GLAZIA_ESP_NOW_PMK));
  esp_now_register_send_cb(espnow_send_cb);
  esp_now_register_recv_cb(espnow_recv_cb);
  s_pair_state = PAIR_WAITING_HELLO;
  memset(s_pair_nonce, 0, sizeof(s_pair_nonce));
  memset(&s_pending_ack, 0, sizeof(s_pending_ack));

  if (!s_ack_task_handle) {
    BaseType_t ok = xTaskCreate(ack_send_task, "ack_send", 3072, NULL, 5,
                                &s_ack_task_handle);
    if (ok != pdPASS) {
      ESP_LOGE(TAG, "Failed to create ACK send task");
      abort();
    }
  }

  // Add hub as encrypted peer — must be done BEFORE HELLO arrives
  if (!esp_now_is_peer_exist(s_hub_mac_bytes)) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, s_hub_mac_bytes, 6);
    peer.channel = 0;    // follow current channel
    peer.encrypt = true; // LMK-encrypted link
    memcpy(peer.lmk, s_lmk, 16);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    ESP_LOGI(TAG, "Hub added as encrypted ESP-NOW peer");
  }
}

/* ── Channel scan task ──────────────────────────────────────────────────────
 */
static void channel_scan_task(void *arg) {
  uint8_t ch = 1;
  while (!s_hub_paired && !s_scan_stop) {
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    ch = (ch % 13) + 1;
    vTaskDelay(pdMS_TO_TICKS(400));
  }
  ESP_LOGI(TAG, "Channel scan task stopping");
  vTaskDelete(NULL);
}

/* ── Event task ─────────────────────────────────────────────────────────────
 */
static const char *reed_payload_for_level(int level) {
  return level == 0 ? "door_close" : "door_open";
}

static const char *reed_label_for_level(int level) {
  return level == 0 ? "CLOSED - magnet detected" : "OPEN - no magnet";
}

static void reed_gpio_isr_handler(void *arg) {
  uint32_t event = 1;
  BaseType_t high_task_woken = pdFALSE;

  if (s_reed_event_queue) {
    xQueueSendFromISR(s_reed_event_queue, &event, &high_task_woken);
  }

  if (high_task_woken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

static void vibration_gpio_isr_handler(void *arg) {
  uint32_t event = 1;
  BaseType_t high_task_woken = pdFALSE;
  if (s_vibration_event_queue) {
    xQueueSendFromISR(s_vibration_event_queue, &event, &high_task_woken);
  }
  if (high_task_woken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

static void send_reed_state_event(int reed_level) {
  espnow_packet_t pkt = {.type = PKT_EVENT};
  snprintf(pkt.payload, sizeof(pkt.payload), "%s",
           reed_payload_for_level(reed_level));

  ESP_LOGI(TAG, "REED SEND: GPIO%d=%d, state=%s, payload=%s",
           (int)REED_GPIO, reed_level, reed_label_for_level(reed_level),
           pkt.payload);

  esp_err_t err = esp_now_send(s_hub_mac_bytes, (uint8_t *)&pkt, sizeof(pkt));
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "REED SEND OK: %s queued to hub", pkt.payload);
  } else {
    ESP_LOGW(TAG, "REED SEND FAILED: %s queue error: %s", pkt.payload,
             esp_err_to_name(err));
  }
}

static void send_vibration_event(void) {
  espnow_packet_t pkt = {.type = PKT_EVENT};
  snprintf(pkt.payload, sizeof(pkt.payload), "vibration");
  ESP_LOGI(TAG, "VIBRATION SEND: GPIO%d triggered", (int)VIBRATION_GPIO);
  esp_err_t err = esp_now_send(s_hub_mac_bytes, (uint8_t *)&pkt, sizeof(pkt));
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "VIBRATION SEND OK: vibration queued to hub");
  } else {
    ESP_LOGW(TAG, "VIBRATION SEND FAILED: %s", esp_err_to_name(err));
  }
}

static void event_task(void *arg) {
  uint32_t event;
  int last_state = -1;

  ESP_LOGI(TAG, "REED TASK: waiting for ESP-NOW pairing before sending events");
  while (!s_hub_paired) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }

  last_state = gpio_get_level(REED_GPIO);
  ESP_LOGI(TAG, "REED INITIAL: GPIO%d=%d, state=%s", (int)REED_GPIO,
           last_state, reed_label_for_level(last_state));
  send_reed_state_event(last_state);

  while (1) {
    if (xQueueReceive(s_reed_event_queue, &event, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    vTaskDelay(pdMS_TO_TICKS(REED_DEBOUNCE_MS));
    int state = gpio_get_level(REED_GPIO);
    if (state == last_state) {
      ESP_LOGI(TAG, "REED DEBOUNCE: GPIO%d still %d (%s), no event sent",
               (int)REED_GPIO, state, reed_label_for_level(state));
      continue;
    }

    ESP_LOGI(TAG, "REED CHANGE: GPIO%d %d -> %d, state=%s", (int)REED_GPIO,
             last_state, state, reed_label_for_level(state));
    last_state = state;
    if (s_hub_paired) {
      send_reed_state_event(state);
    } else {
      ESP_LOGW(TAG, "REED CHANGE DROPPED: hub is not paired");
    }
  }
}

static void vibration_task(void *arg) {
  uint32_t event;
  ESP_LOGI(TAG, "VIBRATION TASK: waiting for ESP-NOW pairing before sending events");
  while (!s_hub_paired) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  while (1) {
    if (xQueueReceive(s_vibration_event_queue, &event, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    if (!s_hub_paired) {
      ESP_LOGW(TAG, "VIBRATION DROPPED: hub is not paired");
      continue;
    }
    send_vibration_event();
    /* Drain ISR noise during cooldown to prevent queue fill-up from a single knock */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(VIBRATION_COOLDOWN_MS);
    uint32_t drain;
    while (xTaskGetTickCount() < deadline) {
      xQueueReceive(s_vibration_event_queue, &drain, pdMS_TO_TICKS(50));
    }
  }
}

/* ── BLE (NimBLE) ───────────────────────────────────────────────────────────
 */

static int gatt_write_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
    return 0;

  uint16_t uuid16 = ble_uuid_u16(ctxt->chr->uuid);
  uint16_t data_len = OS_MBUF_PKTLEN(ctxt->om);

  if (uuid16 == 0xFF10) {
    uint16_t n = data_len < 17 ? data_len : 17;
    os_mbuf_copydata(ctxt->om, 0, n, s_ble_hub_mac);
    s_ble_hub_mac[n] = '\0';
    normalize_mac_string(s_ble_hub_mac);
    ESP_LOGI(TAG, "[DEV-LOG:REMOVE_BEFORE_PROD] BLE: hub_mac = %s", s_ble_hub_mac);
  } else if (uuid16 == 0xFF11) {
    uint16_t n = data_len < 32 ? data_len : 32;
    char masked_key[16] = {0};
    os_mbuf_copydata(ctxt->om, 0, n, s_ble_prov_key);
    s_ble_prov_key[n] = '\0';
    mask_prov_key(s_ble_prov_key, masked_key, sizeof(masked_key));
    ESP_LOGI(TAG, "[DEV-LOG:REMOVE_BEFORE_PROD] BLE: prov_key = %s (%d chars)",
             masked_key, n);
  }

  // Trigger when both are valid; enforce MAC format before accepting (E6)
  if (is_valid_mac_format(s_ble_hub_mac) && strlen(s_ble_prov_key) == 32) {
    if (!s_ble_got_creds) {
      ESP_LOGI(TAG, "BLE: valid hub_mac and prov_key received");
    }
    s_ble_got_creds = true;
    s_ble_conn_handle = conn_handle;
    xSemaphoreGive(s_ble_done_sem);
  }

  return 0;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0xFF00),
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = BLE_UUID16_DECLARE(0xFF10),
                    .access_cb = gatt_write_cb,
                    .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                    .val_handle = &s_hub_mac_val_handle,
                },
                {
                    .uuid = BLE_UUID16_DECLARE(0xFF11),
                    .access_cb = gatt_write_cb,
                    .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                    .val_handle = &s_prov_key_val_handle,
                },
                {0},
            },
    },
    {0},
};

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg) {
  switch (event->type) {
  case BLE_GAP_EVENT_CONNECT:
    if (event->connect.status == 0) {
      s_ble_conn_handle = event->connect.conn_handle;
      ESP_LOGI(TAG, "BLE connected: conn_handle=%d",
               event->connect.conn_handle);
    } else {
      ESP_LOGW(TAG, "BLE connection failed: status=%d",
               event->connect.status);
    }
    break;
  case BLE_GAP_EVENT_DISCONNECT:
    ESP_LOGI(TAG, "BLE disconnected: conn_handle=%d reason=%d",
             event->disconnect.conn.conn_handle, event->disconnect.reason);
    if (s_ble_conn_handle == event->disconnect.conn.conn_handle) {
      s_ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    break;
  case BLE_GAP_EVENT_ADV_COMPLETE:
    ESP_LOGI(TAG, "BLE advertising stopped: reason=%d",
             event->adv_complete.reason);
    break;
  default:
    break;
  }

  return 0;
}

static void ble_on_sync(void) {
  uint8_t addr_type;
  ble_hs_id_infer_auto(0, &addr_type);

  struct ble_gap_adv_params adv_params = {0};
  adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
  adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

  struct ble_hs_adv_fields fields = {0};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.name = (uint8_t *)"GlaziaSensor";
  fields.name_len = strlen("GlaziaSensor");
  fields.name_is_complete = 1;

  ble_gap_adv_set_fields(&fields);
  ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params,
                    ble_gap_event_cb, NULL);
  ESP_LOGI(TAG, "BLE advertising as 'GlaziaSensor'");
}

static void ble_host_task(void *param) {
  nimble_port_run(); // blocks until nimble_port_stop()
  nimble_port_freertos_deinit();
}

// Returns true if credentials were received, false on timeout
static bool ble_provision(void) {
  s_ble_got_creds = false;
  memset(s_ble_hub_mac, 0, sizeof(s_ble_hub_mac));
  memset(s_ble_prov_key, 0, sizeof(s_ble_prov_key));
  s_ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;

  nimble_port_init();
  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_gatts_count_cfg(s_gatt_svcs);
  ble_gatts_add_svcs(s_gatt_svcs);
  ble_hs_cfg.sync_cb = ble_on_sync;
  ble_svc_gap_device_name_set("GlaziaSensor");
  nimble_port_freertos_init(ble_host_task);

  // Wait for write callback to give semaphore, or 60s timeout
  bool got = (xSemaphoreTake(s_ble_done_sem,
                             pdMS_TO_TICKS(BLE_ADV_TIMEOUT_MS)) == pdTRUE);

  // Disconnect and stop advertising / BLE host
  ble_gap_adv_stop();
  if (s_ble_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
    ble_gap_terminate(s_ble_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  nimble_port_stop();
  vTaskDelay(pdMS_TO_TICKS(200)); // allow host task to exit cleanly

  return got && s_ble_got_creds;
}

/* ── Button ─────────────────────────────────────────────────────────────────
 */
static void wait_for_button(void) {
  ESP_LOGI(TAG, "Waiting for button on GPIO%d ...", BUTTON_GPIO);
  while (gpio_get_level(BUTTON_GPIO) != 0) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  vTaskDelay(pdMS_TO_TICKS(50)); // debounce
  while (gpio_get_level(BUTTON_GPIO) == 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  ESP_LOGI(TAG, "Button released — starting BLE");
}

/* ── Entry point ────────────────────────────────────────────────────────────
 */
void app_main(void) {
  // NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  ESP_LOGI(TAG, "Reset reason: %d", esp_reset_reason());

  // Own MAC
  uint8_t own_mac[6];
  esp_read_mac(own_mac, ESP_MAC_WIFI_STA);
  snprintf(s_own_mac_str, sizeof(s_own_mac_str),
           "%02X:%02X:%02X:%02X:%02X:%02X", own_mac[0], own_mac[1], own_mac[2],
           own_mac[3], own_mac[4], own_mac[5]);

  log_sensor_identity("Glazia sensor boot");

  // Button GPIO (active-low, internal pull-up)
  gpio_config_t io = {
      .pin_bit_mask = (1ULL << BUTTON_GPIO),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  gpio_config(&io);

  s_reed_event_queue = xQueueCreate(REED_EVENT_QUEUE_DEPTH, sizeof(uint32_t));
  if (!s_reed_event_queue) {
    ESP_LOGE(TAG, "Failed to create reed event queue");
    abort();
  }

  s_vibration_event_queue = xQueueCreate(VIBRATION_EVENT_QUEUE_DEPTH, sizeof(uint32_t));
  if (!s_vibration_event_queue) {
    ESP_LOGE(TAG, "Failed to create vibration event queue");
    abort();
  }

  gpio_config_t reed_io = {
      .pin_bit_mask = (1ULL << REED_GPIO),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_ANYEDGE,
  };
  ESP_ERROR_CHECK(gpio_config(&reed_io));
  ESP_ERROR_CHECK(gpio_install_isr_service(0));
  ESP_ERROR_CHECK(gpio_isr_handler_add(REED_GPIO, reed_gpio_isr_handler, NULL));
  int reed_boot_level = gpio_get_level(REED_GPIO);
  ESP_LOGI(TAG, "REED SETUP: GPIO%d input with internal pull-up, interrupt on both edges",
           (int)REED_GPIO);
  ESP_LOGI(TAG, "REED SETUP: initial GPIO%d=%d, state=%s", (int)REED_GPIO,
           reed_boot_level, reed_label_for_level(reed_boot_level));

  gpio_config_t vib_io = {
      .pin_bit_mask = (1ULL << VIBRATION_GPIO),
      .mode         = GPIO_MODE_INPUT,
      .pull_up_en   = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type    = GPIO_INTR_POSEDGE,
  };
  ESP_ERROR_CHECK(gpio_config(&vib_io));
  ESP_ERROR_CHECK(gpio_isr_handler_add(VIBRATION_GPIO, vibration_gpio_isr_handler, NULL));
  ESP_LOGI(TAG, "VIBRATION SETUP: GPIO%d input, POSEDGE interrupt (no pull resistor)",
           (int)VIBRATION_GPIO);

  s_ble_done_sem = xSemaphoreCreateBinary();

  // ── Determine boot state ──────────────────────────────────────────────
  char hub_mac_str[18] = {0};
  char prov_key_hex[33] = {0};

  bool has_main = nvs_load_pair(NVS_MAIN_NS, hub_mac_str, prov_key_hex);
  bool has_prov = false;
  bool is_fresh = false;

  if (!has_main) {
    has_prov = nvs_load_pair(NVS_PROV_NS, hub_mac_str, prov_key_hex);
  }
  if (!has_main && !has_prov) {
    is_fresh = true;
  }

  if (is_fresh) {
    // ── Fresh sensor: button → BLE → provisional NVS ──────────────────
    ESP_LOGI(TAG, "Fresh sensor — waiting for button + BLE provisioning");
    wait_for_button();

    bool got = ble_provision();
    if (!got) {
      ESP_LOGW(TAG, "BLE timed out — restarting");
      esp_restart();
    }

    strncpy(hub_mac_str, s_ble_hub_mac, sizeof(hub_mac_str) - 1);
    strncpy(prov_key_hex, s_ble_prov_key, sizeof(prov_key_hex) - 1);
    normalize_mac_string(hub_mac_str);
    nvs_save_pair(NVS_PROV_NS, hub_mac_str, prov_key_hex);
    has_prov = true;
  }

  // ── Convert credentials ───────────────────────────────────────────────
  normalize_mac_string(hub_mac_str);
  strncpy(s_ble_hub_mac, hub_mac_str, sizeof(s_ble_hub_mac) - 1);
  s_ble_hub_mac[sizeof(s_ble_hub_mac) - 1] = '\0';
  if (!mac_str_to_bytes(hub_mac_str, s_hub_mac_bytes)) {
    ESP_LOGE(TAG, "Invalid hub MAC format in storage — clearing NVS and restarting");
    nvs_erase_ns(NVS_MAIN_NS);
    nvs_erase_ns(NVS_PROV_NS);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
  }
  hex_to_bytes(prov_key_hex, s_lmk, 16);
  ESP_LOGI(TAG, "[DEV-LOG:REMOVE_BEFORE_PROD] Hub MAC: %s  LMK: loaded", hub_mac_str);

  // ── WiFi + ESP-NOW ────────────────────────────────────────────────────
  wifi_init_for_espnow();
  espnow_init_and_add_hub();

  if (has_main) {
    // Paired — channel-scan indefinitely until hub's HELLO re-establishes link
    ESP_LOGI(TAG, "Main pairing — scanning for hub HELLO...");
    xTaskCreate(channel_scan_task, "ch_scan", 2048, NULL, 4, NULL);
    xTaskCreate(event_task, "events", 3072, NULL, 5, NULL);
    xTaskCreate(vibration_task, "vib_evts", 3072, NULL, 5, NULL);
    // event_task / vibration_task will skip sends until s_hub_paired=true (set in recv_cb)

  } else {
    // Provisional — must receive COMMIT within the local pairing timeout.
    ESP_LOGI(TAG, "Provisional — waiting up to %d ms for COMMIT from hub", HELLO_PROV_TIMEOUT_MS);
    xTaskCreate(channel_scan_task, "ch_scan", 2048, NULL, 4, NULL);

    int waited = 0;
    while (!s_hub_paired && waited < HELLO_PROV_TIMEOUT_MS) {
      vTaskDelay(pdMS_TO_TICKS(500));
      waited += 500;
    }

    s_scan_stop = true;

    if (s_hub_paired) {
      // COMMIT received — promote provisional credentials to main storage.
      nvs_save_pair(NVS_MAIN_NS, hub_mac_str, prov_key_hex);
      nvs_erase_ns(NVS_PROV_NS);
      memset(prov_key_hex, 0, sizeof(prov_key_hex));
      ESP_LOGI(TAG, "Pairing confirmed — promoted to main NVS");
      xTaskCreate(event_task, "events", 3072, NULL, 5, NULL);
      xTaskCreate(vibration_task, "vib_evts", 3072, NULL, 5, NULL);
    } else {
      // Hub never responded — roll back
      ESP_LOGE(TAG, "Pairing timeout in state %s — clearing provisional NVS and restarting",
               pair_state_str(s_pair_state));
      nvs_erase_ns(NVS_PROV_NS);
      vTaskDelay(pdMS_TO_TICKS(1000));
      esp_restart();
    }
  }
}
