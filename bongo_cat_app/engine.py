#!/usr/bin/env python3
"""
Bongo Cat Monitoring Engine - Enhanced with Mouse Support
"""

import time
import serial
import serial.tools.list_ports
import threading
from collections import deque
from pynput import keyboard, mouse  # Added mouse support
import sys
import platform
import psutil
import datetime
from typing import Callable, Optional, Dict, Any

class BongoCatEngine:
    """Bongo Cat engine using proven original implementation with configuration support"""
    
    def __init__(self, config_manager=None):
        """Initialize with exact original parameters plus configuration support"""
        self.config = config_manager
        self.tray = None
        
        # Get connection settings from config or use defaults
        if self.config:
            conn_settings = self.config.get_connection_settings()
            self.port = conn_settings.get('com_port', 'AUTO')
            self.baudrate = conn_settings.get('baudrate', 115200)
            behavior_settings = self.config.get_behavior_settings()
            self.idle_timeout = behavior_settings.get('idle_timeout_seconds', 1.0)
            self.sleep_timeout = behavior_settings.get('sleep_timeout_minutes', 1) * 60
            print(f"⏰ Timeouts: Idle={self.idle_timeout}s, Sleep={self.sleep_timeout}s")
            
            if hasattr(self.config, 'add_change_callback'):
                self.config.add_change_callback(self._on_config_change)
        else:
            self.port = 'AUTO'
            self.baudrate = 115200
            self.idle_timeout = 1.0
            self.sleep_timeout = 60
            
        self.serial_conn = None
        self.running = False
        
        # Animation control
        self.last_sent_speed = -1
        self.last_sent_state = ""
        self.last_command_time = 0
        self.min_command_interval = 0.5
        
        # Keystroke detection
        self.keystroke_buffer = deque(maxlen=50)
        current_time = time.time()
        self.last_keystroke_time = current_time
        self.typing_active = False
        self.idle_start_time = current_time
        self.sleep_start_time = None
        
        # WPM calculation
        self.current_wpm = 0
        self.max_wpm = 200
        
        # State management
        self.current_state = "IDLE"
        self.state_change_time = time.time()
        
        # Timing settings
        self.last_stats_sent = 0
        self.last_time_sent = 0
        
        # Industry-standard WPM
        self.chars_per_word = 7.5
        self.min_animation_speed = 500
        self.max_animation_speed = 40
        
        # WPM thresholds
        self.slow_threshold = 20
        self.normal_threshold = 40
        self.fast_threshold = 65
        self.streak_threshold = 85
        
        self.last_streak_state = False
        
        # System monitoring
        self.cpu_percent = 0
        self.ram_percent = 0
        self.system_monitor_running = False
        self.system_monitor_thread = None
        
        # Idle management
        self.idle_progression_started = False
        
        # Thread synchronization
        self._data_lock = threading.Lock()
        self._serial_lock = threading.Lock()
        
        self.config_callbacks: Dict[str, Callable] = {}
        
        if self.config:
            self.config.add_change_callback(self._on_config_change)
    
    def set_tray_reference(self, tray):
        self.tray = tray
        print("🔗 Engine connected to system tray for status updates")
    
    def _on_config_change(self, key: str, value: Any):
        print(f"🔧 Engine: Config changed {key} = {value} (will apply on restart)")
        if key == "behavior.idle_timeout_seconds":
            self.idle_timeout = value
        elif key == "behavior.sleep_timeout_minutes":
            self.sleep_timeout = value * 60
            print(f"🔧 Sleep timeout updated: {value} minutes ({self.sleep_timeout}s)")
    
    def save_config_to_arduino(self):
        print("💾 Saving configuration to Arduino EEPROM...")
        self.send_command("SAVE_SETTINGS")
        print("✅ Configuration saved to Arduino EEPROM")

    def apply_all_config_to_arduino(self):
        if not self.config:
            return
        
        print("📤 Applying all configuration to Arduino...")
        
        display = self.config.get_display_settings()
        self.send_command(f"DISPLAY_CPU:{'ON' if display.get('show_cpu') else 'OFF'}")
        self.send_command(f"DISPLAY_RAM:{'ON' if display.get('show_ram') else 'OFF'}")
        self.send_command(f"DISPLAY_WPM:{'ON' if display.get('show_wpm') else 'OFF'}")
        self.send_command(f"DISPLAY_TIME:{'ON' if display.get('show_time') else 'OFF'}")
        self.send_command(f"TIME_FORMAT:{'24' if display.get('time_format_24h') else '12'}")
        
        behavior = self.config.get_behavior_settings()
        self.send_command(f"SLEEP_TIMEOUT:{behavior.get('sleep_timeout_minutes', 5)}")
        
        self.send_command("SAVE_SETTINGS")
        print("✅ Configuration applied to Arduino")

    def find_esp32_port(self):
        print("🔍 Scanning for ESP32...")
        ports = serial.tools.list_ports.comports()
        esp32_ports = []
        
        for port in ports:
            esp32_keywords = ['CP210', 'CH340', 'CH341', 'FT232', 'ESP32', 'Silicon Labs', 'QinHeng Electronics']
            description = str(port.description).upper()
            manufacturer = str(port.manufacturer).upper() if port.manufacturer else ""
            
            for keyword in esp32_keywords:
                if keyword.upper() in description or keyword.upper() in manufacturer:
                    esp32_ports.append(port)
                    print(f"🎯 Found potential ESP32: {port.device} - {port.description}")
                    break
        
        if not esp32_ports:
            print("❌ No ESP32 found automatically")
            return None
        
        selected_port = esp32_ports[0].device
        print(f"✅ Auto-selected: {selected_port}")
        return selected_port

    def connect_serial(self, retries=3):
        if self.port == 'AUTO' or not self.port:
            detected_port = self.find_esp32_port()
            if detected_port:
                self.port = detected_port
            else:
                print("❌ Could not auto-detect ESP32. Please specify port manually.")
                return False
        
        for attempt in range(retries):
            try:
                if attempt > 0:
                    print(f"🔄 Retry attempt {attempt + 1}/{retries}...")
                    time.sleep(2)
                
                print(f"🔌 Connecting to {self.port}...")
                self.serial_conn = serial.Serial(port=self.port, baudrate=self.baudrate, timeout=1)
                time.sleep(2)
                
                self.send_command("PING")
                time.sleep(0.1)
                
                print(f"✅ Connected to {self.port}")
                self.send_initial_sync()
                if self.tray:
                    self.tray.update_connection_status("connected")
                return True
                
            except Exception as e:
                print(f"❌ Connection failed: {e}")
                if attempt < retries - 1:
                    continue
                if self.tray:
                    self.tray.update_connection_status("error")
                return False
        
        if self.tray:
            self.tray.update_connection_status("error")
        return False
    
    def send_initial_sync(self):
        try:
            current_time_str = datetime.datetime.now().strftime("%H:%M")
            self.send_command(f"TIME:{current_time_str}")
            cpu, ram = self.get_system_stats()
            self.send_command(f"STATS:CPU:{cpu},RAM:{ram},WPM:0")
            self.last_stats_sent = time.time()
            self.last_time_sent = time.time()
        except Exception as e:
            print(f"⚠️ Initial sync error: {e}")
    
    def disconnect_serial(self):
        if self.serial_conn and self.serial_conn.is_open:
            self.send_command("STOP")
            time.sleep(0.1)
            self.serial_conn.close()
            print("📱 Disconnected from Bongo Cat")
            if self.tray:
                self.tray.update_connection_status("disconnected")
    
    def send_command(self, command):
        if self.serial_conn and self.serial_conn.is_open:
            try:
                self.serial_conn.write(f"{command}\n".encode())
            except Exception as e:
                print(f"⚠️ Command '{command}' failed: {e}")
    
    def start_system_monitor(self):
        if not self.system_monitor_running:
            self.system_monitor_running = True
            self.system_monitor_thread = threading.Thread(target=self._system_monitor_loop, daemon=True)
            self.system_monitor_thread.start()
            print("📊 System monitor thread started")
    
    def stop_system_monitor(self):
        self.system_monitor_running = False
        if self.system_monitor_thread:
            self.system_monitor_thread.join(timeout=2.0)
            print("📊 System monitor thread stopped")
    
    def _system_monitor_loop(self):
        while self.system_monitor_running:
            try:
                cpu_usage = psutil.cpu_percent(interval=1.0)
                memory = psutil.virtual_memory()
                ram_usage = memory.percent
                with self._data_lock:
                    self.cpu_percent = int(cpu_usage)
                    self.ram_percent = int(ram_usage)
            except Exception as e:
                print(f"⚠️ System monitor error: {e}")
                time.sleep(1.0)
    
    def get_system_stats(self):
        try:
            with self._data_lock:
                return self.cpu_percent, self.ram_percent
        except Exception as e:
            print(f"⚠️ System stats access error: {e}")
            return 0, 0
    
    def update_system_stats(self):
        current_time = time.time()
        if not hasattr(self, 'last_stats_sent'):
            self.last_stats_sent = 0
            self.last_time_sent = 0
        
        if current_time - self.last_stats_sent >= 2.0:
            try:
                cpu, ram = self.get_system_stats()
                wpm = int(self.current_wpm) if hasattr(self, 'current_wpm') else 0
                self.send_command(f"STATS:CPU:{cpu},RAM:{ram},WPM:{wpm}")
                self.last_stats_sent = current_time
            except Exception as e:
                print(f"⚠️ Stats update error: {e}")
        
        if current_time - self.last_time_sent >= 30.0 or self.last_time_sent == 0:
            try:
                current_time_str = datetime.datetime.now().strftime("%H:%M")
                self.send_command(f"TIME:{current_time_str}")
                self.last_time_sent = current_time
            except Exception as e:
                print(f"⚠️ Time sync error: {e}")
    
    def calculate_wpm_industry_standard(self):
        now = time.time()
        if hasattr(self, '_last_wpm_calc_time') and (now - self._last_wpm_calc_time) < 0.25:
            return getattr(self, '_cached_wpm', 0)
        
        with self._data_lock:
            if len(self.keystroke_buffer) < 2:
                self._cached_wpm = 0
                self._last_wpm_calc_time = now
                return 0
            
            recent_keystrokes = list(self.keystroke_buffer)[-8:]
            if len(recent_keystrokes) < 2:
                self._cached_wpm = 0
                self._last_wpm_calc_time = now
                return 0
            
            time_span = now - recent_keystrokes[0]
            keystroke_count = len(recent_keystrokes)
        
        if time_span > 0.4:
            raw_wpm = (keystroke_count / self.chars_per_word) * (60 / time_span)
            if hasattr(self, '_cached_wpm') and self._cached_wpm > 0:
                smoothed_wpm = (self._cached_wpm * 0.6) + (raw_wpm * 0.4)
            else:
                smoothed_wpm = raw_wpm
            
            self._cached_wpm = min(smoothed_wpm, self.max_wpm)
            self._last_wpm_calc_time = now
            return self._cached_wpm
        
        self._cached_wpm = 0
        self._last_wpm_calc_time = now
        return 0
    
    def wpm_to_animation_speed(self, wpm):
        if wpm <= 0: return self.min_animation_speed
        wpm = min(wpm, self.max_wpm)
        normalized = wpm / self.max_wpm
        speed = self.min_animation_speed - (normalized * (self.min_animation_speed - self.max_animation_speed))
        return int(max(min(speed, 500), 30))
    
    def determine_animation_state(self, wpm):
        hysteresis = 2
        if self.current_state == "IDLE":
            if wpm >= 3:
                if wpm < self.slow_threshold: return "SLOW"
                elif wpm < self.normal_threshold: return "NORMAL"
                else: return "FAST"
        elif self.current_state == "SLOW":
            if wpm < 2: return "IDLE"
            elif wpm >= self.slow_threshold + hysteresis:
                if wpm < self.normal_threshold: return "NORMAL"
                else: return "FAST"
        elif self.current_state == "NORMAL":
            if wpm < self.slow_threshold - hysteresis: return "SLOW" if wpm >= 2 else "IDLE"
            elif wpm >= self.normal_threshold + hysteresis: return "FAST"
        elif self.current_state == "FAST":
            if wpm < self.normal_threshold - hysteresis:
                if wpm >= self.slow_threshold: return "NORMAL"
                else: return "SLOW" if wpm >= 2 else "IDLE"
        return self.current_state
    
    def is_streak_active(self, wpm):
        return wpm >= self.fast_threshold
    
    # --- MOUSE SUPPORT ADDED HERE ---
    def on_mouse_click(self, x, y, button, pressed):
        """Handle mouse clicks"""
        if pressed:
            try:
                command = None
                if button == mouse.Button.left:
                    command = "MOUSE:LEFT"
                elif button == mouse.Button.right:
                    command = "MOUSE:RIGHT"
                
                if command:
                    # Send directly to ensure low latency
                    self.send_command(command)
                    # Reset sleep timer
                    self.idle_start_time = time.time()
                    self.sleep_start_time = None
                    if self.idle_progression_started:
                        self.idle_progression_started = False
            except Exception as e:
                print(f"⚠️ Mouse click error: {e}")

    def on_key_press(self, key):
        """Lightweight keystroke detection"""
        try:
            current_time = time.time()
            with self._data_lock:
                self.keystroke_buffer.append(current_time)
                self.last_keystroke_time = current_time
                if not self.typing_active:
                    self.typing_active = True
                    self.sleep_start_time = None
                    if self.tray:
                        self.tray.update_typing_status(True, self.current_wpm)
        except Exception as e:
            print(f"❌ Keystroke detection error: {e}")
    
    def send_animation_command(self, wpm, force_update=False):
        current_time = time.time()
        if not force_update and not self.typing_active and (current_time - self.last_command_time) < self.min_command_interval:
            return
        
        new_state = self.determine_animation_state(wpm)
        animation_speed = self.wpm_to_animation_speed(wpm)
        is_streak = self.is_streak_active(wpm)
        
        if not hasattr(self, 'last_streak_state'):
            self.last_streak_state = False
        
        speed_changed = abs(animation_speed - self.last_sent_speed) > 25
        state_changed = new_state != self.last_sent_state
        streak_changed = is_streak != self.last_streak_state
        
        time_since_last_command = current_time - self.last_command_time
        force_periodic_update = time_since_last_command > (1.0 if self.typing_active else 4.0)
        
        if state_changed or speed_changed or streak_changed or force_update or force_periodic_update:
            self.last_sent_speed = animation_speed
            self.last_sent_state = new_state
            self.last_command_time = current_time
            if streak_changed:
                self.last_streak_state = is_streak
            
            if self.current_state != new_state:
                self.current_state = new_state
                self.state_change_time = current_time
            
            if self.serial_conn and self.serial_conn.is_open:
                try:
                    commands_to_send = []
                    if wpm <= 0:
                        commands_to_send.append("STOP")
                        if self.last_streak_state:
                            commands_to_send.append("STREAK_OFF")
                            self.last_streak_state = False
                    else:
                        commands_to_send.append(f"SPEED:{animation_speed}")
                        if streak_changed:
                            commands_to_send.append("STREAK_ON" if is_streak else "STREAK_OFF")
                    
                    if commands_to_send:
                        self.serial_conn.write(('\n'.join(commands_to_send) + '\n').encode())
                except Exception as e:
                    print(f"⚠️ Command send error: {e}")
    
    def update_animation(self):
        try:
            current_time = time.time()
            try:
                self.update_system_stats()
            except Exception as stats_error:
                print(f"⚠️ Stats update error: {stats_error}")
            
            with self._data_lock:
                last_keystroke_time = self.last_keystroke_time
                keystroke_buffer_copy = list(self.keystroke_buffer)
                typing_active = self.typing_active
            
            if current_time - last_keystroke_time > self.idle_timeout:
                if typing_active:
                    with self._data_lock:
                        self.typing_active = False
                        self.current_wpm = 0
                        self.idle_start_time = current_time
                        self.keystroke_buffer.clear()
                        if self.tray:
                            self.tray.update_typing_status(False, 0)
                    self.send_animation_command(0, force_update=True)
                
                time_idle = current_time - self.idle_start_time
                if time_idle >= self.sleep_timeout and self.sleep_start_time is None:
                    self.sleep_start_time = current_time
                    if self.serial_conn and self.serial_conn.is_open:
                        self.serial_conn.write(b"IDLE_START\n")
                return
            
            if keystroke_buffer_copy:
                new_wpm = self.calculate_wpm_industry_standard()
                if self.current_wpm == 0:
                    self.current_wpm = new_wpm
                else:
                    wpm_diff = abs(new_wpm - self.current_wpm)
                    smoothing = 0.7 if wpm_diff > 15 else (0.4 if wpm_diff > 5 else 0.2)
                    self.current_wpm = (self.current_wpm * (1 - smoothing)) + (new_wpm * smoothing)
                
                self.send_animation_command(self.current_wpm)
        except Exception as e:
            print(f"❌ Animation update error: {e}")
    
    def update_animation_loop(self):
        print("🎬 Animation thread started")
        while self.running:
            try:
                self.update_animation()
                time.sleep(0.08)
            except Exception:
                time.sleep(0.1)
    
    def start_monitoring(self):
        print("🚀 Bongo Cat Engine v3.1 - Enhanced Mouse Support")
        if not self.connect_serial():
            return False
        
        self.running = True
        self.start_system_monitor()
        
        if self.config:
            time.sleep(2.0)
            self.apply_all_config_to_arduino()
        
        update_thread = threading.Thread(target=self.update_animation_loop, daemon=True)
        update_thread.start()
        
        # Start Listeners
        try:
            # --- UPDATED: Start BOTH Keyboard and Mouse listeners ---
            m_listener = mouse.Listener(on_click=self.on_mouse_click)
            k_listener = keyboard.Listener(on_press=self.on_key_press)
            
            m_listener.start()
            k_listener.start()
            
            m_listener.join()
            k_listener.join()
            
        except KeyboardInterrupt:
            pass
        
        return True
    
    def stop_monitoring(self):
        print("\n🛑 Stopping Bongo Cat monitor...")
        self.running = False
        self.disconnect_serial()
        self.stop_system_monitor()