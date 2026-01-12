#!/usr/bin/env python3
"""
Bongo Cat Application - Main Entry Point
Fixed threading model - Engine ALWAYS runs on main thread for proper keyboard timing
"""

import sys
import signal
import argparse
import threading
import time
from config import ConfigManager
from engine import BongoCatEngine
from tray import BongoCatSystemTray

class BongoCatApplication:
    """Main Bongo Cat application with FIXED thread-safe GUI"""
    
    def __init__(self, start_minimized=False):
        """Initialize the application"""
        self.start_minimized = start_minimized
        self.config = None
        self.engine = None
        self.tray = None
        self.tk_root = None
        self.running = False
        
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)
    
    def signal_handler(self, sig, frame):
        print('\n🛑 Shutting down gracefully...')
        self.shutdown()
    
    def initialize_components(self):
        try:
            print("📂 Loading configuration...")
            self.config = ConfigManager()
            
            print("🔧 Initializing Bongo Cat Engine...")
            self.engine = BongoCatEngine(config_manager=self.config)
            
            print("📱 Setting up system tray...")
            self.tray = BongoCatSystemTray(
                config_manager=self.config,
                engine=self.engine,
                on_exit_callback=self.shutdown
            )
            
            self.engine.set_tray_reference(self.tray)
            self.config.add_change_callback(self.tray.on_config_change)
            return True
            
        except Exception as e:
            print(f"❌ Initialization error: {e}")
            return False
    
    def run(self):
        print("🐱 Bongo Cat Application v2.2 - MOUSE SUPPORT ENABLED")
        print("=" * 60)
        
        if not self.initialize_components():
            return 1
        
        self.running = True
        
        try:
            print("📱 Starting system tray with run_detached()...")
            self.tray.start_detached()
            
            print("🔄 Checking initial connection status...")
            
            if self.start_minimized:
                print("🔕 Running in background mode...")
            else:
                print("🖥️ Running in normal mode...")
            
            print("✅ System tray started")
            print("🎯 Starting animation engine (Keyboard + Mouse monitoring)...")
            
            # Start the engine which now includes Mouse Listener
            self.engine.start_monitoring()
                
        except KeyboardInterrupt:
            print("\n🛑 Interrupted by user")
        except Exception as e:
            print(f"❌ Runtime error: {e}")
            return 1
        finally:
            self.shutdown()
        
        return 0
    
    def shutdown(self):
        print("🛑 Shutting down components...")
        self.running = False
        
        if self.engine:
            self.engine.stop_monitoring()
        
        if self.tray:
            self.tray.stop()
        
        if hasattr(self, 'tk_root') and self.tk_root:
            try:
                self.tk_root.destroy()
            except:
                pass
        
        print("👋 Goodbye!")
        sys.exit(0)

def main():
    parser = argparse.ArgumentParser(description="Bongo Cat Typing Monitor")
    parser.add_argument("--minimized", action="store_true", help="Start minimized to system tray")
    parser.add_argument("--startup", action="store_true", help="Started automatically with Windows")
    args = parser.parse_args()
    
    start_minimized = args.minimized or args.startup
    app = BongoCatApplication(start_minimized=start_minimized)
    return app.run()

if __name__ == "__main__":
    sys.exit(main())