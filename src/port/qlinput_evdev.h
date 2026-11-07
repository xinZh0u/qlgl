/**
 * @file    qlinput_evdev.h
 * @brief   Linux evdev 扫描码 → QLKey（内部头文件，不安装）。
 *
 * Wayland 的 wl_keyboard 给的是 evdev 键码，没有 xkbcommon 就只能用 US 布局
 * 的静态表。M2 接入 xkbcommon 后会替换这里，接口保持不变。
 */
#ifndef QLGL_PORT_QLINPUT_EVDEV_H
#define QLGL_PORT_QLINPUT_EVDEV_H

#include <linux/input-event-codes.h>

static uint32_t ql_evdev_key(uint32_t code)
{
    switch (code) {
    case KEY_ESC:        return QL_KEY_ESCAPE;
    case KEY_TAB:        return QL_KEY_TAB;
    case KEY_ENTER:      return QL_KEY_ENTER;
    case KEY_BACKSPACE:  return QL_KEY_BACKSPACE;
    case KEY_SPACE:      return QL_KEY_SPACE;
    case KEY_DELETE:     return QL_KEY_DELETE;
    case KEY_LEFT:       return QL_KEY_LEFT;
    case KEY_RIGHT:      return QL_KEY_RIGHT;
    case KEY_UP:         return QL_KEY_UP;
    case KEY_DOWN:       return QL_KEY_DOWN;
    case KEY_HOME:       return QL_KEY_HOME;
    case KEY_END:        return QL_KEY_END;
    case KEY_PAGEUP:     return QL_KEY_PAGE_UP;
    case KEY_PAGEDOWN:   return QL_KEY_PAGE_DOWN;
    case KEY_INSERT:     return QL_KEY_INSERT;

    case KEY_F1:  return QL_KEY_F1;
    case KEY_F2:  return QL_KEY_F2;
    case KEY_F3:  return QL_KEY_F3;
    case KEY_F4:  return QL_KEY_F4;
    case KEY_F5:  return QL_KEY_F5;
    case KEY_F6:  return QL_KEY_F6;
    case KEY_F7:  return QL_KEY_F7;
    case KEY_F8:  return QL_KEY_F8;
    case KEY_F9:  return QL_KEY_F9;
    case KEY_F10: return QL_KEY_F10;
    case KEY_F11: return QL_KEY_F11;
    case KEY_F12: return QL_KEY_F12;

    /* 字母：US 布局 */
    case KEY_A: return 'a'; case KEY_B: return 'b'; case KEY_C: return 'c';
    case KEY_D: return 'd'; case KEY_E: return 'e'; case KEY_F: return 'f';
    case KEY_G: return 'g'; case KEY_H: return 'h'; case KEY_I: return 'i';
    case KEY_J: return 'j'; case KEY_K: return 'k'; case KEY_L: return 'l';
    case KEY_M: return 'm'; case KEY_N: return 'n'; case KEY_O: return 'o';
    case KEY_P: return 'p'; case KEY_Q: return 'q'; case KEY_R: return 'r';
    case KEY_S: return 's'; case KEY_T: return 't'; case KEY_U: return 'u';
    case KEY_V: return 'v'; case KEY_W: return 'w'; case KEY_X: return 'x';
    case KEY_Y: return 'y'; case KEY_Z: return 'z';

    case KEY_1: return '1'; case KEY_2: return '2'; case KEY_3: return '3';
    case KEY_4: return '4'; case KEY_5: return '5'; case KEY_6: return '6';
    case KEY_7: return '7'; case KEY_8: return '8'; case KEY_9: return '9';
    case KEY_0: return '0';

    case KEY_MINUS:      return '-';
    case KEY_EQUAL:      return '=';
    case KEY_LEFTBRACE:  return '[';
    case KEY_RIGHTBRACE: return ']';
    case KEY_SEMICOLON:  return ';';
    case KEY_APOSTROPHE: return '\'';
    case KEY_GRAVE:      return '`';
    case KEY_BACKSLASH:  return '\\';
    case KEY_COMMA:      return ',';
    case KEY_DOT:        return '.';
    case KEY_SLASH:      return '/';

    default: return QL_KEY_NONE;
    }
}

static uint8_t ql_evdev_button(uint32_t code)
{
    switch (code) {
    case BTN_LEFT:   return QL_BUTTON_LEFT;
    case BTN_MIDDLE: return QL_BUTTON_MIDDLE;
    case BTN_RIGHT:  return QL_BUTTON_RIGHT;
    default:         return 0xFF;
    }
}

#endif /* QLGL_PORT_QLINPUT_EVDEV_H */
