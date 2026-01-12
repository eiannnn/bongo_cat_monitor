#include <lvgl.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <EEPROM.h>
#include "Free_Fonts.h"
#include "animations_sprites.h"

// Display settings
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320

// --- GLOBAL VARIABLES ---
uint32_t mouse_lock_timer = 0; 

// Configuration settings structure
struct BongoCatSettings {
    bool show_cpu = true;
    bool show_ram = true;
    bool show_wpm = true;
    bool show_time = true;
    bool time_format_24h = true;
    int sleep_timeout_minutes = 5;
    float animation_sensitivity = 1.0;
    uint32_t checksum = 0;
};
uint32_t calculateChecksum(const BongoCatSettings* s);
// EEPROM settings
#define SETTINGS_ADDRESS 0
#define SETTINGS_SIZE sizeof(BongoCatSettings)
#define EEPROM_SIZE 512

BongoCatSettings settings;
TFT_eSPI tft = TFT_eSPI();

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[SCREEN_WIDTH * 10];

sprite_manager_t sprite_manager;
lv_obj_t * cat_canvas = NULL;
uint32_t last_frame_time = 0;
bool animation_paused = false;

bool python_control_mode = true;
uint32_t last_command_time = 0;
#define TYPING_TIMEOUT_MS 2000
#define PYTHON_TIMEOUT_MS 5000

uint32_t frame_skip_counter = 0;
#define CAT_SIZE 64

lv_obj_t * screen = NULL;
lv_obj_t * cpu_label = NULL;
lv_obj_t * ram_label = NULL;
lv_obj_t * wpm_label = NULL;
lv_obj_t * time_label = NULL;

int cpu_usage = 0;
int ram_usage = 0;
int wpm_speed = 0;
String current_time_str = "00:00";
bool time_initialized = false;

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t*)&color_p->full, w * h, true);
    tft.endWrite();
    lv_disp_flush_ready(disp);
}

void updateSystemStats(int cpu, int ram, int wpm) {
    cpu_usage = cpu;
    ram_usage = ram;
    wpm_speed = wpm;
    if (cpu_label) lv_label_set_text_fmt(cpu_label, "CPU: %d%%", cpu);
    if (ram_label) lv_label_set_text_fmt(ram_label, "RAM: %d%%", ram);
    if (wpm_label) lv_label_set_text_fmt(wpm_label, "WPM: %d", wpm);
}

void updateTimeDisplay() {
    if (time_label && current_time_str.length() > 0) {
        String display_time = current_time_str;
        if (!settings.time_format_24h && current_time_str.length() == 5) {
            int hour = current_time_str.substring(0, 2).toInt();
            String minute = current_time_str.substring(3, 5);
            String ampm = (hour >= 12) ? "PM" : "AM";
            if (hour == 0) hour = 12;
            else if (hour > 12) hour -= 12;
            display_time = String(hour) + ":" + minute + "\n" + ampm;
        }
        lv_label_set_text(time_label, display_time.c_str());
        if (!time_initialized) {
            Serial.print("🕐 Time initialized: ");
            Serial.println(display_time);
            time_initialized = true;
        }
    }
}

uint32_t calculateChecksum(const BongoCatSettings* s) {
    uint32_t sum = 0;
    const uint8_t* data = (const uint8_t*)s;
    size_t size = sizeof(BongoCatSettings) - sizeof(uint32_t); 
    for (size_t i = 0; i < size; i++) sum += data[i];
    return sum;
}

bool validateSettings(const BongoCatSettings* s) {
    if (s->sleep_timeout_minutes < 1 || s->sleep_timeout_minutes > 60) return false;
    if (s->animation_sensitivity < 0.1 || s->animation_sensitivity > 5.0) return false;
    uint32_t expected_checksum = calculateChecksum(s);
    return (s->checksum == expected_checksum);
}

void saveSettings() {
    settings.checksum = calculateChecksum(&settings);
    EEPROM.put(SETTINGS_ADDRESS, settings);
    EEPROM.commit();
    Serial.println("💾 Settings saved to EEPROM");
}

void loadSettings() {
    BongoCatSettings temp_settings;
    EEPROM.get(SETTINGS_ADDRESS, temp_settings);
    if (validateSettings(&temp_settings)) {
        settings = temp_settings;
        Serial.println("📂 Settings loaded from EEPROM");
    } else {
        Serial.println("⚠️ Invalid settings in EEPROM, using defaults");
        resetSettings();
    }
}

void resetSettings() {
    settings.show_cpu = true;
    settings.show_ram = true;
    settings.show_wpm = true;
    settings.show_time = true;
    settings.time_format_24h = true;
    settings.sleep_timeout_minutes = 5;
    settings.animation_sensitivity = 1.0;
    settings.checksum = calculateChecksum(&settings);
    Serial.println("🔄 Settings reset to factory defaults");
}

void updateDisplayVisibility() {
    if (cpu_label) {
        if (settings.show_cpu) lv_obj_clear_flag(cpu_label, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(cpu_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (ram_label) {
        if (settings.show_ram) lv_obj_clear_flag(ram_label, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(ram_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (wpm_label) {
        if (settings.show_wpm) lv_obj_clear_flag(wpm_label, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(wpm_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (time_label) {
        if (settings.show_time) lv_obj_clear_flag(time_label, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(time_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void handleSerialCommands() {
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        command.trim();
        
        uint32_t current_time = millis();
        last_command_time = current_time;
        python_control_mode = true;
        
        if (command.startsWith("SPEED:")) {
            String speed_str = command.substring(6);
            uint16_t speed = speed_str.toInt();
            animation_state_t new_state = sprite_manager.current_state;
            
            if (speed == 0) {
                sprite_manager_set_state(&sprite_manager, ANIM_STATE_IDLE_STAGE1, current_time);
            } else if (speed < 80) {
                new_state = ANIM_STATE_TYPING_SLOW;
            } else if (speed < 150) {
                new_state = ANIM_STATE_TYPING_NORMAL;
            } else {
                new_state = ANIM_STATE_TYPING_FAST;
            }
            
            sprite_manager_set_state(&sprite_manager, new_state, current_time);
            
            // Only update speed if meaningful change
            uint16_t old_speed = sprite_manager.animation_speed_ms;
            sprite_manager.animation_speed_ms = speed;
            
            last_command_time = current_time;
            python_control_mode = true;
            sprite_manager.idle_progression_enabled = false;

        } else if (command.startsWith("MOUSE:")) {
            String btn = command.substring(6);
            
            // --- FLIPPED LOGIC (Requested by you) ---
            if (btn == "LEFT") {
                // User clicked LEFT -> Cat uses LEFT paw (Frame 0)
                sprite_manager.paw_frame = 0; 
                sprite_manager.current_sprites[LAYER_PAWS] = &leftpawdown;
                sprite_manager.current_sprites[LAYER_EFFECTS] = &left_click_effect;
            } 
            else if (btn == "RIGHT") {
                // User clicked RIGHT -> Cat uses RIGHT paw (Frame 2)
                sprite_manager.paw_frame = 2; 
                sprite_manager.current_sprites[LAYER_PAWS] = &rightpawdown;
                sprite_manager.current_sprites[LAYER_EFFECTS] = &right_click_effect;
            }
            // ----------------------------------------
            
            // Activate lock so the typing animation doesn't overwrite our click
            mouse_lock_timer = millis() + 150;
            
            sprite_manager.paw_animation_active = true;
            sprite_manager.current_sprites[LAYER_FACE] = &happy_face;
            
            Serial.println("🖱️ Mouse Click: " + btn);
        } else if (command == "STOP") {
            sprite_manager_set_state(&sprite_manager, ANIM_STATE_IDLE_STAGE1, current_time);
            sprite_manager.idle_progression_enabled = false;
            python_control_mode = true;
            last_command_time = current_time;
            
        } else if (command == "IDLE_START") {
            sprite_manager_set_state(&sprite_manager, ANIM_STATE_IDLE_STAGE1, current_time);
            sprite_manager.idle_progression_enabled = true;
            python_control_mode = false;
            
        } else if (command == "STREAK_ON") {
            sprite_manager.is_streak_mode = true;
        } else if (command == "STREAK_OFF") {
            sprite_manager.is_streak_mode = false;
        } else if (command.startsWith("STATS:")) {
             String stats = command.substring(6);
            int cpuStart = stats.indexOf("CPU:") + 4;
            int cpuEnd = stats.indexOf(",", cpuStart);
            int cpu = stats.substring(cpuStart, cpuEnd).toInt();
            int ramStart = stats.indexOf("RAM:") + 4;
            int ramEnd = stats.indexOf(",", ramStart);
            int ram = stats.substring(ramStart, ramEnd).toInt();
            int wpmStart = stats.indexOf("WPM:") + 4;
            int wpm = stats.substring(wpmStart).toInt();
            updateSystemStats(cpu, ram, wpm);
        } else if (command.startsWith("TIME:")) {
            String time_str = command.substring(5);
            if (time_str.length() == 5 && time_str.charAt(2) == ':') {
                current_time_str = time_str;
            }
        } else if (command.startsWith("CPU:")) {
            cpu_usage = command.substring(4).toInt();
        } else if (command.startsWith("RAM:")) {
            ram_usage = command.substring(4).toInt();
        } else if (command.startsWith("WPM:")) {
            wpm_speed = command.substring(4).toInt();
        } else if (command == "PING") {
            Serial.println("PONG");
        } else if (command == "SAVE_SETTINGS") {
            saveSettings();
        } else if (command == "LOAD_SETTINGS") {
            loadSettings();
            updateDisplayVisibility();
        } else if (command == "RESET_SETTINGS") {
            resetSettings();
            updateDisplayVisibility();
            saveSettings();
        }
    }
}

void sprite_manager_init(sprite_manager_t* manager) {
    manager->current_sprites[LAYER_BODY] = &standardbody1;
    manager->current_sprites[LAYER_FACE] = &stock_face;
    manager->current_sprites[LAYER_TABLE] = &table1;
    manager->current_sprites[LAYER_PAWS] = &twopawsup;
    manager->current_sprites[LAYER_EFFECTS] = NULL;
    
    manager->current_state = ANIM_STATE_IDLE_STAGE1;
    manager->state_start_time = millis();
    manager->blink_timer = millis() + random(3000, 8000);
    manager->ear_twitch_timer = millis() + random(10000, 30000);
    manager->effect_timer = 0;
    manager->effect_frame = 0;
    manager->paw_animation_active = false;
    manager->paw_frame = 0;
    manager->paw_timer = 0;
    manager->animation_speed_ms = 200;
    manager->click_effect_left = false;
    manager->idle_progression_enabled = false;
    manager->last_typing_time = 0;
    manager->is_streak_mode = false;
    manager->blink_start_time = 0;
    manager->blinking = false;
    manager->ear_twitch_start_time = 0;
    manager->ear_twitching = false;
    Serial.println("🐱 Sprite manager initialized");
}

void calculateSleepStageTiming(int timeout_minutes, unsigned long* stage1_ms, unsigned long* stage2_ms, unsigned long* stage3_ms) {
    unsigned long total_ms = (unsigned long)timeout_minutes * 60 * 1000;
    unsigned long min_stage2 = 5000;   
    unsigned long max_stage2 = 60000;  
    unsigned long min_stage3 = 3000;     
    unsigned long max_stage3 = 30000;  
    
    if (timeout_minutes <= 3) {
        *stage2_ms = max(min_stage2, min(max_stage2, (unsigned long)(total_ms * 0.25)));
        *stage3_ms = max(min_stage3, min(max_stage3, (unsigned long)(total_ms * 0.15)));
    } else if (timeout_minutes <= 10) {
        *stage2_ms = max(min_stage2, min(max_stage2, (unsigned long)(total_ms * 0.20)));
        *stage3_ms = max(min_stage3, min(max_stage3, (unsigned long)(total_ms * 0.10)));
    } else {
        *stage2_ms = max(min_stage2, min(max_stage2, (unsigned long)(total_ms * 0.15)));
        *stage3_ms = max(min_stage3, min(max_stage3, (unsigned long)(total_ms * 0.05)));
    }
    *stage1_ms = total_ms - *stage2_ms - *stage3_ms;
}

void sprite_manager_update(sprite_manager_t* manager, uint32_t current_time) {
    // --- CHECK MOUSE LOCK ---
    bool mouse_locked = (current_time < mouse_lock_timer);
    
    // Check for typing timeout
    if (manager->paw_animation_active && manager->last_typing_time > 0 && !mouse_locked) {
        if (current_time - manager->last_typing_time > TYPING_TIMEOUT_MS) {
            manager->paw_animation_active = false;
            manager->current_sprites[LAYER_EFFECTS] = NULL;
            // Force return to idle pose
            manager->current_sprites[LAYER_PAWS] = &twopawsup;
            Serial.println("🛑 Typing timeout");
        }
    }
    
    if (python_control_mode && current_time - last_command_time > PYTHON_TIMEOUT_MS) {
        python_control_mode = false;
        manager->idle_progression_enabled = true;
    }
    
    if (manager->idle_progression_enabled || !python_control_mode) {
        unsigned long s1, s2, s3;
        calculateSleepStageTiming(settings.sleep_timeout_minutes, &s1, &s2, &s3);
        if (manager->current_state == ANIM_STATE_IDLE_STAGE1 && current_time - manager->state_start_time > s1)
            sprite_manager_set_state(manager, ANIM_STATE_IDLE_STAGE2, current_time);
        else if (manager->current_state == ANIM_STATE_IDLE_STAGE2 && current_time - manager->state_start_time > s2)
            sprite_manager_set_state(manager, ANIM_STATE_IDLE_STAGE3, current_time);
        else if (manager->current_state == ANIM_STATE_IDLE_STAGE3 && current_time - manager->state_start_time > s3)
            sprite_manager_set_state(manager, ANIM_STATE_IDLE_STAGE4, current_time);
    }
    
    // --- PAW ANIMATION LOGIC ---
    if (!mouse_locked) {
        if (manager->paw_animation_active) {
            if (current_time - manager->paw_timer >= manager->animation_speed_ms) {
                manager->paw_frame = (manager->paw_frame + 1) % 4;
                switch (manager->paw_frame) {
                    case 0: 
                        manager->current_sprites[LAYER_PAWS] = &leftpawdown;
                        if (manager->current_state == ANIM_STATE_TYPING_FAST || manager->current_state == ANIM_STATE_TYPING_STREAK)
                            manager->current_sprites[LAYER_EFFECTS] = &left_click_effect;
                        else
                            manager->current_sprites[LAYER_EFFECTS] = NULL;
                        break;
                    case 1: 
                        manager->current_sprites[LAYER_PAWS] = &twopawsup;
                        manager->current_sprites[LAYER_EFFECTS] = NULL;
                        break;
                    case 2: 
                        manager->current_sprites[LAYER_PAWS] = &rightpawdown;
                        if (manager->current_state == ANIM_STATE_TYPING_FAST || manager->current_state == ANIM_STATE_TYPING_STREAK)
                            manager->current_sprites[LAYER_EFFECTS] = &right_click_effect;
                        else
                            manager->current_sprites[LAYER_EFFECTS] = NULL;
                        break;
                    case 3: 
                        manager->current_sprites[LAYER_PAWS] = &twopawsup;
                        manager->current_sprites[LAYER_EFFECTS] = NULL;
                        break;
                }
                manager->paw_timer = current_time;
            }
        } else {
            // Idle paw logic - Ensure hands are visible in Stage 1
            if (manager->current_state == ANIM_STATE_IDLE_STAGE1) manager->current_sprites[LAYER_PAWS] = &twopawsup;
            else if (manager->current_state >= ANIM_STATE_IDLE_STAGE2) manager->current_sprites[LAYER_PAWS] = NULL;
            
            if (manager->current_state != ANIM_STATE_IDLE_STAGE4) manager->current_sprites[LAYER_EFFECTS] = NULL;
        }
    } else {
        manager->paw_timer = current_time;
    }
    
    // Sleepy effects
    if (manager->current_state == ANIM_STATE_IDLE_STAGE4) {
        if (current_time - manager->effect_timer > 1000) {
            manager->effect_frame = (manager->effect_frame + 1) % 3;
            switch (manager->effect_frame) {
                case 0: manager->current_sprites[LAYER_EFFECTS] = &sleepy1; break;
                case 1: manager->current_sprites[LAYER_EFFECTS] = &sleepy2; break;
                case 2: manager->current_sprites[LAYER_EFFECTS] = &sleepy3; break;
            }
            manager->effect_timer = current_time;
        }
    }
    
    // Blinking
    bool can_blink = (manager->current_state != ANIM_STATE_IDLE_STAGE3 && manager->current_state != ANIM_STATE_IDLE_STAGE4);
    if (!manager->blinking && current_time >= manager->blink_timer && can_blink) {
        manager->blinking = true;
        manager->blink_start_time = current_time;
        manager->current_sprites[LAYER_FACE] = &blink_face;
    } else if (manager->blinking && current_time - manager->blink_start_time > 200) {
        manager->blinking = false;
        if (manager->current_state == ANIM_STATE_IDLE_STAGE3 || manager->current_state == ANIM_STATE_IDLE_STAGE4)
            manager->current_sprites[LAYER_FACE] = &sleepy_face;
        else if (manager->is_streak_mode && manager->paw_animation_active)
            manager->current_sprites[LAYER_FACE] = &happy_face;
        else
            manager->current_sprites[LAYER_FACE] = &stock_face;
            
        if (can_blink) manager->blink_timer = current_time + random(3000, 8000);
        else manager->blink_timer = current_time + random(5000, 10000);
    }
    
    // Ear twitch
    if (!manager->ear_twitching && current_time >= manager->ear_twitch_timer) {
        manager->ear_twitching = true;
        manager->ear_twitch_start_time = current_time;
        manager->current_sprites[LAYER_BODY] = &bodyeartwitch;
    } else if (manager->ear_twitching && current_time - manager->ear_twitch_start_time > 500) {
        manager->ear_twitching = false;
        manager->current_sprites[LAYER_BODY] = &standardbody1;
        manager->ear_twitch_timer = current_time + random(10000, 30000);
    }
}

void sprite_manager_set_state(sprite_manager_t* manager, animation_state_t new_state, uint32_t current_time) {
    // FIX: If the state is the same, just update timestamp and return. 
    // This PREVENTS the animation glitch/reset when multiple speed commands arrive quickly.
    if (manager->current_state == new_state) {
        if (new_state >= ANIM_STATE_TYPING_SLOW) manager->last_typing_time = current_time;
        return; 
    }

    manager->current_state = new_state;
    manager->state_start_time = current_time;
    if (new_state >= ANIM_STATE_TYPING_SLOW) manager->last_typing_time = current_time;
    
    manager->current_sprites[LAYER_BODY] = &standardbody1;
    manager->current_sprites[LAYER_TABLE] = &table1;
    
    switch (new_state) {
        case ANIM_STATE_IDLE_STAGE1:
            manager->current_sprites[LAYER_PAWS] = &twopawsup;
            manager->current_sprites[LAYER_EFFECTS] = NULL;
            manager->current_sprites[LAYER_FACE] = &stock_face;
            manager->paw_animation_active = false;
            break;
        case ANIM_STATE_IDLE_STAGE2:
            manager->current_sprites[LAYER_PAWS] = NULL;
            manager->current_sprites[LAYER_EFFECTS] = NULL;
            manager->current_sprites[LAYER_FACE] = &stock_face;
            manager->paw_animation_active = false;
            break;
        case ANIM_STATE_IDLE_STAGE3:
            manager->current_sprites[LAYER_PAWS] = NULL;
            manager->current_sprites[LAYER_EFFECTS] = NULL;
            manager->current_sprites[LAYER_FACE] = &sleepy_face;
            manager->paw_animation_active = false;
            break;
        case ANIM_STATE_IDLE_STAGE4:
            manager->current_sprites[LAYER_PAWS] = NULL;
            manager->current_sprites[LAYER_FACE] = &sleepy_face;
            manager->paw_animation_active = false;
            manager->effect_timer = current_time;
            break;
        case ANIM_STATE_TYPING_SLOW:
        case ANIM_STATE_TYPING_NORMAL:
            manager->current_sprites[LAYER_FACE] = manager->is_streak_mode ? &happy_face : &stock_face;
            manager->current_sprites[LAYER_EFFECTS] = NULL;
            manager->current_sprites[LAYER_PAWS] = &leftpawdown;
            manager->paw_animation_active = true;
            manager->paw_frame = 0;
            manager->paw_timer = current_time;
            break;
        case ANIM_STATE_TYPING_FAST:
        case ANIM_STATE_TYPING_STREAK:
            manager->current_sprites[LAYER_FACE] = manager->is_streak_mode ? &happy_face : &stock_face;
            manager->current_sprites[LAYER_PAWS] = &leftpawdown;
            manager->paw_animation_active = true;
            manager->paw_frame = 0;
            manager->paw_timer = current_time;
            break;
    }
    
    if (new_state >= ANIM_STATE_TYPING_SLOW) manager->idle_progression_enabled = false;
}

void sprite_render_layers(sprite_manager_t* manager, lv_obj_t* canvas, uint32_t current_time) {
    lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_TRANSP);
    lv_draw_img_dsc_t img_dsc;
    lv_draw_img_dsc_init(&img_dsc);
    for (int layer = 0; layer < NUM_LAYERS; layer++) {
        const lv_img_dsc_t* sprite = manager->current_sprites[layer];
        if (sprite) lv_canvas_draw_img(canvas, 0, 0, sprite, &img_dsc);
    }
}

const char* get_state_name(animation_state_t state) {
    switch (state) {
        case ANIM_STATE_IDLE_STAGE1: return "IDLE_STAGE1";
        case ANIM_STATE_IDLE_STAGE2: return "IDLE_STAGE2"; 
        case ANIM_STATE_IDLE_STAGE3: return "IDLE_STAGE3";
        case ANIM_STATE_IDLE_STAGE4: return "IDLE_STAGE4";
        case ANIM_STATE_TYPING_SLOW: return "TYPING_SLOW";
        case ANIM_STATE_TYPING_NORMAL: return "TYPING_NORMAL";
        case ANIM_STATE_TYPING_FAST: return "TYPING_FAST";
        default: return "UNKNOWN";
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("🐱 Bongo Cat with Sprites Starting...");
    EEPROM.begin(EEPROM_SIZE);
    loadSettings();
    randomSeed(analogRead(0));
    tft.init();
    tft.setRotation(0);
    tft.fillScreen(TFT_WHITE);
    pinMode(27, OUTPUT);
    digitalWrite(27, HIGH);
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, SCREEN_WIDTH * 10);
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_WIDTH;
    disp_drv.ver_res = SCREEN_HEIGHT;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);
    sprite_manager_init(&sprite_manager);
    createBongoCat();
    updateDisplayVisibility();
    Serial.println("✅ Bongo Cat Ready!");
}

void createBongoCat() {
    screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    cat_canvas = lv_canvas_create(screen);
    static lv_color_t canvas_buf[CAT_SIZE * CAT_SIZE];
    lv_canvas_set_buffer(cat_canvas, canvas_buf, CAT_SIZE, CAT_SIZE, LV_IMG_CF_TRUE_COLOR);
    lv_img_set_zoom(cat_canvas, 1024);
    lv_img_set_antialias(cat_canvas, false);
    lv_obj_align(cat_canvas, LV_ALIGN_CENTER, 12, 50);
    
    cpu_label = lv_label_create(screen);
    lv_label_set_text(cpu_label, "CPU: 0%");
    lv_obj_set_style_text_font(cpu_label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(cpu_label, lv_color_black(), 0);
    lv_obj_align(cpu_label, LV_ALIGN_TOP_LEFT, 5, 5);
    
    ram_label = lv_label_create(screen);
    lv_label_set_text(ram_label, "RAM: 0%");
    lv_obj_set_style_text_font(ram_label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(ram_label, lv_color_black(), 0);
    lv_obj_align(ram_label, LV_ALIGN_TOP_LEFT, 5, 25);
    
    wpm_label = lv_label_create(screen);
    lv_label_set_text(wpm_label, "WPM: 0");
    lv_obj_set_style_text_font(wpm_label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(wpm_label, lv_color_black(), 0);
    lv_obj_align(wpm_label, LV_ALIGN_TOP_LEFT, 5, 45);
    
    time_label = lv_label_create(screen);
    lv_label_set_text(time_label, "00:00");
    lv_obj_set_style_text_font(time_label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(time_label, lv_color_black(), 0);
    lv_obj_set_style_text_align(time_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(time_label, LV_ALIGN_TOP_RIGHT, -5, 5);
    
    sprite_render_layers(&sprite_manager, cat_canvas, millis());
}

void loop() {
    handleSerialCommands();
    uint32_t current_time = millis();
    static uint32_t last_animation_update = 0;
    
    if (current_time - last_animation_update >= 25) {
        sprite_manager_update(&sprite_manager, current_time);
        
        static uint32_t last_sprite_hash = 0;
        uint32_t current_sprite_hash = 0;
        for (int i = 0; i < NUM_LAYERS; i++) current_sprite_hash += (uint32_t)sprite_manager.current_sprites[i];
        
        if (current_sprite_hash != last_sprite_hash || (current_time - last_animation_update) > 200) {
            sprite_render_layers(&sprite_manager, cat_canvas, current_time);
            last_sprite_hash = current_sprite_hash;
        }
        last_animation_update = current_time;
    }
    
    static uint32_t last_time_update = 0;
    if (current_time - last_time_update >= 1000) {
        updateTimeDisplay();
        last_time_update = current_time;
    }
    
    static uint32_t last_lvgl_update = 0;
    if (current_time - last_lvgl_update >= 20) {
        lv_timer_handler();
        last_lvgl_update = current_time;
    }
    delay(2);
}