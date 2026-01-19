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
#include "ui_sportsmode.h"
#include "ui_booting.h"
#include "ui_img_coolant.h"
#include "ui_img_scc.h"
#include "spt_ui_img_scc.h"

#ifdef __cplusplus
}
#endif

//scctestloop



// ===== MCP2515 핀 정의 (앞에서 잡은 배선 기준) =====
#define CAN_CS 5         // MCP2515 CS
#define CAN_INT 27       // MCP2515 INT
#define MODE_BTN_PIN 26  // mode change BTN

// ===== SCC state =====
typedef enum {
  SCC_OFF = 0,   // C1
  SCC_READY,     // E1
  SCC_ON,        // E9 (FOLLOWING)
  SCC_OVERRIDE,  // F1 (운전자 가속 개입)
  SCC_LS_CANCEL  // F9 (저속 해제)
} scc_state_t;

typedef enum { MODE_NORMAL = 0,
               MODE_SPORTS = 1 } screen_mode_t;
static screen_mode_t g_mode = MODE_NORMAL;

MCP_CAN CAN0(CAN_CS);



TFT_eSPI tft = TFT_eSPI();

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[240 * 40];
static lv_color_t buf2[240 * 40];

static uint32_t last_ms = 0;

// ===== timers =====
static lv_timer_t *boot_timer = nullptr;
static bool g_ui_ready = false;
static bool g_can_ok = false;  // CAN이 정상 초기화되었는지 여부








static scc_state_t decode_scc_state(uint8_t raw) {
  switch (raw) {
    case 0xC1: return SCC_OFF;        // SYSTEM OFF
    case 0xE1: return SCC_READY;      // READY
    case 0xE9: return SCC_ON;         // FOLLOWING
    case 0xF1: return SCC_OVERRIDE;   // OVERRIDE (운전자 가속)
    case 0xF9: return SCC_LS_CANCEL;  // LOW SPEED CANCEL
    default: return SCC_OFF;
  }
}

static bool g_scc_override_visible = true;   // 현재 보여주는 중인지
static unsigned long g_scc_override_ms = 0;  // 마지막 토글 시각


static scc_state_t g_scc_state = SCC_OFF;
// ===== Runtime values (CAN으로 계속 갱신될 값 캐시) =====
static int g_speed_kph = 0;
static int g_coolant_c = 85;
static int g_rpm = 0;
static int g_scc_set_kph = 120;
static int g_scc_dist_level = 2;     // 1~3
static bool g_scc_lead_car = false;  // false=전방차량 없음, true=있음




// ================= Display flush =================
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)color_p, w * h, true);
  tft.endWrite();

  lv_disp_flush_ready(disp);
}

// ================= Utility: clamp =================
static int clampi(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}


// ================= UI Transition (Normal <-> SCC) Animations =================
// LVGL 8.3.x 기준. 240x240 원형 LCD에서 화면 전환 시 어색한 점프를 줄이기 위한 효과.

// ---- Layout constants (SquareLine 기준 고정값) ----
static const lv_coord_t SPD_NORMAL_X = 0;
static const lv_coord_t SPD_NORMAL_Y = 0;
static const lv_coord_t KMH_NORMAL_X = 0;
static const lv_coord_t KMH_NORMAL_Y = 40;
static const lv_coord_t AH_NORMAL_X = 65;
static const lv_coord_t AH_NORMAL_Y = 0;

static const lv_coord_t SPD_SCC_X = 0;
static const lv_coord_t SPD_SCC_Y = 80;
static const lv_coord_t KMH_SCC_X = 0;
static const lv_coord_t KMH_SCC_Y = 105;
static const lv_coord_t AH_SCC_X = 46;
static const lv_coord_t AH_SCC_Y = 80;

// ---- Translate deltas (NORMAL -> SCC) ----
// 기존 절대좌표 상수로부터 "이동량"을 계산
static const lv_coord_t SPD_TX_SCC = (SPD_SCC_X - SPD_NORMAL_X);
static const lv_coord_t SPD_TY_SCC = (SPD_SCC_Y - SPD_NORMAL_Y);

static const lv_coord_t KMH_TX_SCC = (KMH_SCC_X - KMH_NORMAL_X);
static const lv_coord_t KMH_TY_SCC = (KMH_SCC_Y - KMH_NORMAL_Y);

static const lv_coord_t AH_TX_SCC = (AH_SCC_X - AH_NORMAL_X);
static const lv_coord_t AH_TY_SCC = (AH_SCC_Y - AH_NORMAL_Y);


// ---- Timing ----
static const uint16_t TRANS_MOVE_MS = 320;
static const uint16_t TRANS_FADE_MS = 220;

// ---- Exec callbacks ----
static void anim_exec_set_tx(void *var, int32_t v) {
  lv_obj_set_style_translate_x((lv_obj_t *)var, (lv_coord_t)v, LV_PART_MAIN | LV_STATE_DEFAULT);
}
static void anim_exec_set_ty(void *var, int32_t v) {
  lv_obj_set_style_translate_y((lv_obj_t *)var, (lv_coord_t)v, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void anim_exec_set_opa(void *var, int32_t v) {
  lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, LV_PART_MAIN | LV_STATE_DEFAULT);
}

// Arc용: 기본적으로 MAIN/INDICATOR/KNOB를 함께 페이드
static void anim_exec_set_arc_opa(void *var, int32_t v) {
  lv_obj_t *o = (lv_obj_t *)var;
  lv_obj_set_style_opa(o, (lv_opa_t)v, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_arc_opa(o, (lv_opa_t)v, LV_PART_INDICATOR | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_opa(o, (lv_opa_t)v, LV_PART_KNOB | LV_STATE_DEFAULT);
}

// SCCArcON용: MAIN은 0~100까지만(기존 디자인 유지), INDICATOR/KNOB는 0~255
static void anim_exec_set_scc_arc_opa(void *var, int32_t v) {
  lv_obj_t *o = (lv_obj_t *)var;
  int32_t main_opa = (v * 100) / 255;
  lv_obj_set_style_opa(o, (lv_opa_t)main_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_arc_opa(o, (lv_opa_t)v, LV_PART_INDICATOR | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_opa(o, (lv_opa_t)v, LV_PART_KNOB | LV_STATE_DEFAULT);
}

static void start_anim(void *var,
                       lv_anim_exec_xcb_t exec_cb,
                       int32_t from, int32_t to,
                       uint16_t time_ms, uint16_t delay_ms,
                       lv_anim_ready_cb_t ready_cb,
                       void *user_data) {
  lv_anim_del(var, exec_cb);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, var);
  lv_anim_set_exec_cb(&a, exec_cb);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_time(&a, time_ms);
  lv_anim_set_delay(&a, delay_ms);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  if (ready_cb) lv_anim_set_ready_cb(&a, ready_cb);
  if (user_data) lv_anim_set_user_data(&a, user_data);
  lv_anim_start(&a);
}

typedef struct {
  lv_obj_t *obj;
  bool hide_on_done;
} anim_hide_ud_t;

static void anim_hide_ready_cb(lv_anim_t *a) {
  anim_hide_ud_t *ud = (anim_hide_ud_t *)a->user_data;
  if (ud) {
    if (ud->hide_on_done && ud->obj) {
      lv_obj_add_flag(ud->obj, LV_OBJ_FLAG_HIDDEN);
    }
    lv_mem_free(ud);
    a->user_data = NULL;
  }
}

static void fade_show_obj(lv_obj_t *obj, bool is_arc, bool is_scc_arc) {
  if (!obj) return;

  lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);

  // 초기 투명
  if (is_scc_arc) anim_exec_set_scc_arc_opa(obj, 0);
  else if (is_arc) anim_exec_set_arc_opa(obj, 0);
  else anim_exec_set_opa(obj, 0);

  // 페이드 인
  start_anim(obj,
             is_scc_arc ? (lv_anim_exec_xcb_t)anim_exec_set_scc_arc_opa
                        : (is_arc ? (lv_anim_exec_xcb_t)anim_exec_set_arc_opa
                                  : (lv_anim_exec_xcb_t)anim_exec_set_opa),
             0, 255,
             TRANS_FADE_MS, 0,
             NULL, NULL);
}

static void fade_hide_obj(lv_obj_t *obj, bool is_arc, bool is_scc_arc) {
  if (!obj) return;

  lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);  // 숨김 상태면 페이드가 안 보이므로 먼저 해제

  anim_hide_ud_t *ud = (anim_hide_ud_t *)lv_mem_alloc(sizeof(anim_hide_ud_t));
  if (!ud) {
    // 메모리 부족 시 즉시 숨김
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  ud->obj = obj;
  ud->hide_on_done = true;

  start_anim(obj,
             is_scc_arc ? (lv_anim_exec_xcb_t)anim_exec_set_scc_arc_opa
                        : (is_arc ? (lv_anim_exec_xcb_t)anim_exec_set_arc_opa
                                  : (lv_anim_exec_xcb_t)anim_exec_set_opa),
             255, 0,
             TRANS_FADE_MS, 0,
             anim_hide_ready_cb, ud);
}

static void move_obj_translate_to(lv_obj_t *obj,
                                  lv_coord_t tx_to, lv_coord_t ty_to,
                                  uint16_t time_ms,
                                  lv_anim_ready_cb_t ty_ready_cb,
                                  void *ty_user_data) {
  if (!obj) return;

  lv_coord_t tx_from = lv_obj_get_style_translate_x(obj, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_coord_t ty_from = lv_obj_get_style_translate_y(obj, LV_PART_MAIN | LV_STATE_DEFAULT);

  if (tx_from != tx_to) {
    start_anim(obj, (lv_anim_exec_xcb_t)anim_exec_set_tx,
               tx_from, tx_to,
               time_ms, 0,
               NULL, NULL);
  }

  if (ty_from != ty_to) {
    start_anim(obj, (lv_anim_exec_xcb_t)anim_exec_set_ty,
               ty_from, ty_to,
               time_ms, 0,
               ty_ready_cb, ty_user_data);
  } else {
    if (ty_ready_cb) {
      lv_anim_t dummy;
      dummy.user_data = ty_user_data;
      ty_ready_cb(&dummy);
    }
  }
}


static bool is_scc_layout_state(scc_state_t st) {
  return (st != SCC_OFF);
}

static void set_main_text_fonts_scc(void) {
  if (ui_speedlabel) lv_obj_set_style_text_font(ui_speedlabel, &lv_font_montserrat_32, LV_PART_MAIN);
  if (ui_kmhtext) lv_obj_set_style_text_font(ui_kmhtext, &lv_font_montserrat_10, LV_PART_MAIN);
  if (ui_autoholdindicator) lv_obj_set_style_text_font(ui_autoholdindicator, &lv_font_montserrat_8, LV_PART_MAIN);
}

static void set_main_text_fonts_normal(void) {
  if (ui_speedlabel) lv_obj_set_style_text_font(ui_speedlabel, &lv_font_montserrat_48, LV_PART_MAIN);
  if (ui_kmhtext) lv_obj_set_style_text_font(ui_kmhtext, &lv_font_montserrat_14, LV_PART_MAIN);
  if (ui_autoholdindicator) lv_obj_set_style_text_font(ui_autoholdindicator, &lv_font_montserrat_10, LV_PART_MAIN);
}



static void speed_to_normal_ready_cb(lv_anim_t *a) {
  (void)a;
  // 위치 애니메이션이 끝난 뒤 폰트만 정상 상태로 복귀(점프 최소화)
  set_main_text_fonts_normal();
}


static void speed_to_scc_ready_cb(lv_anim_t *a) {
  (void)a;
  // SCC 진입 이동이 끝난 뒤 폰트 변경
  set_main_text_fonts_scc();
}


static void transition_to_scc_layout(bool to_scc) {
  if (to_scc) {
    // 폰트는 이동 후에 적용(점프/줌 느낌 최소화)
    move_obj_translate_to(ui_speedlabel, SPD_TX_SCC, SPD_TY_SCC, TRANS_MOVE_MS, speed_to_scc_ready_cb, NULL);
    move_obj_translate_to(ui_kmhtext, KMH_TX_SCC, KMH_TY_SCC, TRANS_MOVE_MS, NULL, NULL);
    move_obj_translate_to(ui_autoholdindicator, AH_TX_SCC, AH_TY_SCC, TRANS_MOVE_MS, NULL, NULL);

  } else {
    // NORMAL 복귀: translate를 0으로
    move_obj_translate_to(ui_speedlabel, 0, 0, TRANS_MOVE_MS, speed_to_normal_ready_cb, NULL);
    move_obj_translate_to(ui_kmhtext, 0, 0, TRANS_MOVE_MS, NULL, NULL);
    move_obj_translate_to(ui_autoholdindicator, 0, 0, TRANS_MOVE_MS, NULL, NULL);
  }




  // SCC 객체: 페이드 인/아웃
  if (to_scc) {
    fade_show_obj(ui_SCCsettext, false, false);
    fade_show_obj(ui_SCCsetspeedlabel, false, false);
    fade_show_obj(ui_SCCkmhtext, false, false);
    fade_show_obj(ui_SCCimg, false, false);

    // SCCArcON은 디자인 유지용 전용 페이드
    fade_show_obj(ui_SCCArcON, true, true);

    // Coolant는 사라짐
    fade_hide_obj(ui_Arccoolant, true, false);
    fade_hide_obj(ui_coolantimg, false, false);

    // Gear는 NORMAL에서만
    if (ui_gearlabel) lv_obj_add_flag(ui_gearlabel, LV_OBJ_FLAG_HIDDEN);

  } else {
    fade_hide_obj(ui_SCCsettext, false, false);
    fade_hide_obj(ui_SCCsetspeedlabel, false, false);
    fade_hide_obj(ui_SCCkmhtext, false, false);
    fade_hide_obj(ui_SCCimg, false, false);
    fade_hide_obj(ui_SCCArcON, true, true);

    // Coolant는 다시 나타남
    fade_show_obj(ui_Arccoolant, true, false);
    fade_show_obj(ui_coolantimg, false, false);

    if (ui_gearlabel) lv_obj_clear_flag(ui_gearlabel, LV_OBJ_FLAG_HIDDEN);
  }
}

// ================= Speed UI =================
static void set_speed(int kph) {
  kph = clampi(kph, 0, 260);

  if (ui_Arcspeed) lv_arc_set_value(ui_Arcspeed, kph);

  char buf[8];
  snprintf(buf, sizeof(buf), "%d", kph);

  if (ui_speedlabel) lv_label_set_text(ui_speedlabel, buf);
  if (ui_sptspeedlabel) lv_label_set_text(ui_sptspeedlabel, buf);  // ✅ sports도 갱신
}

// ================= Coolant UI =================
static void set_coolant(int coolant_c) {
  coolant_c = clampi(coolant_c, 0, 160);

  if (ui_Arccoolant) lv_arc_set_value(ui_Arccoolant, coolant_c);
  if (ui_sptcoolantgauge) lv_arc_set_value(ui_sptcoolantgauge, coolant_c);  // ✅

  lv_color_t col;
  if (coolant_c >= 105) col = lv_color_hex(0xFF3B30);
  else if (coolant_c >= 100) col = lv_color_hex(0xFFCC00);
  else col = lv_color_hex(0x2BCC30);

  if (ui_Arccoolant) lv_obj_set_style_arc_color(ui_Arccoolant, col, LV_PART_INDICATOR | LV_STATE_DEFAULT);
  if (ui_sptcoolantgauge) lv_obj_set_style_arc_color(ui_sptcoolantgauge, col, LV_PART_INDICATOR | LV_STATE_DEFAULT);  // ✅
}

// ================= spt RPM UI =================
static void set_rpm(int rpm) {
  rpm = clampi(rpm, 0, 8000);

  if (ui_sptRPMgauge) lv_arc_set_value(ui_sptRPMgauge, rpm);

  // ✅ RPM 텍스트 제거: 숫자만 1줄로 표시
  if (ui_sptrpmtext) {
    char t[8];
    snprintf(t, sizeof(t), "%d", rpm);
    lv_label_set_text(ui_sptrpmtext, t);
  }
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

  if (ui_sptSCCsetspeedlabel) {
    char b[8];
    snprintf(b, sizeof(b), "%d", set_kph);
    lv_label_set_text(ui_sptSCCsetspeedlabel, b);
  }
}

// ================= SCC: text color (arc 제외) =================
static lv_color_t scc_ready_color(void) {
  // sports 배경이 어두우니 밝게
  if (g_mode == MODE_SPORTS) return lv_color_hex(0xC0C0C0);  // 연회색(권장)
  return lv_color_hex(0x000000);                             // normal READY(기존 유지)
}


static void scc_set_text_color_by_state(scc_state_t state) {
  lv_color_t color;

  switch (state) {
    case SCC_READY:
      // READY: 검정
      color = scc_ready_color();
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

  // ✅ sports SCC text color도 같이 적용
  if (ui_sptSCCsettext) {
    lv_obj_set_style_text_color(ui_sptSCCsettext, color, LV_PART_MAIN | LV_STATE_DEFAULT);
  }
  if (ui_sptSCCsetspeedlabel) {
    lv_obj_set_style_text_color(ui_sptSCCsetspeedlabel, color, LV_PART_MAIN | LV_STATE_DEFAULT);
  }
  if (ui_sptSCCkmhtext) {
    lv_obj_set_style_text_color(ui_sptSCCkmhtext, color, LV_PART_MAIN | LV_STATE_DEFAULT);
  }
}



static void scc_set_visible(scc_state_t state) {
  bool scc_visible = (state != SCC_OFF);

  // READY / ON / OVERRIDE / LOW SPEED CANCEL 은 모두 쿨런트 숨김
  bool hide_coolant = (state == SCC_READY || state == SCC_ON || state == SCC_OVERRIDE || state == SCC_LS_CANCEL);

  // 기어 표시는 NORMAL(SCC_OFF)에서만
  bool show_gear = (state == SCC_OFF);

  // --- SCC 그룹 표시/숨김 ---
  if (scc_visible) {
    lv_obj_clear_flag(ui_SCCsettext, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_SCCsetspeedlabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_SCCkmhtext, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_SCCArcON, LV_OBJ_FLAG_HIDDEN);
    if (ui_SCCimg) lv_obj_clear_flag(ui_SCCimg, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(ui_SCCsettext, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_SCCsetspeedlabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_SCCkmhtext, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_SCCArcON, LV_OBJ_FLAG_HIDDEN);
    if (ui_SCCimg) lv_obj_add_flag(ui_SCCimg, LV_OBJ_FLAG_HIDDEN);
  }

  // --- Coolant 게이지/아이콘 표시/숨김 ---
  if (hide_coolant) {
    lv_obj_add_flag(ui_Arccoolant, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_coolantimg, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(ui_Arccoolant, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_coolantimg, LV_OBJ_FLAG_HIDDEN);
  }

  // --- Gear 라벨 표시/숨김 ---
  if (ui_gearlabel) {
    if (show_gear) {
      lv_obj_clear_flag(ui_gearlabel, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(ui_gearlabel, LV_OBJ_FLAG_HIDDEN);
    }
  }
}


// ================= SCC: image table & update =================
// index: [color][lead_car][dist]
// color: 0 = READY, 1 = ON
// lead_car: 0 = 없음, 1 = 있음
// dist: 0,1,2 => d1,d2,d3
static const lv_img_dsc_t *SCC_IMG[2][2][3] = {
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

// sports SCC: READY/ON(2) x leadcar(2) x dist(3)
static const lv_img_dsc_t *SPT_SCC_IMG[2][2][3] = {
  // READY (회색)
  {
    { &spt_ui_img_scc_rdy_car0_d1, &spt_ui_img_scc_rdy_car0_d2, &spt_ui_img_scc_rdy_car0_d3 },
    { &spt_ui_img_scc_rdy_car1_d1, &spt_ui_img_scc_rdy_car1_d2, &spt_ui_img_scc_rdy_car1_d3 },
  },
  // ON (초록)
  {
    { &spt_ui_img_scc_on_car0_d1, &spt_ui_img_scc_on_car0_d2, &spt_ui_img_scc_on_car0_d3 },
    { &spt_ui_img_scc_on_car1_d1, &spt_ui_img_scc_on_car1_d2, &spt_ui_img_scc_on_car1_d3 },
  }
};


// SCC 상태/거리/전방차량에 맞춰 ui_SCCimg 갱신
static void scc_update_image(void) {
  if (!ui_SCCimg) return;

  // 가시성(숨김/표시)은 scc_set_visible()/transition에서 담당
  if (g_scc_state == SCC_OFF) {
    return;
  }

  int color_idx = (g_scc_state == SCC_ON || g_scc_state == SCC_OVERRIDE) ? 1 : 0;
  // READY / LS_CANCEL 등 → 0(회색 세트), ON / OVERRIDE → 1(초록 세트)

  int car_idx = g_scc_lead_car ? 1 : 0;               // 0=없음, 1=있음
  int dist_idx = clampi(g_scc_dist_level, 1, 3) - 1;  // 0~2

  const lv_img_dsc_t *img = SCC_IMG[color_idx][car_idx][dist_idx];
  lv_img_set_src(ui_SCCimg, img);
}

static void spt_scc_update_image(void) {
  if (!ui_sptSCCimg) return;

  // SCC OFF면 숨김
  if (g_scc_state == SCC_OFF) {
    lv_obj_add_flag(ui_sptSCCimg, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  // READY/ON이면 표시
  lv_obj_clear_flag(ui_sptSCCimg, LV_OBJ_FLAG_HIDDEN);

  // READY/LS_CANCEL => 0(회색), ON/OVERRIDE => 1(초록)
  int color_idx = (g_scc_state == SCC_ON || g_scc_state == SCC_OVERRIDE) ? 1 : 0;

  int car_idx = g_scc_lead_car ? 1 : 0;               // 0=없음, 1=있음
  int dist_idx = clampi(g_scc_dist_level, 1, 3) - 1;  // 0~2

  lv_img_set_src(ui_sptSCCimg, SPT_SCC_IMG[color_idx][car_idx][dist_idx]);
}


static void scc_set_text_blink_visible(bool visible) {
  if (ui_SCCsettext) {
    if (visible) lv_obj_clear_flag(ui_SCCsettext, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ui_SCCsettext, LV_OBJ_FLAG_HIDDEN);
  }
  if (ui_SCCsetspeedlabel) {
    if (visible) lv_obj_clear_flag(ui_SCCsetspeedlabel, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ui_SCCsetspeedlabel, LV_OBJ_FLAG_HIDDEN);
  }
  if (ui_SCCkmhtext) {
    if (visible) lv_obj_clear_flag(ui_SCCkmhtext, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ui_SCCkmhtext, LV_OBJ_FLAG_HIDDEN);
  }
}

static void scc_override_blink_task(void) {
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
// (새로 추가) 애니메이션 없이 즉시 레이아웃 동기화(translate만 사용)
static void sync_main_text_layout_instant(bool to_scc) {
  if (!ui_speedlabel || !ui_kmhtext || !ui_autoholdindicator) return;

  // pos는 건드리지 않는다! (SquareLine 기준 좌표 유지)
  if (to_scc) {
    set_main_text_fonts_scc();

    lv_obj_set_style_translate_x(ui_speedlabel, SPD_TX_SCC, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_translate_y(ui_speedlabel, SPD_TY_SCC, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_translate_x(ui_kmhtext, KMH_TX_SCC, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_translate_y(ui_kmhtext, KMH_TY_SCC, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_translate_x(ui_autoholdindicator, AH_TX_SCC, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_translate_y(ui_autoholdindicator, AH_TY_SCC, LV_PART_MAIN | LV_STATE_DEFAULT);

  } else {
    set_main_text_fonts_normal();

    lv_obj_set_style_translate_x(ui_speedlabel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_translate_y(ui_speedlabel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_translate_x(ui_kmhtext, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_translate_y(ui_kmhtext, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_translate_x(ui_autoholdindicator, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_translate_y(ui_autoholdindicator, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
  }
}


// ================= Gear (PRND) 표시 =================

// 디바운스용 상태
static uint8_t g_gear_raw_last = 0xFF;    // 마지막으로 관측된 raw 값
static uint8_t g_gear_raw_stable = 0x40;  // 안정된(raw 확정) 값 (기본 P로 시작)
static uint8_t g_gear_same_count = 0;     // 같은 값이 연속으로 몇 번 들어왔는지

// raw(Byte1) -> 표시 문자열 변환
static const char *gear_text_from_raw(uint8_t raw) {
  switch (raw) {
    case 0x40: return "P";  // Park
    case 0x47: return "R";  // Reverse
    case 0x46: return "N";  // Neutral
    case 0x45: return "D";  // Drive
    default: return "-";    // 정의되지 않은 값
  }
}

// 디바운스 포함 업데이트
// ================= PRND 기어 표시 (디바운스 없이 즉시 반영) =================
static void gear_update_from_byte1(uint8_t raw) {
  const char *gear_text = gear_text_from_raw(raw);

  if (ui_gearlabel) lv_label_set_text(ui_gearlabel, gear_text);
  if (ui_sptgearlabel) lv_label_set_text(ui_sptgearlabel, gear_text);
}





// ================= State setter =================
static void set_scc_state(scc_state_t new_state) {
  if (g_scc_state == new_state) return;

  scc_state_t prev = g_scc_state;
  bool prev_layout = is_scc_layout_state(prev);
  bool new_layout = is_scc_layout_state(new_state);

  g_scc_state = new_state;

  // 색상(READY 검정 / ON,OVERRIDE 초록 / LS_CANCEL 빨강)
  scc_set_text_color_by_state(g_scc_state);

  // 이미지 소스는 상태 변경 시점에 갱신
  if (g_scc_state != SCC_OFF) {
    scc_update_image();
  }

  // UI 준비 전(부팅 중)에는 즉시 반영
  if (!g_ui_ready) {

    return;
  }

  bool to_scc = (g_scc_state != SCC_OFF);  // 또는 is_scc_layout_state(g_scc_state)

  scc_set_visible(g_scc_state);
  sync_main_text_layout_instant(to_scc);

  scc_set_text_color_by_state(g_scc_state);

  if (g_scc_state != SCC_OFF) {
    scc_update_set_speed(g_scc_set_kph);
    scc_update_image();
    spt_scc_update_image();
  }


  // Normal <-> SCC 전환만 애니메이션
  if (prev_layout != new_layout) {
    // normal 화면이 실제로 떠 있을 때만 애니메이션
    bool normal_active = (g_mode == MODE_NORMAL) && (lv_scr_act() == ui_normalday);

    if (normal_active) {
      transition_to_scc_layout(new_layout);  // 애니메이션
    } else {
      sync_main_text_layout_instant(new_layout);  // 즉시 반영
      scc_set_visible(g_scc_state);               // coolant/gear/scc 숨김 상태 즉시 동기화
    }
  } else {
    // READY<->ON 같은 “레이아웃 변화 없는” 상태는 위치 건드리지 말고
    // visible + 색 + 이미지 + set speed만 갱신
    scc_set_visible(g_scc_state);
  }



  if (g_mode == MODE_SPORTS) {
    spt_set_scc_group_visible(scc_is_visible_state());
  }

  // SCC 표시 중이면 set speed 동기화
  if (g_scc_state != SCC_OFF) {
    scc_update_set_speed(g_scc_set_kph);
  }
}


// ===== Forward declarations (load_screen_by_mode에서 사용하므로 위에 필요) =====
static void set_speed(int kph);
static void set_coolant(int coolant_c);


static void sports_post_load_cb(lv_timer_t *t) {
  // 레이아웃 확정
  if (ui_sportsmode) lv_obj_update_layout(ui_sportsmode);
  lv_refr_now(NULL);

  // SCC OFF면 숨김, READY/ON이면 표시
  spt_set_scc_group_visible(scc_is_visible_state());

  lv_timer_del(t);
}

static void load_screen_by_mode(screen_mode_t m) {
  lv_obj_t *target = (m == MODE_SPORTS) ? ui_sportsmode : ui_normalday;
  if (lv_scr_act() == target) return;

  if (ui_speedlabel) {
    lv_anim_del(ui_speedlabel, (lv_anim_exec_xcb_t)anim_exec_set_tx);
    lv_anim_del(ui_speedlabel, (lv_anim_exec_xcb_t)anim_exec_set_ty);
  }
  if (ui_kmhtext) {
    lv_anim_del(ui_kmhtext, (lv_anim_exec_xcb_t)anim_exec_set_tx);
    lv_anim_del(ui_kmhtext, (lv_anim_exec_xcb_t)anim_exec_set_ty);
  }
  if (ui_autoholdindicator) {
    lv_anim_del(ui_autoholdindicator, (lv_anim_exec_xcb_t)anim_exec_set_tx);
    lv_anim_del(ui_autoholdindicator, (lv_anim_exec_xcb_t)anim_exec_set_ty);
  }


  lv_scr_load_anim(target, LV_SCR_LOAD_ANIM_FADE_IN, 180, 0, false);

  set_speed(g_speed_kph);
  set_coolant(g_coolant_c);

  if (m == MODE_NORMAL) {
    // 1) 무조건 OFF(기본)로 리셋: 중앙 원점/큰 폰트/쿨런트 보이기
    sync_main_text_layout_instant(false);
    scc_set_visible(SCC_OFF);  // SCC 그룹 숨기고 coolant/gear 복구

    // 2) 실제 SCC가 켜져 있다면, OFF에서 다시 SCC 레이아웃으로 내려가는 애니메이션을 재생
    if (g_scc_state != SCC_OFF) {
      // 색/이미지/설정속도 먼저 맞춤
      scc_set_text_color_by_state(g_scc_state);
      scc_update_image();
      scc_update_set_speed(g_scc_set_kph);

      transition_to_scc_layout(true);
    }
  }

  scc_set_text_color_by_state(g_scc_state);
}



// ================= Boot animation timer =================
static void boot_timer_cb(lv_timer_t *t) {
  (void)t;
  static int p = 0;

  p += 2;  // 0->100 속도
  if (p > 100) p = 100;

  lv_bar_set_value(ui_loadingbar, p, LV_ANIM_OFF);

  if (p >= 100) {
    // bar 숨기고 normalday로 전환
    lv_obj_add_flag(ui_loadingbar, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load_anim(ui_normalday, LV_SCR_LOAD_ANIM_FADE_IN, 250, 0, false);
    //lv_scr_load_anim(ui_sportsmode, LV_SCR_LOAD_ANIM_FADE_IN, 250, 0, false);


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

static lv_color_t autohold_standby_color(void) {
  if (g_mode == MODE_SPORTS) return lv_color_hex(0xC0C0C0);  // 연회색
  return lv_color_hex(0x000000);                             // 노말 대기(기존)
}

static void set_autohold_indicator_common(lv_obj_t *label, bool ah_on, bool ah_active) {
  if (!label) return;

  if (!ah_on) {
    lv_label_set_text(label, "");
    return;
  }

  lv_label_set_text(label, "AUTO\nHOLD");

  lv_color_t col = ah_active ? lv_color_hex(0x00C040) : autohold_standby_color();
  lv_obj_set_style_text_color(label, col, LV_PART_MAIN | LV_STATE_DEFAULT);
}



// ================= CAN 수신 → UI 반영 =================
static void read_can_and_update_ui() {
  // 한 루프에 최대 5개까지 처리 (필요하면 숫자 조정 가능)
  for (int n = 0; n < 5; ++n) {
    if (CAN0.checkReceive() != CAN_MSGAVAIL) break;

    unsigned long canId = 0;
    byte len = 0;
    byte buf[8];

    CAN0.readMsgBuf(&canId, &len, buf);

    switch (canId) {

      // ===== 0x420: SCC 상태 / 세트속도 / 거리 / 전방차량 =====
      case 0x420:
        {
          if (len < 7) break;

          uint8_t raw_state = buf[0];
          uint8_t raw_set = buf[5];
          uint8_t d6 = buf[6];

          // 상태 디코딩
          scc_state_t st = decode_scc_state(raw_state);
          set_scc_state(st);  // 가시성/색/레이아웃/SCC 이미지 기본 처리

          // 세트속도 (raw 그대로 km/h)
          g_scc_set_kph = raw_set;
          scc_update_set_speed(g_scc_set_kph);

          // 거리 단계 (상위 4비트, 1~3)
          int dist = (d6 >> 4) & 0x0F;
          if (dist < 1 || dist > 3) dist = 2;  // 이상값 나오면 가운데(2)
          g_scc_dist_level = dist;

          // 전방차량 유무 (하위 4비트: 0x0D = 있음, 0x0C = 없음)
          uint8_t lowNib = d6 & 0x0F;
          g_scc_lead_car = (lowNib == 0x0D);

          // 거리/전방차량에 맞춰 이미지 갱신
          scc_update_image();
          spt_scc_update_image();
          break;
        }

      // ===== 0x0A0: 속도 + 냉각수 (최유력 후보) =====
      case 0x0A0:
        {
          // RPM: Byte2, Byte3 little-endian, rpm = raw/4
          if (len >= 4) {
            uint16_t rpm_raw = ((uint16_t)buf[3] << 8) | buf[2];
            g_rpm = (int)(rpm_raw / 4);
            set_rpm(g_rpm);
          }

          // 속도
          if (len >= 5) {
            uint8_t raw_speed = buf[4];
            g_speed_kph = raw_speed;
            set_speed(g_speed_kph);
          }

          // 냉각수
          if (len >= 2) {
            uint8_t raw_clt = buf[1];
            g_coolant_c = (int)raw_clt - 40;
            set_coolant(g_coolant_c);
          }
          break;
        }


        // ===== 0x47F: AutoHold 상태 =====
      case 0x47F:
        {
          if (len >= 2) {
            uint8_t b0 = buf[0];
            uint8_t b1 = buf[1];

            bool ah_on = (b1 == 0x00 || b1 == 0x01);
            bool ah_active = (b1 == 0x01) || (b0 == 0x11);

            // ✅ normal 화면
            set_autohold_indicator_common(ui_autoholdindicator, ah_on, ah_active);

            // ✅ sports 화면 (sportsmode에 있는 라벨)
            set_autohold_indicator_common(ui_sptautoholdindicator, ah_on, ah_active);
          }
          break;
        }


      // ===== 0x1087: PRND 기어 위치 =====
      case 0x43F:
        {
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


static void spt_set_scc_group_visible(bool vis) {
  lv_obj_t *objs[] = {
    ui_sptSCCsettext,
    ui_sptSCCsetspeedlabel,
    ui_sptSCCkmhtext,
    ui_sptSCCimg
  };

  for (size_t i = 0; i < sizeof(objs) / sizeof(objs[0]); i++) {
    if (!objs[i]) continue;
    if (vis) lv_obj_clear_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
  }

  // 글자 가림 방지: SCC 이미지는 뒤로, 텍스트는 앞으로 (선택)
  if (ui_sptSCCimg) lv_obj_move_background(ui_sptSCCimg);
  if (ui_sptSCCsettext) lv_obj_move_foreground(ui_sptSCCsettext);
  if (ui_sptSCCsetspeedlabel) lv_obj_move_foreground(ui_sptSCCsetspeedlabel);
  if (ui_sptSCCkmhtext) lv_obj_move_foreground(ui_sptSCCkmhtext);
}

static bool scc_is_visible_state(void) {
  return (g_scc_state == SCC_READY || g_scc_state == SCC_ON);
}







//setup, loop

void setup() {
  Serial.begin(115200);
  delay(100);

  tft.begin();
  tft.setRotation(0);

  // ===== LVGL 초기화 =====
  lv_init();

  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, 240 * 40);

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
    g_can_ok = true;           // ✅ CAN 사용 가능
    CAN0.setMode(MCP_NORMAL);  // 실제 버스 참여 모드
  } else {
    Serial.print("CAN init FAIL, code=");
    Serial.println(canResult);
    g_can_ok = false;  // ✅ 실패 시에는 CAN 호출 건너뛴다
  }

  pinMode(CAN_INT, INPUT);
  pinMode(MODE_BTN_PIN, INPUT_PULLUP);


  // ===== 부팅 애니메이션 시작 =====
  lv_bar_set_value(ui_loadingbar, 0, LV_ANIM_OFF);
  lv_obj_clear_flag(ui_loadingbar, LV_OBJ_FLAG_HIDDEN);
  boot_timer = lv_timer_create(boot_timer_cb, 30, NULL);

  last_ms = millis();
}


static void poll_mode_button(void) {
  static int last_read = HIGH;
  static int stable = HIGH;
  static uint32_t last_change_ms = 0;

  int now = digitalRead(MODE_BTN_PIN);

  if (now != last_read) {
    last_read = now;
    last_change_ms = millis();
  }

  // 50ms 디바운스
  if (millis() - last_change_ms < 50) return;

  if (now != stable) {
    stable = now;

    // 눌림(LOW) 엣지에서만 토글
    if (stable == LOW) {
      if (!g_ui_ready) return;  // ✅ 부팅 중에는 무시(원하면 나중에 개선 가능)

      g_mode = (g_mode == MODE_NORMAL) ? MODE_SPORTS : MODE_NORMAL;
      load_screen_by_mode(g_mode);
      //scctestloop
      spt_scc_update_image();
    }
  }
}



void loop() {
  uint32_t now = millis();
  lv_tick_inc(now - last_ms);
  last_ms = now;

  lv_timer_handler();

  if (g_ui_ready && g_can_ok) {
    read_can_and_update_ui();  // 위에서 바꾼 버전
  }

  // ✅ OVERRIDE 점멸 처리
  scc_override_blink_task();

  poll_mode_button();


  delay(5);
}
