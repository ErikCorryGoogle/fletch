// Copyright (c) 2015, the Fletch project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

#include <stdarg.h>

extern "C" {
  #include "lcd_log.h"
}
#include "stm32746g_discovery_lcd.h"

#include "logger.h"

Logger* logger = NULL;

Logger::Logger() {
  // Initialize the LCD.
  BSP_LCD_Init();
  BSP_LCD_LayerDefaultInit(1, LCD_FB_START_ADDRESS);
  BSP_LCD_SelectLayer(1);
  BSP_LCD_SetFont(&LCD_DEFAULT_FONT);

  /* Initialize LCD Log module */
  LCD_LOG_Init();

  // Show Header and Footer texts.
  LCD_LOG_SetHeader((uint8_t *)"Fletch");
  LCD_LOG_SetFooter((uint8_t *)"STM32746G-DISCO board");
}

void Logger::vlog(Level level, char* format, va_list args) {
  switch (level) {
    case Logger::LDEBUG:
      LCD_LineColor = LCD_COLOR_CYAN;
      break;
  case Logger::LINFO:
      LCD_LineColor = LCD_COLOR_BLACK;
      break;
    case Logger::LWARNING:
      LCD_LineColor = LCD_COLOR_ORANGE;
      break;
    case Logger::LERROR:
      LCD_LineColor = LCD_COLOR_RED;
      break;
    case Logger::LFATAL:
      LCD_LineColor = LCD_COLOR_DARKRED;
      break;
  }
  vprintf(format, args);
  LCD_LineColor = LCD_COLOR_BLACK;
}

void Logger::log(Level level, char *format, ...) {
  va_list args;
  va_start(args, format);
  vlog(level, format, args);
  va_end(args);
}

void Logger::debug(char *format, ...) {
  va_list args;
  va_start(args, format);
  vlog(LDEBUG, format, args);
  va_end(args);
}

void Logger::info(char *format, ...) {
  va_list args;
  va_start(args, format);
  vlog(LINFO, format, args);
  va_end(args);
}

void Logger::warning(char *format, ...) {
  va_list args;
  va_start(args, format);
  vlog(LWARNING, format, args);
  va_end(args);
}

void Logger::error(char *format, ...) {
  va_list args;
  va_start(args, format);
  vlog(LERROR, format, args);
  va_end(args);
}

void Logger::fatal(char *format, ...) {
  va_list args;
  va_start(args, format);
  vlog(LFATAL, format, args);
  va_end(args);
}
