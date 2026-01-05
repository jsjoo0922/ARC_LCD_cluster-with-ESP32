#include <lvgl.h>
#include <TFT_eSPI.h>
#include "esp_heap_caps.h"

#include <SPI.h>
#include "mcp_can.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "ui.h"
#include "ui_normalday.h"
#include "ui_booting.h"
#include "ui_img_coolant.h"
#include "ui_img_scc.h"
#ifdef __cplusplus
}
#endif

// ===== MCP2515 핀 정의 (앞에서 잡은 배선 기준) =====
#define CAN_CS   5     // MCP2515 CS
#define CAN_INT  27    // MCP2515 INT

MCP_CAN CAN0(CAN_CS);



TFT_eSPI tft = TFT_eSPI();

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[240 * 20];

static uint32_t last_ms = 0;

// ===== timers =====
static lv_timer_t* boot_timer   = nullptr;
static bool g_ui_ready = false;
static bool g_can_ok   = false;  // CAN이 정상 초기화되었는지 여부



// ===== SCC state =====
typedef enum {
  SCC_OFF = 0,      // C1
  SCC_READY,        // E1
  SCC_ON,           // E9 (FOLLOWING)
  SCC_OVERRIDE,     // F1 (운전자 가속 개입)
  SCC_LS_CANCEL     // F9 (저속 해제)
} scc_state_t;

static scc_state_t decode_scc_state(uint8_t raw)
{
  switch (raw) {
    case 0xC1: return SCC_OFF;       // SYSTEM OFF
    case 0xE1: return SCC_READY;     // READY
    case 0xE9: return SCC_ON;        // FOLLOWING
    case 0xF1: return SCC_OVERRIDE;  // OVERRIDE (운전자 가속)
    case 0xF9: return SCC_LS_CANCEL; // LOW SPEED CANCEL
    default:   return SCC_OFF;
  }
}

static bool g_scc_override_visible    = true;   // 현재 보여주는 중인지
static unsigned long g_scc_override_ms = 0;     // 마지막 토글 시각


static scc_state_t g_scc_state = SCC_OFF;

// ===== demo values (나중에 CAN 값으로 교체) =====
static int g_speed_kph   = 0;
static int g_coolant_c   = 85;
static int g_scc_set_kph = 120;
static int  g_scc_dist_level = 2;     // 1~3
static bool g_scc_lead_car    = false; // false=전방차량 없음, true=있음

// ================= Display flush =================
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t*)color_p, w * h, true);
  tft.endWrite();

  lv_disp_flush_ready(disp);
}

// ================= Utility: clamp =================
static int clampi(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// ================= Speed UI =================
static void set_speed(int kph) {
  kph = clampi(kph, 0, 260);
  lv_arc_set_value(ui_Arcspeed, kph);

  char buf[8];
  snprintf(buf, sizeof(buf), "%d", kph);
  lv_label_set_text(ui_speedlabel, buf);
}

// ================= Coolant UI =================
static void set_coolant(int coolant_c) {
  coolant_c = clampi(coolant_c, 0, 160);
  lv_arc_set_value(ui_Arccoolant, coolant_c);

  lv_color_t col;
  if (coolant_c >= 105)      col = lv_color_hex(0xFF3B30); // red
  else if (coolant_c >= 100) col = lv_color_hex(0xFFCC00); // yellow
  else                       col = lv_color_hex(0x2BCC30); // green

  lv_obj_set_style_arc_color(ui_Arccoolant, col, LV_PART_INDICATOR | LV_STATE_DEFAULT);
}

// ================= SCC: set speed value sync =================
// 사용자의 정정 요구 반영: 라벨 위치 이동 X, 값만 동기화
static void scc_update_set_speed(int set_kph) {
  set_kph = clampi(set_kph, 0, 260);
  g_scc_set_kph = set_kph;

  // Arc(게이지) 채움 값
  lv_arc_set_value(ui_SCCArcON, set_kph);

  // 라벨(숫자) 표시
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", set_kph);
  lv_label_set_text(ui_SCCsetspeedlabel, buf);
}

// ================= SCC: text color (arc 제외) =================
static void scc_set_text_color_by_state(scc_state_t state)
{
  lv_color_t color;

  switch (state) {
    case SCC_READY:
      // READY: 검정
      color = lv_color_hex(0x000000);
      break;

    case SCC_ON:
    case SCC_OVERRIDE:
      // ON + OVERRIDE: 살짝 어두운 초록
        color = lv_color_hex(0x00CC33);
        break;


    case SCC_LS_CANCEL:
      // LOW SPEED CANCEL: 빨강
      color = lv_color_hex(0xFF0000);
      break;

    case SCC_OFF:
    default:
      // OFF일 때는 어차피 scc_set_visible()에서 숨길 거라 색 변경 안 함
      return;
  }

  if (ui_SCCsettext) {
    lv_obj_set_style_text_color(ui_SCCsettext, color, LV_PART_MAIN | LV_STATE_DEFAULT);
  }
  if (ui_SCCsetspeedlabel) {
    lv_obj_set_style_text_color(ui_SCCsetspeedlabel, color, LV_PART_MAIN | LV_STATE_DEFAULT);
  }
  if (ui_SCCkmhtext) {
    lv_obj_set_style_text_color(ui_SCCkmhtext, color, LV_PART_MAIN | LV_STATE_DEFAULT);
  }
}



static void scc_set_visible(scc_state_t state) {
  bool scc_visible  = (state != SCC_OFF);

  // READY / ON / OVERRIDE / LOW SPEED CANCEL 은 모두 쿨런트 숨김
  bool hide_coolant = (state == SCC_READY ||
                       state == SCC_ON    ||
                       state == SCC_OVERRIDE ||
                       state == SCC_LS_CANCEL);

  // 기어 표시는 NORMAL(SCC_OFF)에서만
  bool show_gear = (state == SCC_OFF);

  // --- SCC 그룹 표시/숨김 ---
  if (scc_visible) {
    lv_obj_clear_flag(ui_SCCsettext,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_SCCsetspeedlabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_SCCkmhtext,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_SCCArcON,         LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(ui_SCCsettext,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_SCCsetspeedlabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_SCCkmhtext,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_SCCArcON,         LV_OBJ_FLAG_HIDDEN);
  }

  // --- Coolant 게이지/아이콘 표시/숨김 ---
  if (hide_coolant) {
    lv_obj_add_flag(ui_Arccoolant,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_coolantimg,  LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(ui_Arccoolant,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_coolantimg,  LV_OBJ_FLAG_HIDDEN);
  }

  // --- Gear 라벨 표시/숨김 ---
  if (ui_gearlabel) {
    if (show_gear) {
      lv_obj_clear_flag(ui_gearlabel, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(ui_gearlabel,   LV_OBJ_FLAG_HIDDEN);
    }
  }
}


// ================= SCC: image table & update =================
// index: [color][lead_car][dist]
// color: 0 = READY, 1 = ON
// lead_car: 0 = 없음, 1 = 있음
// dist: 0,1,2 => d1,d2,d3
static const lv_img_dsc_t* SCC_IMG[2][2][3] = {
    // READY
    {
        { &ui_img_scc_rdy_car0_d1, &ui_img_scc_rdy_car0_d2, &ui_img_scc_rdy_car0_d3 },
        { &ui_img_scc_rdy_car1_d1, &ui_img_scc_rdy_car1_d2, &ui_img_scc_rdy_car1_d3 },
    },
    // ON
    {
        { &ui_img_scc_on_car0_d1, &ui_img_scc_on_car0_d2, &ui_img_scc_on_car0_d3 },
        { &ui_img_scc_on_car1_d1, &ui_img_scc_on_car1_d2, &ui_img_scc_on_car1_d3 },
    }
};

// SCC 상태/거리/전방차량에 맞춰 ui_SCCimg 갱신
static void scc_update_image(void)
{
    if (!ui_SCCimg) return;

    // SCC_OFF 면 이미지 숨김
    if (g_scc_state == SCC_OFF) {
        lv_obj_add_flag(ui_SCCimg, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    int color_idx = (g_scc_state == SCC_ON || g_scc_state == SCC_OVERRIDE) ? 1 : 0;
      // READY / LS_CANCEL 등 → 0 (회색 세트)
      // ON / OVERRIDE → 1 (초록 세트)

    int car_idx   = g_scc_lead_car ? 1 : 0;               // 0=없음, 1=있음
    int dist_idx  = clampi(g_scc_dist_level, 1, 3) - 1;   // 0~2

    const lv_img_dsc_t* img = SCC_IMG[color_idx][car_idx][dist_idx];
    lv_img_set_src(ui_SCCimg, img);
    lv_obj_clear_flag(ui_SCCimg, LV_OBJ_FLAG_HIDDEN);
}

static void scc_set_text_blink_visible(bool visible)
{
  if (ui_SCCsettext) {
    if (visible) lv_obj_clear_flag(ui_SCCsettext, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(ui_SCCsettext,   LV_OBJ_FLAG_HIDDEN);
  }
  if (ui_SCCsetspeedlabel) {
    if (visible) lv_obj_clear_flag(ui_SCCsetspeedlabel, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(ui_SCCsetspeedlabel,   LV_OBJ_FLAG_HIDDEN);
  }
  if (ui_SCCkmhtext) {
    if (visible) lv_obj_clear_flag(ui_SCCkmhtext, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(ui_SCCkmhtext,   LV_OBJ_FLAG_HIDDEN);
  }
}

static void scc_override_blink_task(void)
{
  // OVERRIDE가 아니면 항상 켜진 상태로 복귀
  if (g_scc_state != SCC_OVERRIDE) {
    if (!g_scc_override_visible) {
      g_scc_override_visible = true;
      scc_set_text_blink_visible(true);
    }
    return;
  }

  // OVERRIDE일 때만 0.5초 간격 토글
  unsigned long now = millis();
  if (now - g_scc_override_ms >= 500) {  // 500ms = 0.5초
    g_scc_override_ms = now;
    g_scc_override_visible = !g_scc_override_visible;
    scc_set_text_blink_visible(g_scc_override_visible);
  }
}




// ================= Main text layout by SCC state =================
static void apply_main_text_layout_by_scc_state(scc_state_t state)
{
  if (!ui_speedlabel || !ui_kmhtext || !ui_autoholdindicator) return;

  bool scc_layout = (state == SCC_READY ||
                     state == SCC_ON    ||
                     state == SCC_OVERRIDE ||
                     state == SCC_LS_CANCEL);

  if (scc_layout) {
    // ===== READY/ON 공통 레이아웃 (사용자 기록값) =====
    lv_obj_set_style_text_font(ui_speedlabel, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_pos(ui_speedlabel, 0, 80);

    lv_obj_set_style_text_font(ui_kmhtext, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_pos(ui_kmhtext, 0, 105);

    lv_obj_set_style_text_font(ui_autoholdindicator, &lv_font_montserrat_8, LV_PART_MAIN);
    lv_obj_set_pos(ui_autoholdindicator, 46, 80);

  } else {
    // ===== NORMAL(OFF) 레이아웃도 "수동 고정값"으로 명시 =====
    // 아래 값들은 SquareLine에서 NORMAL 상태 좌표/폰트 그대로 적어주면 됨.
    // (여기가 핵심: 저장값(spd_x0...) 안 쓰고 확정값으로 복귀)

    // 예시: 반드시 사용자가 가진 NORMAL 값으로 교체
    lv_obj_set_style_text_font(ui_speedlabel, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_pos(ui_speedlabel, /*NORMAL_X*/ 0, /*NORMAL_Y*/ 0);

    lv_obj_set_style_text_font(ui_kmhtext, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(ui_kmhtext, /*NORMAL_X*/ 0, /*NORMAL_Y*/ 40);

    lv_obj_set_style_text_font(ui_autoholdindicator, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_pos(ui_autoholdindicator, /*NORMAL_X*/ 65, /*NORMAL_Y*/ 0);
  }
}

// ================= Gear (PRND) 표시 =================

// 디바운스용 상태
static uint8_t g_gear_raw_last       = 0xFF;  // 마지막으로 관측된 raw 값
static uint8_t g_gear_raw_stable     = 0x40;  // 안정된(raw 확정) 값 (기본 P로 시작)
static uint8_t g_gear_same_count     = 0;     // 같은 값이 연속으로 몇 번 들어왔는지

// raw(Byte1) -> 표시 문자열 변환
static const char* gear_text_from_raw(uint8_t raw)
{
  switch (raw) {
    case 0x40: return "P";  // Park
    case 0x47: return "R";  // Reverse
    case 0x46: return "N";  // Neutral
    case 0x45: return "D";  // Drive
    default:   return "-";  // 정의되지 않은 값
  }
}

// 디바운스 포함 업데이트
// ================= PRND 기어 표시 (디바운스 없이 즉시 반영) =================
static void gear_update_from_byte1(uint8_t raw)
{
  if (!ui_gearlabel) return;   // 라벨 포인터 이름이 다르면 여기 맞춰서 수정

  const char* gear_text = "";

  switch (raw) {
    case 0x40:  // P
      gear_text = "P";
      break;
    case 0x47:  // R
      gear_text = "R";
      break;
    case 0x46:  // N
      gear_text = "N";
      break;
    case 0x45:  // D
      gear_text = "D";
      break;
    default:
      // 알 수 없는 값일 때는 원하는 대로:
      // gear_text = "?";  // 혹은 그냥 빈 문자열 유지
      gear_text = "";
      break;
  }

  lv_label_set_text(ui_gearlabel, gear_text);
}




// ================= State setter =================
static void set_scc_state(scc_state_t new_state) {
  if (g_scc_state == new_state) return;
  g_scc_state = new_state;

  // 가시성( + coolant hide ) 반영
  scc_set_visible(g_scc_state);

  // 색상 반영(ready 검정, on 초록)
  scc_set_text_color_by_state(g_scc_state);

  // ON일 때 메인 텍스트 레이아웃 변경
  apply_main_text_layout_by_scc_state(g_scc_state);

  // READY/ON일 때만 set speed 동기화(표시 중이므로)
  if (g_scc_state != SCC_OFF) {
    scc_update_set_speed(g_scc_set_kph);
  }

    // SCC 이미지 갱신
  scc_update_image();
}

// ================= Boot animation timer =================
static void boot_timer_cb(lv_timer_t* t) {
  (void)t;
  static int p = 0;

  p += 2; // 0->100 속도
  if (p > 100) p = 100;

  lv_bar_set_value(ui_loadingbar, p, LV_ANIM_OFF);

  if (p >= 100) {
    // bar 숨기고 normalday로 전환
    lv_obj_add_flag(ui_loadingbar, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load_anim(ui_normalday, LV_SCR_LOAD_ANIM_FADE_IN, 250, 0, false);

    // 부팅 타이머 종료
    lv_timer_del(boot_timer);
    boot_timer = nullptr;

    // normalday 초기 UI 반영
    set_speed(g_speed_kph);
    set_coolant(g_coolant_c);

    // SCCArcON 범위가 0~100으로 되어 있으면 120이 포화되므로, 한 번 맞춰줌
    lv_arc_set_range(ui_SCCArcON, 0, 260);

    // 시작 상태 반영(원하면 SCC_READY로 시작 등 변경 가능)
    set_scc_state(g_scc_state);

     // UI 준비 완료 플래그
    g_ui_ready = true;

  }
}


// ================= CAN 수신 → UI 반영 =================
// ================= CAN 수신 → UI 반영 =================
static void read_can_and_update_ui()
{
  // 한 루프에 최대 5개까지 처리 (필요하면 숫자 조정 가능)
  for (int n = 0; n < 5; ++n) {
    if (CAN0.checkReceive() != CAN_MSGAVAIL) break;

    unsigned long canId = 0;
    byte len = 0;
    byte buf[8];

    CAN0.readMsgBuf(&canId, &len, buf);

    switch (canId) {

      // ===== 0x420: SCC 상태 / 세트속도 / 거리 / 전방차량 =====
      case 0x420: {
        if (len < 7) break;

        uint8_t raw_state = buf[0];
        uint8_t raw_set   = buf[5];
        uint8_t d6        = buf[6];

        // 상태 디코딩
        scc_state_t st = decode_scc_state(raw_state);
        set_scc_state(st);   // 가시성/색/레이아웃/SCC 이미지 기본 처리

        // 세트속도 (raw 그대로 km/h)
        g_scc_set_kph = raw_set;
        scc_update_set_speed(g_scc_set_kph);

        // 거리 단계 (상위 4비트, 1~3)
        int dist = (d6 >> 4) & 0x0F;
        if (dist < 1 || dist > 3) dist = 2;   // 이상값 나오면 가운데(2)
        g_scc_dist_level = dist;

        // 전방차량 유무 (하위 4비트: 0x0D = 있음, 0x0C = 없음)
        uint8_t lowNib = d6 & 0x0F;
        g_scc_lead_car = (lowNib == 0x0D);

        // 거리/전방차량에 맞춰 이미지 갱신
        scc_update_image();
        break;
      }

      // ===== 0x0A0: 속도 + 냉각수 (최유력 후보) =====
      case 0x0A0: {
        // 속도: B4 → km/h (이미 1세대에서 검증했던 바이트)
        if (len >= 5) {
          uint8_t raw_speed = buf[4];   // B4
          g_speed_kph = raw_speed;
          set_speed(g_speed_kph);
        }

        // 냉각수: Byte1 - 40 (확정 식)
        if (len >= 2) {
          uint8_t raw_clt = buf[1];     // Byte1
          int coolant_c = (int)raw_clt - 40;   // CLT[°C] = raw - 40
          g_coolant_c = coolant_c;
          set_coolant(g_coolant_c);
        }
        break;
      }

      // ===== 0x47F: AutoHold 상태 =====
      case 0x47F: {
        if (len >= 2 && ui_autoholdindicator) {
          uint8_t b0 = buf[0];
          uint8_t b1 = buf[1];

          // ON 여부: 00(주행 ON 대기), 01(정차 ON 작동)
          bool ah_on     = (b1 == 0x00 || b1 == 0x01);
          // 작동 여부: 정차 ON(01) 또는 ABS가 압 유지(b0 == 0x11)
          bool ah_active = (b1 == 0x01) || (b0 == 0x11);

          if (!ah_on) {
            // AutoHold OFF → 표시 숨김
            lv_label_set_text(ui_autoholdindicator, "");
          } else {
            // ON 상태: 항상 "AUTO / HOLD" 두 줄 표시
            lv_label_set_text(ui_autoholdindicator, "AUTO\nHOLD");

            // 색상: 대기=검정, 작동중=초록
            lv_color_t col = ah_active
                ? lv_color_hex(0x00C040)   // 작동중: 초록
                : lv_color_hex(0x000000);  // ON 대기: 검정 (나이트모드 나중에 조정)

            lv_obj_set_style_text_color(ui_autoholdindicator,
                                        col,
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
          }
        }
        break;
      }

      // ===== 0x1087: PRND 기어 위치 =====
      case 0x1087: {
        if (len >= 2) {
          uint8_t raw_gear = buf[1];  // Byte1 기준 (40=P, 47=R, 46=N, 45=D)
          // 디버그용: 값 확인
          // Serial.print("GEAR raw = 0x"); Serial.println(raw_gear, HEX);

          gear_update_from_byte1(raw_gear);
        }
        break;
      }

      default:
        // 아직 안 쓰는 ID는 무시
        break;
    }
  }
}


//setup, loop

void setup() {
  Serial.begin(115200);
  delay(100);

  tft.begin();
  tft.setRotation(0);

  // ===== LVGL 초기화 =====
  lv_init();

  lv_disp_draw_buf_init(&draw_buf, buf1, NULL, 240 * 20);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = 240;
  disp_drv.ver_res = 240;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;

  lv_disp_t *disp = lv_disp_drv_register(&disp_drv);
  lv_disp_set_default(disp);

  // ===== UI 생성 =====
  ui_init();  // SquareLine에서 생성한 ui_booting / ui_normalday 전부 이 안에서 만듦

  // Coolant 아이콘 이미지 연결
  lv_img_set_src(ui_coolantimg, &ui_img_coolant);

  // ===== MCP2515 초기화 =====
  // VSPI: SCK=18, MISO=19, MOSI=23
  SPI.begin(18, 19, 23);

  // MCP 오실레이터가 8MHz라고 가정 (16MHz라면 MCP_16MHZ로 변경)
  byte canResult = CAN0.begin(MCP_ANY, CAN_500KBPS, MCP_8MHZ);
  if (canResult == CAN_OK) {
    Serial.println("CAN init OK");
    g_can_ok = true;  // ✅ CAN 사용 가능
    CAN0.setMode(MCP_NORMAL);   // 실제 버스 참여 모드
  } else {
    Serial.print("CAN init FAIL, code=");
    Serial.println(canResult);
    g_can_ok = false;          // ✅ 실패 시에는 CAN 호출 건너뛴다
  }

  pinMode(CAN_INT, INPUT);

  // ===== 부팅 애니메이션 시작 =====
  lv_bar_set_value(ui_loadingbar, 0, LV_ANIM_OFF);
  lv_obj_clear_flag(ui_loadingbar, LV_OBJ_FLAG_HIDDEN);
  boot_timer = lv_timer_create(boot_timer_cb, 30, NULL);

  last_ms = millis();
}


void loop() {
  uint32_t now = millis();
  lv_tick_inc(now - last_ms);
  last_ms = now;

  lv_timer_handler();

  if (g_ui_ready && g_can_ok) {
    read_can_and_update_ui();   // 위에서 바꾼 버전
  }

  // ✅ OVERRIDE 점멸 처리
  scc_override_blink_task();

  delay(5);
}

