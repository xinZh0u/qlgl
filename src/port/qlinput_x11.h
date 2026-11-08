/**
 * @file    qlinput_x11.h
 * @brief   X11 事件 → QLInputEvent 的翻译（内部头文件，不安装）。
 *
 * X11 的显示后端和 GL 后端共用这份逻辑，避免两处各写一遍键码表。
 * 调用方必须先包含 <X11/Xlib.h>、<X11/keysym.h> 和 <qlgl/qlgl.h>。
 */
#ifndef QLGL_PORT_QLINPUT_X11_H
#define QLGL_PORT_QLINPUT_X11_H

static uint32_t ql_x11_key_from_keysym(KeySym sym)
{
    /* 可打印字符直接用 ASCII；X11 的 keysym 对 ASCII 区间就是 ASCII 值 */
    if (sym >= XK_a && sym <= XK_z)
        return (uint32_t)sym;
    if (sym >= XK_A && sym <= XK_Z)
        return (uint32_t)(sym - XK_A + 'a');
    if (sym >= XK_0 && sym <= XK_9)
        return (uint32_t)sym;
    if (sym >= XK_F1 && sym <= XK_F12)
        return (uint32_t)(QL_KEY_F1 + (sym - XK_F1));

    switch (sym) {
    case XK_BackSpace: return QL_KEY_BACKSPACE;
    case XK_Tab:       return QL_KEY_TAB;
    case XK_Return:
    case XK_KP_Enter:  return QL_KEY_ENTER;
    case XK_Escape:    return QL_KEY_ESCAPE;
    case XK_space:     return QL_KEY_SPACE;
    case XK_Delete:    return QL_KEY_DELETE;
    case XK_Left:      return QL_KEY_LEFT;
    case XK_Up:        return QL_KEY_UP;
    case XK_Right:     return QL_KEY_RIGHT;
    case XK_Down:      return QL_KEY_DOWN;
    case XK_Home:      return QL_KEY_HOME;
    case XK_End:       return QL_KEY_END;
    case XK_Page_Up:   return QL_KEY_PAGE_UP;
    case XK_Page_Down: return QL_KEY_PAGE_DOWN;
    case XK_Insert:    return QL_KEY_INSERT;
    default:           return QL_KEY_NONE;
    }
}

static uint8_t ql_x11_mods(unsigned int state)
{
    uint8_t mods = 0;
    if ((state & ShiftMask) != 0)   mods |= QL_MOD_SHIFT;
    if ((state & ControlMask) != 0) mods |= QL_MOD_CTRL;
    if ((state & Mod1Mask) != 0)    mods |= QL_MOD_ALT;
    if ((state & Mod4Mask) != 0)    mods |= QL_MOD_META;
    if ((state & LockMask) != 0)    mods |= QL_MOD_CAPS;
    return mods;
}

static uint8_t ql_x11_button_of(unsigned int detail, int *is_wheel, int32_t *wheel)
{
    *is_wheel = 0;
    *wheel = 0;
    switch (detail) {
    case Button1: return QL_BUTTON_LEFT;
    case Button2: return QL_BUTTON_MIDDLE;
    case Button3: return QL_BUTTON_RIGHT;
    case Button4: *is_wheel = 1; *wheel = -1; return 0;   /* 向上滚 = dy 负 */
    case Button5: *is_wheel = 1; *wheel = 1;  return 0;
    case 6:       *is_wheel = 1; *wheel = 0;  return 0;   /* 横向滚动 M1 忽略 */
    case 7:       *is_wheel = 1; *wheel = 0;  return 0;
    default:      return 0xFF;
    }
}

/* 把一个 X 事件翻译成输入事件；不认识的类型直接忽略 */
/* 事件坐标是**设备像素**；输入系统与 ui 树用的是 CSS px，这里统一换算 */
static inline int32_t ql_x11_css(ql_coord_t v, ql_coord_t scale_fx)
{
    return (scale_fx > 0 && scale_fx != QL_SCALE_ONE) ? ql_unscale_len(v, scale_fx) : v;
}

static void ql_x11_push_input_scaled(XEvent *ev, uint8_t *buttons, ql_coord_t scale_fx)
{
    int is_wheel;
    int32_t wheel;
    uint8_t button;

    switch (ev->type) {
    case MotionNotify: {
        int32_t x = ql_x11_css(ev->xmotion.x, scale_fx);
        int32_t y = ql_x11_css(ev->xmotion.y, scale_fx);
        if ((ev->xmotion.state & Button1Mask) != 0)   *buttons |= QL_BUTTON_BIT_LEFT;
        else                                         *buttons &= (uint8_t)~QL_BUTTON_BIT_LEFT;
        if ((ev->xmotion.state & Button2Mask) != 0)   *buttons |= QL_BUTTON_BIT_MIDDLE;
        else                                         *buttons &= (uint8_t)~QL_BUTTON_BIT_MIDDLE;
        if ((ev->xmotion.state & Button3Mask) != 0)   *buttons |= QL_BUTTON_BIT_RIGHT;
        else                                         *buttons &= (uint8_t)~QL_BUTTON_BIT_RIGHT;
        ql_input_push_move(x, y, *buttons);
        break;
    }

    case ButtonPress:
    case ButtonRelease: {
        int down = (ev->type == ButtonPress);
        int32_t x = ql_x11_css(ev->xbutton.x, scale_fx);
        int32_t y = ql_x11_css(ev->xbutton.y, scale_fx);
        button = ql_x11_button_of(ev->xbutton.button, &is_wheel, &wheel);
        if (is_wheel) {
            if (down && wheel != 0)
                ql_input_push_wheel(x, y, 0, wheel);
            break;
        }
        if (button == 0xFF)
            break;
        if (down)
            *buttons |= (uint8_t)(1u << button);
        else
            *buttons &= (uint8_t)~(1u << button);
        ql_input_push_button(x, y, button, down);
        break;
    }

    case EnterNotify:
        ql_input_push_move(ql_x11_css(ev->xcrossing.x, scale_fx),
                           ql_x11_css(ev->xcrossing.y, scale_fx), *buttons);
        break;

    case LeaveNotify:
        ql_input_push_leave();
        break;

    case KeyPress:
    case KeyRelease: {
        KeySym sym = XLookupKeysym(&ev->xkey, 0);
        uint32_t key = ql_x11_key_from_keysym(sym);
        if (key == QL_KEY_NONE)
            break;
        ql_input_push_key(key, ev->type == KeyPress, ql_x11_mods(ev->xkey.state));
        break;
    }

    default:
        break;
    }
}

#endif /* QLGL_PORT_QLINPUT_X11_H */
