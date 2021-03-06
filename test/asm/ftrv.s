test_ftrv:
  # REGISTER_IN xf0  0x3f800000
  # REGISTER_IN xf1  0x00000000
  # REGISTER_IN xf2  0x00000000
  # REGISTER_IN xf3  0x00000000
  # REGISTER_IN xf4  0x00000000
  # REGISTER_IN xf5  0x40000000
  # REGISTER_IN xf6  0x00000000
  # REGISTER_IN xf7  0x00000000
  # REGISTER_IN xf8  0x00000000
  # REGISTER_IN xf9  0x00000000
  # REGISTER_IN xf10 0x3f800000
  # REGISTER_IN xf11 0x00000000
  # REGISTER_IN xf12 0x00000000
  # REGISTER_IN xf13 0x00000000
  # REGISTER_IN xf14 0x00000000
  # REGISTER_IN xf15 0x3f800000
  # REGISTER_IN fr4  0x40000000
  # REGISTER_IN fr5  0x40800000
  # REGISTER_IN fr6  0x41000000
  # REGISTER_IN fr7  0x00000000
  # XF0 XF4 XF8  XF12     FR0     XF0 * FR0 + XF4 * FR1 + XF8  * FR2 + XF12 * FR3
  # XF1 XF5 XF9  XF13  *  FR1  =  XF1 * FR0 + XF5 * FR1 + XF9  * FR2 + XF13 * FR3
  # XF2 XF6 XF10 XF14     FR2     XF2 * FR0 + XF6 * FR1 + XF10 * FR2 + XF14 * FR3
  # XF3 XF7 XF11 XF15     FR3     XF3 * FR0 + XF7 * FR1 + XF11 * FR2 + XF15 * FR3
  ftrv xmtrx, fv4
  rts 
  nop
  # REGISTER_OUT fr4 0x40000000
  # REGISTER_OUT fr5 0x41000000
  # REGISTER_OUT fr6 0x41000000
  # REGISTER_OUT fr7 0x00000000
