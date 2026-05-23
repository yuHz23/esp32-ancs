// ================================================================
//  ESP32-S3 ANCS Bridge  —  GIAI DOAN 3 (WiFi + POST server)
//  Doc thong bao iPhone qua ANCS -> POST JSON len Noti Bridge
//  dashboard (D:\noti-bridge, /notification, X-Auth-Token).
//  Thay nguon Android Z5 = thay SePay.
//
//  Luong: iPhone (GATT server ANCS) -> ESP32 (peripheral, mo GATT
//  client tren link san co, bond) -> nhan notif -> day vao FreeRTOS
//  queue (tu BT task) -> loop() (main task) POST qua WiFi.
//  Tach queue de HTTP KHONG block BT stack.
//
//  Toolchain: Arduino core 2.0.17 (Bluedroid), framework=arduino.
//  C++: KHONG dung designated initializer.
// ================================================================
#include <Arduino.h>
#include <string.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatt_defs.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// =====================  CONFIG (khong co secret) =====================
#define DEVICE_NAME   "BankBridge"          // ten hien tren iPhone khi pair
#define DEVICE_ID     "esp32-ancs-iphone"   // gui kem moi notif
#define FORWARD_ALL   1                      // 1=day moi notif ; 0=chi app bank
#define AP_SSID       "BankBridge-Setup"     // ten hotspot cau hinh lan dau
#define FACTORY_BTN   0                       // GPIO0 (nut BOOT): giu 3s = factory reset
#define LOCAL_MTU     500
#define INVALID_HANDLE 0
#define GATTS_APP_ID  1

static const char* TAG = "ANCS";

// Cau hinh runtime (luu NVS qua Preferences) - dien qua hotspot lan dau
static String      g_ssid, g_pass, g_serverUrl, g_token;
static bool        g_setupMode = false;
static Preferences prefs;
static WebServer   webServer(80);
static DNSServer   dnsServer;

// ---------------- ANCS UUID (little-endian 16 byte) ----------------
static uint8_t ANCS_SVC_UUID[16]   = {0xD0,0x00,0x2D,0x12,0x1E,0x4B,0x0F,0xA4,0x99,0x4E,0xCE,0xB5,0x31,0xF4,0x05,0x79};
static uint8_t NOTIF_SRC_UUID[16]  = {0xbd,0x1d,0xa2,0x99,0xe6,0x25,0x58,0x8c,0xd9,0x42,0x01,0x63,0x0d,0x12,0xbf,0x9f};
static uint8_t CTRL_PT_UUID[16]    = {0xd9,0xd9,0xaa,0xfd,0xbd,0x9b,0x21,0x98,0xa8,0x49,0xe1,0x45,0xf3,0xd8,0xd1,0x69};
static uint8_t DATA_SRC_UUID[16]   = {0xfb,0x7b,0x7c,0xce,0x6a,0xb3,0x44,0xbe,0xb5,0x4b,0xd6,0x24,0xe9,0xc6,0xea,0x22};

// HID service UUID 0x1812 (128-bit) -> "moi" de iOS LIET KE trong Settings de pair.
static uint8_t HID_SVC_UUID128[16] = {0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,0x00,0x10,0x00,0x00,0x12,0x18,0x00,0x00};

// ---------------- ANCS enums ----------------
enum { EventIDNotificationAdded = 0, EventIDNotificationModified = 1, EventIDNotificationRemoved = 2 };
enum { EventFlagSilent = (1<<0), EventFlagImportant = (1<<1), EventFlagPreExisting = (1<<2) };
enum { CategoryIDIncomingCall = 1 };
enum { CommandIDGetNotificationAttributes = 0, CommandIDGetAppAttributes = 1, CommandIDPerformNotificationAction = 2 };
enum { NotiAttr_AppIdentifier = 0, NotiAttr_Title = 1, NotiAttr_Subtitle = 2, NotiAttr_Message = 3,
       NotiAttr_MessageSize = 4, NotiAttr_Date = 5, NotiAttr_PositiveAction = 6, NotiAttr_NegativeAction = 7 };
enum { ActionIDPositive = 0, ActionIDNegative = 1 };

// ---------------- trang thai GATT client ----------------
static esp_gatt_if_t  g_gattc_if   = ESP_GATT_IF_NONE;
static uint16_t       g_conn_id    = 0;
static uint16_t       g_svc_start  = 0;
static uint16_t       g_svc_end    = 0;
static uint16_t       g_ns_handle  = 0;
static uint16_t       g_ds_handle  = 0;
static uint16_t       g_cp_handle  = 0;
static uint16_t       g_mtu        = 23;
static esp_bd_addr_t  g_remote_bda;
static bool           g_found_svc  = false;
static volatile bool  g_connected  = false;

// ---------------- gom packet Data Source ----------------
static uint8_t  g_ds_buf[1024];
static uint16_t g_ds_len = 0;

// ---------------- hang doi notif (BT task -> loop) ----------------
struct NotifMsg {
  char package[80];
  char title[120];
  char message[300];
  char date[24];
  bool bank;
};
static QueueHandle_t g_notifQ = NULL;

// ---------------- GATT server cho Android (NUS) ----------------
// Android KHONG co ANCS -> app companion ghi notification (JSON) vao RX char nay
// qua BLE. ESP nhan -> day vao cung g_notifQ -> POST server (giong duong iOS).
// Nordic UART Service: svc 6E400001-..., RX(write) 6E400002-...
static uint8_t NUS_SVC_UUID[16] = {0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x6e};
static uint8_t NUS_RX_UUID[16]  = {0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x6e};
enum { IDX_SVC, IDX_RX_DECL, IDX_RX_VAL, NUS_IDX_NB };
static uint16_t      g_nus_handles[NUS_IDX_NB];
static esp_gatt_if_t g_gatts_if = ESP_GATT_IF_NONE;
#define GATTS_RX_MAX 512
static uint8_t g_prep_buf[GATTS_RX_MAX];
static int     g_prep_len = 0;
static const uint16_t s_pri_svc_uuid    = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t s_char_decl_uuid  = ESP_GATT_UUID_CHAR_DECLARE;
static const uint8_t  s_char_prop_write = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
// Bang attribute (ordered init - KHONG dung designated initializer cho C++)
static const esp_gatts_attr_db_t gatt_db[NUS_IDX_NB] = {
  // IDX_SVC: primary service decl, value = 128-bit NUS service UUID
  {{ESP_GATT_AUTO_RSP},   {ESP_UUID_LEN_16,  (uint8_t*)&s_pri_svc_uuid,    ESP_GATT_PERM_READ,  16, 16, NUS_SVC_UUID}},
  // IDX_RX_DECL: characteristic declaration (write)
  {{ESP_GATT_AUTO_RSP},   {ESP_UUID_LEN_16,  (uint8_t*)&s_char_decl_uuid,  ESP_GATT_PERM_READ,  1, 1, (uint8_t*)&s_char_prop_write}},
  // IDX_RX_VAL: characteristic value (RX, write) - app Android ghi JSON vao day
  {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_128, NUS_RX_UUID,                  ESP_GATT_PERM_WRITE, GATTS_RX_MAX, 0, NULL}},
};

// ---------------- adv config ----------------
#define ADV_CONFIG_FLAG       (1 << 0)
#define SCAN_RSP_CONFIG_FLAG  (1 << 1)
static uint8_t g_adv_config_done = 0;
static esp_ble_adv_data_t  g_adv_data;
static esp_ble_adv_data_t  g_scan_rsp;
static esp_ble_adv_params_t g_adv_params;

// ================================================================
static bool isBankApp(const char* appId) {
  return !strcmp(appId, "com.VCB.VCBMobile04")            // Vietcombank
      || !strcmp(appId, "com.mbbank.mbmobile")            // MB Bank
      || !strcmp(appId, "com.techcombank.TCBMobile")      // Techcombank
      || !strcmp(appId, "vn.com.tpb.ebanking")            // TPBank
      || !strcmp(appId, "com.acb.acbmobile")              // ACB
      || !strcmp(appId, "com.bidv.smartbanking")          // BIDV
      || !strcmp(appId, "vn.com.agribank.mobilebanking")  // Agribank
      || !strcmp(appId, "com.vib.vib20")                  // VIB
      || !strcmp(appId, "com.sacombank.sb");              // Sacombank
}

// ---------------- ANCS Control Point commands ----------------
static void requestNotificationAttributes(const uint8_t* uid) {
  uint8_t cmd[32];
  uint16_t i = 0;
  cmd[i++] = CommandIDGetNotificationAttributes;
  memcpy(&cmd[i], uid, 4); i += 4;
  cmd[i++] = NotiAttr_AppIdentifier;
  cmd[i++] = NotiAttr_Title;   cmd[i++] = 0x40; cmd[i++] = 0x00;  // <=64
  cmd[i++] = NotiAttr_Message; cmd[i++] = 0xFF; cmd[i++] = 0x00;  // <=255
  cmd[i++] = NotiAttr_Date;
  esp_err_t e = esp_ble_gattc_write_char(g_gattc_if, g_conn_id, g_cp_handle,
                                         i, cmd, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (e) ESP_LOGE(TAG, "write control point fail: 0x%x", e);
}

static void performNotificationAction(const uint8_t* uid, uint8_t action) {
  uint8_t cmd[6];
  cmd[0] = CommandIDPerformNotificationAction;
  memcpy(&cmd[1], uid, 4);
  cmd[5] = action;
  esp_ble_gattc_write_char(g_gattc_if, g_conn_id, g_cp_handle, sizeof(cmd), cmd,
                           ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
}

// ---------------- parse Data Source -> enqueue ----------------
static void parseDataSource(const uint8_t* msg, uint16_t len) {
  if (!msg || len < 5) return;
  if (msg[0] != CommandIDGetNotificationAttributes) return;

  const uint8_t* p = msg + 5;
  int32_t remain = (int32_t)len - 5;

  NotifMsg n;
  memset(&n, 0, sizeof(n));
  while (remain >= 3) {
    uint8_t  attrId = p[0];
    uint16_t alen   = (uint16_t)p[1] | ((uint16_t)p[2] << 8);
    if ((int32_t)alen > remain - 3) break;
    const char* s = (const char*)&p[3];
    switch (attrId) {
      case NotiAttr_AppIdentifier: snprintf(n.package, sizeof(n.package), "%.*s", (int)alen, s); break;
      case NotiAttr_Title:         snprintf(n.title,   sizeof(n.title),   "%.*s", (int)alen, s); break;
      case NotiAttr_Message:       snprintf(n.message, sizeof(n.message), "%.*s", (int)alen, s); break;
      case NotiAttr_Date:          snprintf(n.date,    sizeof(n.date),    "%.*s", (int)alen, s); break;
      default: break;
    }
    p      += 3 + alen;
    remain -= 3 + alen;
  }
  n.bank = isBankApp(n.package);

  Serial.println("\n=========== NOTIFICATION ===========");
  Serial.printf("  App (bundle): %s\n", n.package);
  Serial.printf("  Title:        %s\n", n.title);
  Serial.printf("  Message:      %s\n", n.message);
  Serial.printf("  Date:         %s\n", n.date);
  if (n.bank) Serial.println("  >>> [BANK] THONG BAO NGAN HANG! <<<");
  Serial.println("====================================");

#if FORWARD_ALL
  bool forward = true;
#else
  bool forward = n.bank;
#endif
  if (forward && g_notifQ) {
    if (xQueueSend(g_notifQ, &n, 0) != pdTRUE)
      Serial.println("[Q] queue day - bo qua 1 notif");
  }
}

static bool accumulateDataSource(const uint8_t* v, uint16_t vlen) {
  if (g_ds_len + vlen > sizeof(g_ds_buf)) { g_ds_len = 0; return false; }
  memcpy(&g_ds_buf[g_ds_len], v, vlen);
  g_ds_len += vlen;
  return vlen < (g_mtu - 3);
}

// ================================================================
//  POST 1 notif len server (chay trong loop / main task)
// ================================================================
static void postNotification(const NotifMsg& n) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[POST] bo qua - WiFi chua ket noi");
    return;
  }
  JsonDocument doc;
  doc["deviceId"] = DEVICE_ID;
  doc["package"]  = n.package;       // bundle ID iOS
  doc["title"]    = n.title;
  doc["text"]     = n.message;
  doc["bigText"]  = n.message;       // iOS khong tach big text
  doc["iosDate"]  = n.date;          // server bo qua field la, luu vao raw_json
  String body;
  serializeJson(doc, body);

  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(5000);
  if (!http.begin(client, g_serverUrl)) {
    Serial.println("[POST] http.begin loi");
    return;
  }
  http.addHeader("Content-Type", "application/json; charset=utf-8");
  http.addHeader("X-Auth-Token", g_token);
  int code = http.POST(body);
  if (code > 0) {
    String resp = http.getString();
    Serial.printf("[POST] %d  %s\n", code, resp.c_str());
  } else {
    Serial.printf("[POST] loi: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}

// ================================================================
//  GATTC event handler
// ================================================================
static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t* param) {
  switch (event) {

  case ESP_GATTC_REG_EVT:
    g_gattc_if = gattc_if;
    ESP_LOGI(TAG, "GATTC registered");
    esp_ble_gap_set_device_name(DEVICE_NAME);
    esp_ble_gap_config_local_icon(ESP_BLE_APPEARANCE_GENERIC_HID);
    esp_ble_gap_config_local_privacy(true);
    break;

  case ESP_GATTC_CONNECT_EVT:
    memcpy(g_remote_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
    g_connected = true;
    Serial.println("\n[BLE] *** iPhone DA KET NOI - mo GATT client ***");
    esp_ble_gattc_open(g_gattc_if, g_remote_bda, BLE_ADDR_TYPE_RANDOM, true);
    break;

  case ESP_GATTC_OPEN_EVT:
    if (param->open.status != ESP_GATT_OK) { ESP_LOGE(TAG, "open fail: 0x%x", param->open.status); break; }
    g_conn_id = param->open.conn_id;
    ESP_LOGI(TAG, "OPEN ok conn_id=%d -> bonding", g_conn_id);
    esp_ble_set_encryption(param->open.remote_bda, ESP_BLE_SEC_ENCRYPT_MITM);
    esp_ble_gattc_send_mtu_req(gattc_if, param->open.conn_id);
    break;

  case ESP_GATTC_CFG_MTU_EVT: {
    g_mtu = param->cfg_mtu.mtu;
    ESP_LOGI(TAG, "MTU=%d -> search ANCS", g_mtu);
    esp_bt_uuid_t svc;
    svc.len = ESP_UUID_LEN_128;
    memcpy(svc.uuid.uuid128, ANCS_SVC_UUID, 16);
    esp_ble_gattc_search_service(gattc_if, g_conn_id, &svc);
    break;
  }

  case ESP_GATTC_SEARCH_RES_EVT:
    if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_128) {
      g_svc_start = param->search_res.start_handle;
      g_svc_end   = param->search_res.end_handle;
      g_found_svc = true;
    }
    break;

  case ESP_GATTC_SEARCH_CMPL_EVT: {
    if (!g_found_svc) { ESP_LOGE(TAG, "ANCS service NOT found"); break; }
    uint16_t count = 0;
    esp_ble_gattc_get_attr_count(gattc_if, g_conn_id, ESP_GATT_DB_CHARACTERISTIC,
                                 g_svc_start, g_svc_end, INVALID_HANDLE, &count);
    if (count == 0) break;
    esp_gattc_char_elem_t* chars = (esp_gattc_char_elem_t*)malloc(sizeof(esp_gattc_char_elem_t) * count);
    if (!chars) break;
    uint16_t got = count;
    esp_ble_gattc_get_all_char(gattc_if, g_conn_id, g_svc_start, g_svc_end, chars, &got, 0);
    for (uint16_t i = 0; i < got; i++) {
      if (chars[i].uuid.len != ESP_UUID_LEN_128) continue;
      const uint8_t* u = chars[i].uuid.uuid.uuid128;
      if ((chars[i].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) && memcmp(u, NOTIF_SRC_UUID, 16) == 0) {
        g_ns_handle = chars[i].char_handle;
        esp_ble_gattc_register_for_notify(gattc_if, g_remote_bda, chars[i].char_handle);
        ESP_LOGI(TAG, "found Notification Source");
      } else if ((chars[i].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) && memcmp(u, DATA_SRC_UUID, 16) == 0) {
        g_ds_handle = chars[i].char_handle;
        esp_ble_gattc_register_for_notify(gattc_if, g_remote_bda, chars[i].char_handle);
        ESP_LOGI(TAG, "found Data Source");
      } else if ((chars[i].properties & ESP_GATT_CHAR_PROP_BIT_WRITE) && memcmp(u, CTRL_PT_UUID, 16) == 0) {
        g_cp_handle = chars[i].char_handle;
        ESP_LOGI(TAG, "found Control Point");
      }
    }
    free(chars);
    break;
  }

  case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
    if (param->reg_for_notify.status != ESP_GATT_OK) break;
    uint16_t count = 0;
    esp_ble_gattc_get_attr_count(gattc_if, g_conn_id, ESP_GATT_DB_DESCRIPTOR,
                                 g_svc_start, g_svc_end, param->reg_for_notify.handle, &count);
    if (count == 0) break;
    esp_gattc_descr_elem_t* descrs = (esp_gattc_descr_elem_t*)malloc(sizeof(esp_gattc_descr_elem_t) * count);
    if (!descrs) break;
    uint16_t got = count;
    esp_ble_gattc_get_all_descr(gattc_if, g_conn_id, param->reg_for_notify.handle, descrs, &got, 0);
    uint8_t notify_en[2] = {0x01, 0x00};
    for (uint16_t i = 0; i < got; i++) {
      if (descrs[i].uuid.len == ESP_UUID_LEN_16 && descrs[i].uuid.uuid.uuid16 == ESP_GATT_UUID_CHAR_CLIENT_CONFIG) {
        esp_ble_gattc_write_char_descr(gattc_if, g_conn_id, descrs[i].handle,
                                       sizeof(notify_en), notify_en,
                                       ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
        break;
      }
    }
    free(descrs);
    break;
  }

  case ESP_GATTC_NOTIFY_EVT:
    if (param->notify.handle == g_ns_handle) {
      if (param->notify.value_len < 8) break;
      uint8_t eventId  = param->notify.value[0];
      uint8_t flags    = param->notify.value[1];
      uint8_t category = param->notify.value[2];
      const uint8_t* uid = &param->notify.value[4];
      if (eventId == EventIDNotificationAdded && category == CategoryIDIncomingCall) {
        performNotificationAction(uid, ActionIDNegative);
      } else if (eventId == EventIDNotificationAdded && !(flags & EventFlagPreExisting)) {
        // Bo qua thong bao CU (PreExisting) iOS dump lai moi lan ket noi -> chi lay tin MOI
        requestNotificationAttributes(uid);
      }
    } else if (param->notify.handle == g_ds_handle) {
      if (accumulateDataSource(param->notify.value, param->notify.value_len)) {
        parseDataSource(g_ds_buf, g_ds_len);
        g_ds_len = 0;
      }
    }
    break;

  case ESP_GATTC_WRITE_CHAR_EVT:
    if (param->write.status != ESP_GATT_OK) ESP_LOGW(TAG, "write char status=0x%x", param->write.status);
    break;

  case ESP_GATTC_DISCONNECT_EVT:
    g_connected = false;
    g_found_svc = false;
    g_ns_handle = g_ds_handle = g_cp_handle = 0;
    g_ds_len = 0;
    Serial.printf("\n[BLE] iPhone NGAT (reason 0x%x) - quang cao lai...\n", param->disconnect.reason);
    esp_ble_gap_start_advertising(&g_adv_params);
    break;

  default: break;
  }
}

// ================================================================
//  GAP event handler
// ================================================================
static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
  switch (event) {
  case ESP_GAP_BLE_SET_LOCAL_PRIVACY_COMPLETE_EVT:
    if (esp_ble_gap_config_adv_data(&g_adv_data) == ESP_OK) g_adv_config_done |= ADV_CONFIG_FLAG;
    if (esp_ble_gap_config_adv_data(&g_scan_rsp) == ESP_OK) g_adv_config_done |= SCAN_RSP_CONFIG_FLAG;
    break;
  case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
    g_adv_config_done &= (~ADV_CONFIG_FLAG);
    if (g_adv_config_done == 0) esp_ble_gap_start_advertising(&g_adv_params);
    break;
  case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
    g_adv_config_done &= (~SCAN_RSP_CONFIG_FLAG);
    if (g_adv_config_done == 0) esp_ble_gap_start_advertising(&g_adv_params);
    break;
  case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
    if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
      ESP_LOGE(TAG, "adv start fail: 0x%x", param->adv_start_cmpl.status);
    else
      Serial.println("[OK] Dang quang cao BLE: " DEVICE_NAME);
    break;
  case ESP_GAP_BLE_SEC_REQ_EVT:
    esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
    break;
  case ESP_GAP_BLE_NC_REQ_EVT:
    esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, true);
    break;
  case ESP_GAP_BLE_AUTH_CMPL_EVT:
    if (param->ble_security.auth_cmpl.success) Serial.println("[BLE] *** BONDING THANH CONG ***");
    else Serial.printf("[BLE] bonding FAIL, reason=0x%x\n", param->ble_security.auth_cmpl.fail_reason);
    break;
  default: break;
  }
}

// ================================================================
//  GATT server (Android) — nhan JSON notif tu app companion qua BLE
// ================================================================
static void processAndroidPayload(const uint8_t* data, int len) {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, (const char*)data, (size_t)len);
  if (e) { Serial.printf("[BLE-S] JSON loi: %s\n", e.c_str()); return; }
  NotifMsg n;
  memset(&n, 0, sizeof(n));
  snprintf(n.package, sizeof(n.package), "%s", (const char*)(doc["package"] | ""));
  snprintf(n.title,   sizeof(n.title),   "%s", (const char*)(doc["title"]   | ""));
  const char* t = doc["text"] | "";
  if (!t[0]) t = doc["bigText"] | "";
  snprintf(n.message, sizeof(n.message), "%s", t);
  n.bank = isBankApp(n.package);

  Serial.println("\n===== ANDROID NOTIFICATION (BLE) =====");
  Serial.printf("  package: %s\n  title:   %s\n  text:    %s\n", n.package, n.title, n.message);
  if (n.bank) Serial.println("  >>> [BANK] <<<");
  Serial.println("======================================");

  if (g_notifQ && xQueueSend(g_notifQ, &n, 0) != pdTRUE) Serial.println("[Q] queue day - bo qua");
}

static void gatts_cb(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t* param) {
  switch (event) {
  case ESP_GATTS_REG_EVT:
    g_gatts_if = gatts_if;
    esp_ble_gatts_create_attr_tab(gatt_db, gatts_if, NUS_IDX_NB, 0);
    break;

  case ESP_GATTS_CREAT_ATTR_TAB_EVT:
    if (param->add_attr_tab.status == ESP_GATT_OK && param->add_attr_tab.num_handle == NUS_IDX_NB) {
      memcpy(g_nus_handles, param->add_attr_tab.handles, sizeof(g_nus_handles));
      esp_ble_gatts_start_service(g_nus_handles[IDX_SVC]);
      Serial.println("[OK] GATT server Android (NUS) san sang");
    } else {
      ESP_LOGE(TAG, "create attr tab fail status=0x%x num=%d", param->add_attr_tab.status, param->add_attr_tab.num_handle);
    }
    break;

  case ESP_GATTS_CONNECT_EVT:
    // Co central ket noi -> quang cao lai de nhan them may khac (iPhone + Android song song)
    esp_ble_gap_start_advertising(&g_adv_params);
    break;

  case ESP_GATTS_WRITE_EVT:
    if (param->write.handle == g_nus_handles[IDX_RX_VAL]) {
      if (!param->write.is_prep) {
        processAndroidPayload(param->write.value, param->write.len);
        if (param->write.need_rsp)
          esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
      } else {
        // long write: gom theo offset
        if (param->write.offset + param->write.len <= GATTS_RX_MAX) {
          memcpy(&g_prep_buf[param->write.offset], param->write.value, param->write.len);
          g_prep_len = param->write.offset + param->write.len;
        }
        if (param->write.need_rsp) {
          esp_gatt_rsp_t rsp;
          memset(&rsp, 0, sizeof(rsp));
          rsp.attr_value.handle = param->write.handle;
          rsp.attr_value.offset = param->write.offset;
          rsp.attr_value.len    = param->write.len;
          memcpy(rsp.attr_value.value, param->write.value, param->write.len);
          esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, &rsp);
        }
      }
    } else if (param->write.need_rsp && !param->write.is_prep) {
      esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
    }
    break;

  case ESP_GATTS_EXEC_WRITE_EVT:
    esp_ble_gatts_send_response(gatts_if, param->exec_write.conn_id, param->exec_write.trans_id, ESP_GATT_OK, NULL);
    if (param->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_EXEC && g_prep_len > 0)
      processAndroidPayload(g_prep_buf, g_prep_len);
    g_prep_len = 0;
    break;

  default: break;
  }
}

// ================================================================
static void initAdvStructs() {
  memset(&g_adv_data, 0, sizeof(g_adv_data));
  g_adv_data.set_scan_rsp        = false;
  g_adv_data.include_name        = false;
  g_adv_data.include_txpower     = false;
  g_adv_data.min_interval        = 0x0006;
  g_adv_data.max_interval        = 0x0010;
  g_adv_data.appearance          = ESP_BLE_APPEARANCE_GENERIC_HID;
  g_adv_data.service_uuid_len    = 16;
  g_adv_data.p_service_uuid      = HID_SVC_UUID128;
  g_adv_data.flag                = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);

  memset(&g_scan_rsp, 0, sizeof(g_scan_rsp));
  g_scan_rsp.set_scan_rsp        = true;
  g_scan_rsp.include_name        = true;

  memset(&g_adv_params, 0, sizeof(g_adv_params));
  g_adv_params.adv_int_min       = 0x100;
  g_adv_params.adv_int_max       = 0x100;
  g_adv_params.adv_type          = ADV_TYPE_IND;
  g_adv_params.own_addr_type     = BLE_ADDR_TYPE_RPA_PUBLIC;
  g_adv_params.channel_map       = ADV_CHNL_ALL;
  g_adv_params.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
}

static void initSecurity() {
  esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
  esp_ble_io_cap_t   iocap    = ESP_IO_CAP_NONE;
  uint8_t key_size = 16;
  uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t rsp_key  = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE,      &iocap,    sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE,    &key_size, sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY,    &init_key, sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY,     &rsp_key,  sizeof(uint8_t));
}

// ================================================================
//  PROVISIONING — hotspot cau hinh lan dau + factory reset
// ================================================================
static void loadConfig() {
  prefs.begin("cfg", true);
  g_ssid      = prefs.getString("ssid", "");
  g_pass      = prefs.getString("pass", "");
  g_serverUrl = prefs.getString("server", "");
  g_token     = prefs.getString("token", "");
  prefs.end();
}

static void factoryReset() {
  Serial.println("\n[FACTORY] Xoa cau hinh -> khoi dong lai...");
  prefs.begin("cfg", false);
  prefs.clear();
  prefs.end();
  delay(300);
  ESP.restart();
}

// Giu nut BOOT (GPIO0) >=3s -> factory reset. Goi trong loop().
static void checkFactoryButton() {
  static unsigned long pressStart = 0;
  if (digitalRead(FACTORY_BTN) == LOW) {
    if (pressStart == 0) pressStart = millis();
    else if (millis() - pressStart >= 3000) factoryReset();
  } else {
    pressStart = 0;
  }
}

static void handleRoot() {
  String html =
    "<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>BankBridge Setup</title>"
    "<style>body{font-family:sans-serif;max-width:420px;margin:auto;padding:16px;background:#f4f4f4}"
    "h2{color:#0a7}label{font-weight:bold;font-size:14px}input{width:100%;padding:10px;margin:6px 0 12px;"
    "box-sizing:border-box;border:1px solid #ccc;border-radius:6px}"
    "button{width:100%;padding:13px;background:#0a7;color:#fff;border:0;font-size:16px;border-radius:6px}"
    ".n{color:#888;font-size:12px}</style></head><body>"
    "<h2>&#9881;&#65039; BankBridge &mdash; Cai dat</h2><form method=POST action=/save>"
    "<label>WiFi</label><input list=nets name=ssid placeholder='Chon hoac go ten WiFi' required>"
    "<datalist id=nets>";
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) html += "<option value='" + WiFi.SSID(i) + "'>";
  html +=
    "</datalist>"
    "<label>Mat khau WiFi</label><input name=pass type=password placeholder='De trong neu WiFi mo'>"
    "<label>Server URL</label><input name=server value='http://192.168.1.12:8787/notification'>"
    "<label>Token (X-Auth-Token)</label><input name=token placeholder='dan token server'>"
    "<button type=submit>Luu &amp; Khoi dong lai</button></form>"
    "<p class=n>Giu nut BOOT 3 giay bat cu luc nao de xoa cau hinh (factory reset).</p></body></html>";
  webServer.send(200, "text/html; charset=utf-8", html);
}

static void handleSave() {
  String ssid = webServer.arg("ssid");
  if (ssid.length() == 0) { webServer.send(400, "text/html; charset=utf-8", "Thieu SSID"); return; }
  prefs.begin("cfg", false);
  prefs.putString("ssid",   ssid);
  prefs.putString("pass",   webServer.arg("pass"));
  prefs.putString("server", webServer.arg("server"));
  prefs.putString("token",  webServer.arg("token"));
  prefs.end();
  webServer.send(200, "text/html; charset=utf-8",
    "<!DOCTYPE html><meta charset=utf-8><body style='font-family:sans-serif;text-align:center;padding:48px'>"
    "<h2>&#9989; Da luu!</h2><p>Thiet bi dang khoi dong lai va ket noi WiFi...</p></body>");
  delay(1500);
  ESP.restart();
}

static void startSetupPortal() {
  Serial.println("\n[SETUP] Chua co cau hinh -> bat hotspot cai dat.");
  WiFi.mode(WIFI_AP_STA);          // AP_STA de vua phat hotspot vua scan duoc WiFi
  WiFi.softAP(AP_SSID);
  IPAddress ip = WiFi.softAPIP();
  dnsServer.start(53, "*", ip);    // captive portal: moi domain -> ESP
  webServer.on("/", handleRoot);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.onNotFound(handleRoot);
  webServer.begin();
  Serial.printf("[SETUP] Ket noi WiFi \"%s\" roi mo http://%s\n", AP_SSID, ip.toString().c_str());
}

// ================================================================
void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);

  Serial.println("\n========================================");
  Serial.println("  ESP32-S3 ANCS Bridge - Noti Bridge");
  Serial.println("  (ANCS iOS + GATTS Android)");
  Serial.println("========================================");

  pinMode(FACTORY_BTN, INPUT_PULLUP);
  esp_log_level_set(TAG, ESP_LOG_INFO);

  // NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase(); nvs_flash_init();
  }

  // Doc cau hinh; chua co WiFi -> 1st time setup (hotspot), KHONG init BLE
  loadConfig();
  if (g_ssid.length() == 0) {
    g_setupMode = true;
    startSetupPortal();
    return;
  }

  Serial.printf("  WiFi: %s | Server: %s\n", g_ssid.c_str(), g_serverUrl.c_str());
  Serial.printf("  Forward: %s\n", FORWARD_ALL ? "MOI notif" : "chi app bank");

  g_notifQ = xQueueCreate(16, sizeof(NotifMsg));

  // BT controller -> BLE
  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  if (esp_bt_controller_init(&bt_cfg)) { Serial.println("[ERR] controller init"); return; }
  if (esp_bt_controller_enable(ESP_BT_MODE_BLE)) { Serial.println("[ERR] controller enable"); return; }
  if (esp_bluedroid_init())   { Serial.println("[ERR] bluedroid init"); return; }
  if (esp_bluedroid_enable()) { Serial.println("[ERR] bluedroid enable"); return; }

  esp_ble_gap_register_callback(gap_cb);
  esp_ble_gattc_register_callback(gattc_cb);
  esp_ble_gattc_app_register(0);
  esp_ble_gatts_register_callback(gatts_cb);     // GATT server cho Android
  esp_ble_gatts_app_register(GATTS_APP_ID);
  esp_ble_gatt_set_local_mtu(LOCAL_MTU);

  initAdvStructs();
  initSecurity();

  // WiFi (STA) — PHAI khoi tao SAU khi BT controller da enable, neu khong
  // se abort trong coex_core_enable (xung dot khoi tao coexistence WiFi/BLE).
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);   // BAT BUOC: modem sleep ON khi WiFi+BLE cung chay (1 radio chia se), neu false -> abort
  WiFi.begin(g_ssid.c_str(), g_pass.c_str());
  Serial.printf("[WiFi] dang ket noi \"%s\" ...\n", g_ssid.c_str());

  Serial.println("[OK] Setup xong. iPhone -> Bluetooth -> \"" DEVICE_NAME "\" -> Pair.");
}

void loop() {
  checkFactoryButton();   // giu BOOT 3s = xoa cau hinh

  // ----- Setup mode: chi chay hotspot + web portal -----
  if (g_setupMode) {
    dnsServer.processNextRequest();
    webServer.handleClient();
    delay(5);
    return;
  }

  // ----- Normal mode -----
  static unsigned long lastHb = 0, lastWifiTry = 0;

  // Quan ly WiFi: thu reconnect moi 10s neu rot
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTry >= 10000) {
    lastWifiTry = millis();
    WiFi.reconnect();
  }

  // Rut hang doi -> POST (chay o main task, khong block BT)
  NotifMsg n;
  if (g_notifQ && xQueueReceive(g_notifQ, &n, 0) == pdTRUE) {
    postNotification(n);
  }

  if (millis() - lastHb >= 5000) {
    lastHb = millis();
    Serial.printf("[hb] t=%lus  BLE=%s  WiFi=%s%s  heap=%u\n",
                  millis() / 1000,
                  g_connected ? "CONNECTED" : "advertising",
                  WiFi.status() == WL_CONNECTED ? "OK " : "down",
                  WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "",
                  ESP.getFreeHeap());
  }
  delay(20);
}
