#include <SDL.h>
#include <stdlib.h>
#include "ui/window.h"
#include "core/assert.h"
#include "core/list.h"
#include "renderer/backend.h"
#include "ui/microprofile.h"
#include "ui/nuklear.h"

#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 480

static void win_destroy_joystick(struct window *win) {
  if (win->joystick) {
    SDL_JoystickClose(win->joystick);
    win->joystick = NULL;
  }
}

static void win_init_joystick(struct window *win) {
  win_destroy_joystick(win);

  // open the first connected joystick
  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
    win->joystick = SDL_JoystickOpen(i);

    if (win->joystick) {
      LOG_INFO("Opened joystick %s (%d)", SDL_JoystickName(win->joystick), i);
      break;
    }
  }

  // reset hat state
  memset(win->hat_state, 0, sizeof(win->hat_state));
}

static void win_handle_paint(struct window *win) {
  rb_begin_frame(win->rb);
  nk_begin_frame(win->nk);
  mp_begin_frame(win->mp);

  list_for_each_entry(listener, &win->listeners, struct window_listener, it) {
    if (listener->paint) {
      listener->paint(listener->data);
    }
  }

  if (win->debug_menu) {
    struct nk_context *ctx = &win->nk->ctx;
    struct nk_panel layout;
    struct nk_rect bounds = {0.0f, 0.0, 200.0f, 200.0f};

    if (nk_begin(ctx, &layout, "debug menu", bounds,
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                     NK_WINDOW_TITLE)) {
      list_for_each_entry(listener, &win->listeners, struct window_listener,
                          it) {
        if (listener->paint_debug_menu) {
          listener->paint_debug_menu(listener->data, ctx);
        }
      }
    }

    nk_end(ctx);
  }

  mp_end_frame(win->mp);
  nk_end_frame(win->nk);
  rb_end_frame(win->rb);
}

static void win_handle_keydown(struct window *win, enum keycode code,
                               int16_t value) {
  list_for_each_entry(listener, &win->listeners, struct window_listener, it) {
    if (listener->keydown) {
      listener->keydown(listener->data, code, value);
    }
  }
}

#define KEY_HAT_UP(hat) (enum keycode)(K_HAT0 + hat * 4 + 0)
#define KEY_HAT_RIGHT(hat) (enum keycode)(K_HAT0 + hat * 4 + 1)
#define KEY_HAT_DOWN(hat) (enum keycode)(K_HAT0 + hat * 4 + 2)
#define KEY_HAT_LEFT(hat) (enum keycode)(K_HAT0 + hat * 4 + 3)

static void win_handle_hatdown(struct window *win, int hat, uint8_t state,
                               int16_t value) {
  switch (state) {
    case SDL_HAT_UP:
      win_handle_keydown(win, KEY_HAT_UP(hat), value);
      break;
    case SDL_HAT_RIGHT:
      win_handle_keydown(win, KEY_HAT_RIGHT(hat), value);
      break;
    case SDL_HAT_DOWN:
      win_handle_keydown(win, KEY_HAT_DOWN(hat), value);
      break;
    case SDL_HAT_LEFT:
      win_handle_keydown(win, KEY_HAT_LEFT(hat), value);
      break;
    case SDL_HAT_RIGHTUP:
      win_handle_keydown(win, KEY_HAT_RIGHT(hat), value);
      win_handle_keydown(win, KEY_HAT_UP(hat), value);
      break;
    case SDL_HAT_RIGHTDOWN:
      win_handle_keydown(win, KEY_HAT_RIGHT(hat), value);
      win_handle_keydown(win, KEY_HAT_DOWN(hat), value);
      break;
    case SDL_HAT_LEFTUP:
      win_handle_keydown(win, KEY_HAT_LEFT(hat), value);
      win_handle_keydown(win, KEY_HAT_UP(hat), value);
      break;
    case SDL_HAT_LEFTDOWN:
      win_handle_keydown(win, KEY_HAT_LEFT(hat), value);
      win_handle_keydown(win, KEY_HAT_DOWN(hat), value);
      break;
    default:
      break;
  }
}

static void win_handle_textinput(struct window *win, const char *text) {
  list_for_each_entry(listener, &win->listeners, struct window_listener, it) {
    if (listener->textinput) {
      listener->textinput(listener->data, text);
    }
  }
}

static void win_handle_mousemove(struct window *win, int x, int y) {
  list_for_each_entry(listener, &win->listeners, struct window_listener, it) {
    if (listener->mousemove) {
      listener->mousemove(listener->data, x, y);
    }
  }
}

static void win_handle_close(struct window *win) {
  list_for_each_entry(listener, &win->listeners, struct window_listener, it) {
    if (listener->close) {
      listener->close(listener->data);
    }
  }
}

static enum keycode translate_sdl_key(SDL_Keysym keysym) {
  enum keycode out = K_UNKNOWN;

  if (keysym.sym >= SDLK_SPACE && keysym.sym <= SDLK_z) {
    // this range maps 1:1 with ASCII chars
    out = (enum keycode)keysym.sym;
  } else {
    switch (keysym.sym) {
      case SDLK_RETURN:
        out = K_RETURN;
        break;
      case SDLK_ESCAPE:
        out = K_ESCAPE;
        break;
      case SDLK_BACKSPACE:
        out = K_BACKSPACE;
        break;
      case SDLK_TAB:
        out = K_TAB;
        break;
      case SDLK_CAPSLOCK:
        out = K_CAPSLOCK;
        break;
      case SDLK_F1:
        out = K_F1;
        break;
      case SDLK_F2:
        out = K_F2;
        break;
      case SDLK_F3:
        out = K_F3;
        break;
      case SDLK_F4:
        out = K_F4;
        break;
      case SDLK_F5:
        out = K_F5;
        break;
      case SDLK_F6:
        out = K_F6;
        break;
      case SDLK_F7:
        out = K_F7;
        break;
      case SDLK_F8:
        out = K_F8;
        break;
      case SDLK_F9:
        out = K_F9;
        break;
      case SDLK_F10:
        out = K_F10;
        break;
      case SDLK_F11:
        out = K_F11;
        break;
      case SDLK_F12:
        out = K_F12;
        break;
      case SDLK_PRINTSCREEN:
        out = K_PRINTSCREEN;
        break;
      case SDLK_SCROLLLOCK:
        out = K_SCROLLLOCK;
        break;
      case SDLK_PAUSE:
        out = K_PAUSE;
        break;
      case SDLK_INSERT:
        out = K_INSERT;
        break;
      case SDLK_HOME:
        out = K_HOME;
        break;
      case SDLK_PAGEUP:
        out = K_PAGEUP;
        break;
      case SDLK_DELETE:
        out = K_DELETE;
        break;
      case SDLK_END:
        out = K_END;
        break;
      case SDLK_PAGEDOWN:
        out = K_PAGEDOWN;
        break;
      case SDLK_RIGHT:
        out = K_RIGHT;
        break;
      case SDLK_LEFT:
        out = K_LEFT;
        break;
      case SDLK_DOWN:
        out = K_DOWN;
        break;
      case SDLK_UP:
        out = K_UP;
        break;
      case SDLK_NUMLOCKCLEAR:
        out = K_NUMLOCKCLEAR;
        break;
      case SDLK_KP_DIVIDE:
        out = K_KP_DIVIDE;
        break;
      case SDLK_KP_MULTIPLY:
        out = K_KP_MULTIPLY;
        break;
      case SDLK_KP_MINUS:
        out = K_KP_MINUS;
        break;
      case SDLK_KP_PLUS:
        out = K_KP_PLUS;
        break;
      case SDLK_KP_ENTER:
        out = K_KP_ENTER;
        break;
      case SDLK_KP_1:
        out = K_KP_1;
        break;
      case SDLK_KP_2:
        out = K_KP_2;
        break;
      case SDLK_KP_3:
        out = K_KP_3;
        break;
      case SDLK_KP_4:
        out = K_KP_4;
        break;
      case SDLK_KP_5:
        out = K_KP_5;
        break;
      case SDLK_KP_6:
        out = K_KP_6;
        break;
      case SDLK_KP_7:
        out = K_KP_7;
        break;
      case SDLK_KP_8:
        out = K_KP_8;
        break;
      case SDLK_KP_9:
        out = K_KP_9;
        break;
      case SDLK_KP_0:
        out = K_KP_0;
        break;
      case SDLK_KP_PERIOD:
        out = K_KP_PERIOD;
        break;
      case SDLK_APPLICATION:
        out = K_APPLICATION;
        break;
      case SDLK_POWER:
        out = K_POWER;
        break;
      case SDLK_KP_EQUALS:
        out = K_KP_EQUALS;
        break;
      case SDLK_F13:
        out = K_F13;
        break;
      case SDLK_F14:
        out = K_F14;
        break;
      case SDLK_F15:
        out = K_F15;
        break;
      case SDLK_F16:
        out = K_F16;
        break;
      case SDLK_F17:
        out = K_F17;
        break;
      case SDLK_F18:
        out = K_F18;
        break;
      case SDLK_F19:
        out = K_F19;
        break;
      case SDLK_F20:
        out = K_F20;
        break;
      case SDLK_F21:
        out = K_F21;
        break;
      case SDLK_F22:
        out = K_F22;
        break;
      case SDLK_F23:
        out = K_F23;
        break;
      case SDLK_F24:
        out = K_F24;
        break;
      case SDLK_EXECUTE:
        out = K_EXECUTE;
        break;
      case SDLK_HELP:
        out = K_HELP;
        break;
      case SDLK_MENU:
        out = K_MENU;
        break;
      case SDLK_SELECT:
        out = K_SELECT;
        break;
      case SDLK_STOP:
        out = K_STOP;
        break;
      case SDLK_AGAIN:
        out = K_AGAIN;
        break;
      case SDLK_UNDO:
        out = K_UNDO;
        break;
      case SDLK_CUT:
        out = K_CUT;
        break;
      case SDLK_COPY:
        out = K_COPY;
        break;
      case SDLK_PASTE:
        out = K_PASTE;
        break;
      case SDLK_FIND:
        out = K_FIND;
        break;
      case SDLK_MUTE:
        out = K_MUTE;
        break;
      case SDLK_VOLUMEUP:
        out = K_VOLUMEUP;
        break;
      case SDLK_VOLUMEDOWN:
        out = K_VOLUMEDOWN;
        break;
      case SDLK_KP_COMMA:
        out = K_KP_COMMA;
        break;
      case SDLK_KP_EQUALSAS400:
        out = K_KP_EQUALSAS400;
        break;
      case SDLK_ALTERASE:
        out = K_ALTERASE;
        break;
      case SDLK_SYSREQ:
        out = K_SYSREQ;
        break;
      case SDLK_CANCEL:
        out = K_CANCEL;
        break;
      case SDLK_CLEAR:
        out = K_CLEAR;
        break;
      case SDLK_PRIOR:
        out = K_PRIOR;
        break;
      case SDLK_RETURN2:
        out = K_RETURN2;
        break;
      case SDLK_SEPARATOR:
        out = K_SEPARATOR;
        break;
      case SDLK_OUT:
        out = K_OUT;
        break;
      case SDLK_OPER:
        out = K_OPER;
        break;
      case SDLK_CLEARAGAIN:
        out = K_CLEARAGAIN;
        break;
      case SDLK_CRSEL:
        out = K_CRSEL;
        break;
      case SDLK_EXSEL:
        out = K_EXSEL;
        break;
      case SDLK_KP_00:
        out = K_KP_00;
        break;
      case SDLK_KP_000:
        out = K_KP_000;
        break;
      case SDLK_THOUSANDSSEPARATOR:
        out = K_THOUSANDSSEPARATOR;
        break;
      case SDLK_DECIMALSEPARATOR:
        out = K_DECIMALSEPARATOR;
        break;
      case SDLK_CURRENCYUNIT:
        out = K_CURRENCYUNIT;
        break;
      case SDLK_CURRENCYSUBUNIT:
        out = K_CURRENCYSUBUNIT;
        break;
      case SDLK_KP_LEFTPAREN:
        out = K_KP_LEFTPAREN;
        break;
      case SDLK_KP_RIGHTPAREN:
        out = K_KP_RIGHTPAREN;
        break;
      case SDLK_KP_LEFTBRACE:
        out = K_KP_LEFTBRACE;
        break;
      case SDLK_KP_RIGHTBRACE:
        out = K_KP_RIGHTBRACE;
        break;
      case SDLK_KP_TAB:
        out = K_KP_TAB;
        break;
      case SDLK_KP_BACKSPACE:
        out = K_KP_BACKSPACE;
        break;
      case SDLK_KP_A:
        out = K_KP_A;
        break;
      case SDLK_KP_B:
        out = K_KP_B;
        break;
      case SDLK_KP_C:
        out = K_KP_C;
        break;
      case SDLK_KP_D:
        out = K_KP_D;
        break;
      case SDLK_KP_E:
        out = K_KP_E;
        break;
      case SDLK_KP_F:
        out = K_KP_F;
        break;
      case SDLK_KP_XOR:
        out = K_KP_XOR;
        break;
      case SDLK_KP_POWER:
        out = K_KP_POWER;
        break;
      case SDLK_KP_PERCENT:
        out = K_KP_PERCENT;
        break;
      case SDLK_KP_LESS:
        out = K_KP_LESS;
        break;
      case SDLK_KP_GREATER:
        out = K_KP_GREATER;
        break;
      case SDLK_KP_AMPERSAND:
        out = K_KP_AMPERSAND;
        break;
      case SDLK_KP_DBLAMPERSAND:
        out = K_KP_DBLAMPERSAND;
        break;
      case SDLK_KP_VERTICALBAR:
        out = K_KP_VERTICALBAR;
        break;
      case SDLK_KP_DBLVERTICALBAR:
        out = K_KP_DBLVERTICALBAR;
        break;
      case SDLK_KP_COLON:
        out = K_KP_COLON;
        break;
      case SDLK_KP_HASH:
        out = K_KP_HASH;
        break;
      case SDLK_KP_SPACE:
        out = K_KP_SPACE;
        break;
      case SDLK_KP_AT:
        out = K_KP_AT;
        break;
      case SDLK_KP_EXCLAM:
        out = K_KP_EXCLAM;
        break;
      case SDLK_KP_MEMSTORE:
        out = K_KP_MEMSTORE;
        break;
      case SDLK_KP_MEMRECALL:
        out = K_KP_MEMRECALL;
        break;
      case SDLK_KP_MEMCLEAR:
        out = K_KP_MEMCLEAR;
        break;
      case SDLK_KP_MEMADD:
        out = K_KP_MEMADD;
        break;
      case SDLK_KP_MEMSUBTRACT:
        out = K_KP_MEMSUBTRACT;
        break;
      case SDLK_KP_MEMMULTIPLY:
        out = K_KP_MEMMULTIPLY;
        break;
      case SDLK_KP_MEMDIVIDE:
        out = K_KP_MEMDIVIDE;
        break;
      case SDLK_KP_PLUSMINUS:
        out = K_KP_PLUSMINUS;
        break;
      case SDLK_KP_CLEAR:
        out = K_KP_CLEAR;
        break;
      case SDLK_KP_CLEARENTRY:
        out = K_KP_CLEARENTRY;
        break;
      case SDLK_KP_BINARY:
        out = K_KP_BINARY;
        break;
      case SDLK_KP_OCTAL:
        out = K_KP_OCTAL;
        break;
      case SDLK_KP_DECIMAL:
        out = K_KP_DECIMAL;
        break;
      case SDLK_KP_HEXADECIMAL:
        out = K_KP_HEXADECIMAL;
        break;
      case SDLK_LCTRL:
        out = K_LCTRL;
        break;
      case SDLK_LSHIFT:
        out = K_LSHIFT;
        break;
      case SDLK_LALT:
        out = K_LALT;
        break;
      case SDLK_LGUI:
        out = K_LGUI;
        break;
      case SDLK_RCTRL:
        out = K_RCTRL;
        break;
      case SDLK_RSHIFT:
        out = K_RSHIFT;
        break;
      case SDLK_RALT:
        out = K_RALT;
        break;
      case SDLK_RGUI:
        out = K_RGUI;
        break;
      case SDLK_MODE:
        out = K_MODE;
        break;
      case SDLK_AUDIONEXT:
        out = K_AUDIONEXT;
        break;
      case SDLK_AUDIOPREV:
        out = K_AUDIOPREV;
        break;
      case SDLK_AUDIOSTOP:
        out = K_AUDIOSTOP;
        break;
      case SDLK_AUDIOPLAY:
        out = K_AUDIOPLAY;
        break;
      case SDLK_AUDIOMUTE:
        out = K_AUDIOMUTE;
        break;
      case SDLK_MEDIASELECT:
        out = K_MEDIASELECT;
        break;
      case SDLK_WWW:
        out = K_WWW;
        break;
      case SDLK_MAIL:
        out = K_MAIL;
        break;
      case SDLK_CALCULATOR:
        out = K_CALCULATOR;
        break;
      case SDLK_COMPUTER:
        out = K_COMPUTER;
        break;
      case SDLK_AC_SEARCH:
        out = K_AC_SEARCH;
        break;
      case SDLK_AC_HOME:
        out = K_AC_HOME;
        break;
      case SDLK_AC_BACK:
        out = K_AC_BACK;
        break;
      case SDLK_AC_FORWARD:
        out = K_AC_FORWARD;
        break;
      case SDLK_AC_STOP:
        out = K_AC_STOP;
        break;
      case SDLK_AC_REFRESH:
        out = K_AC_REFRESH;
        break;
      case SDLK_AC_BOOKMARKS:
        out = K_AC_BOOKMARKS;
        break;
      case SDLK_BRIGHTNESSDOWN:
        out = K_BRIGHTNESSDOWN;
        break;
      case SDLK_BRIGHTNESSUP:
        out = K_BRIGHTNESSUP;
        break;
      case SDLK_DISPLAYSWITCH:
        out = K_DISPLAYSWITCH;
        break;
      case SDLK_KBDILLUMTOGGLE:
        out = K_KBDILLUMTOGGLE;
        break;
      case SDLK_KBDILLUMDOWN:
        out = K_KBDILLUMDOWN;
        break;
      case SDLK_KBDILLUMUP:
        out = K_KBDILLUMUP;
        break;
      case SDLK_EJECT:
        out = K_EJECT;
        break;
      case SDLK_SLEEP:
        out = K_SLEEP;
        break;
    }
  }

  if (keysym.scancode == SDL_SCANCODE_GRAVE) {
    out = K_CONSOLE;
  }

  return out;
}

static void win_pump_sdl(struct window *win) {
  SDL_Event ev;

  while (SDL_PollEvent(&ev)) {
    switch (ev.type) {
      case SDL_KEYDOWN: {
        enum keycode keycode = translate_sdl_key(ev.key.keysym);

        if (keycode != K_UNKNOWN) {
          win_handle_keydown(win, keycode, 1);
        }
      } break;

      case SDL_KEYUP: {
        enum keycode keycode = translate_sdl_key(ev.key.keysym);

        if (keycode != K_UNKNOWN) {
          win_handle_keydown(win, keycode, 0);
        }
      } break;

      case SDL_TEXTINPUT: {
        win_handle_textinput(win, ev.text.text);
      } break;

      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP: {
        enum keycode keycode;

        switch (ev.button.button) {
          case SDL_BUTTON_LEFT:
            keycode = K_MOUSE1;
            break;
          case SDL_BUTTON_RIGHT:
            keycode = K_MOUSE2;
            break;
          case SDL_BUTTON_MIDDLE:
            keycode = K_MOUSE3;
            break;
          case SDL_BUTTON_X1:
            keycode = K_MOUSE4;
            break;
          case SDL_BUTTON_X2:
            keycode = K_MOUSE5;
            break;
          default:
            keycode = K_UNKNOWN;
            break;
        }

        if (keycode != K_UNKNOWN) {
          win_handle_keydown(win, keycode,
                             ev.type == SDL_MOUSEBUTTONDOWN ? 1 : 0);
        }
      } break;

      case SDL_MOUSEWHEEL:
        if (ev.wheel.y > 0) {
          win_handle_keydown(win, K_MWHEELUP, 1);
          win_handle_keydown(win, K_MWHEELUP, 0);
        } else {
          win_handle_keydown(win, K_MWHEELDOWN, 1);
          win_handle_keydown(win, K_MWHEELDOWN, 0);
        }
        break;

      case SDL_MOUSEMOTION:
        win_handle_mousemove(win, ev.motion.x, ev.motion.y);
        break;

      case SDL_JOYDEVICEADDED:
      case SDL_JOYDEVICEREMOVED:
        win_init_joystick(win);
        break;

      case SDL_JOYAXISMOTION:
        if (ev.jaxis.axis < NUM_JOYSTICK_AXES) {
          win_handle_keydown(win, (enum keycode)(K_AXIS0 + ev.jaxis.axis),
                             ev.jaxis.value);
        } else {
          LOG_WARNING("Joystick motion ignored, axis %d >= NUM_JOYSTICK_AXES",
                      ev.jaxis.axis);
        }
        break;

      case SDL_JOYHATMOTION:
        if (ev.jhat.hat < NUM_JOYSTICK_HATS) {
          uint8_t *state = &win->hat_state[ev.jhat.hat];

          if (ev.jhat.value != *state) {
            // old key is up
            win_handle_hatdown(win, ev.jhat.hat, *state, 0);

            // new key is down
            win_handle_hatdown(win, ev.jhat.hat, ev.jhat.value, 1);
          }

          *state = ev.jhat.value;
        } else {
          LOG_WARNING(
              "Joystick hat motion ignored, hat %d >= NUM_JOYSTICK_HATS",
              ev.jhat.hat);
        }
        break;

      case SDL_JOYBUTTONDOWN:
      case SDL_JOYBUTTONUP:
        if (ev.jbutton.button < NUM_JOYSTICK_KEYS) {
          win_handle_keydown(win, (enum keycode)(K_JOY1 + ev.jbutton.button),
                             ev.type == SDL_JOYBUTTONDOWN ? 1 : 0);
        } else {
          LOG_WARNING("Joystick button ignored, button %d >= NUM_JOYSTICK_KEYS",
                      ev.jbutton.button);
        }
        break;

      case SDL_WINDOWEVENT:
        switch (ev.window.event) {
          case SDL_WINDOWEVENT_RESIZED: {
            win->width = ev.window.data1;
            win->height = ev.window.data2;
          } break;
        }
        break;

      case SDL_QUIT:
        win_handle_close(win);
        break;
    }
  }
}

void win_enable_debug_menu(struct window *win, bool active) {
  win->debug_menu = active;
}

void win_enable_text_input(struct window *win, bool active) {
  win->text_input = active;

  if (win->text_input) {
    SDL_StartTextInput();
  } else {
    SDL_StopTextInput();
  }
}

void win_pump_events(struct window *win) {
  win_pump_sdl(win);

  // trigger a paint event after draining all other window-related events
  win_handle_paint(win);
}

void win_add_listener(struct window *win, struct window_listener *listener) {
  list_add(&win->listeners, &listener->it);
}

void win_remove_listener(struct window *win, struct window_listener *listener) {
  list_remove(&win->listeners, &listener->it);
}

struct window *win_create() {
  struct window *win = calloc(1, sizeof(struct window));

  win->width = DEFAULT_WIDTH;
  win->height = DEFAULT_HEIGHT;

  // initialize window
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) < 0) {
    LOG_WARNING("SDL initialization failed: %s", SDL_GetError());
    win_destroy(win);
    return NULL;
  }

  // setup native window
  win->handle = SDL_CreateWindow(
      "redream", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, win->width,
      win->height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if (!win->handle) {
    LOG_WARNING("Window creation failed: %s", SDL_GetError());
    win_destroy(win);
    return NULL;
  }

  // setup render context
  win->rb = rb_create(win);
  if (!win->rb) {
    LOG_WARNING("Render backend creation failed");
    win_destroy(win);
    return NULL;
  }

  // setup nuklear
  win->nk = nk_create(win);
  if (!win->nk) {
    LOG_WARNING("Nuklear creation failed");
    win_destroy(win);
    return NULL;
  }

  // setup microprofile
  win->mp = mp_create(win);
  if (!win->mp) {
    LOG_WARNING("MicroProfile creation failed");
    win_destroy(win);
    return NULL;
  }

  return win;
}

void win_destroy(struct window *win) {
  if (win->mp) {
    mp_destroy(win->mp);
  }

  if (win->nk) {
    nk_destroy(win->nk);
  }

  if (win->rb) {
    rb_destroy(win->rb);
  }

  if (win->handle) {
    SDL_DestroyWindow(win->handle);
  }

  win_destroy_joystick(win);

  SDL_Quit();

  free(win);
}
