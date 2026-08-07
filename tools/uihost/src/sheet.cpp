#include "SharpLcd.h"
#include "VirtualPanel.h"
#include <stdio.h>
using namespace thicket;
static uint8_t fb[LCD_FB_BYTES];
int main(){
  VirtualPanel p; SharpLcd l(p, fb); l.fill_white();
  const char* g[] = {"─","│","┌","┐","└","┘","├","┤","┬","┴","┼",
                     "╭","╮","╯","╰","▀","▄","█","░","▒","▓",
                     "■","□","●","○","◆","◇","▲","▼","◀","▶",
                     "←","↑","→","↓","♡","♥","✓","✗","°","·","•","…"};
  int i=0;
  for(int row=0; row<6; ++row)
    for(int col=0; col<8 && i<43; ++col, ++i)
      l.draw_text((uint16_t)(8+col*48),(uint16_t)(10+row*30), g[i], true);
  l.draw_text(8, 200, "abcdefghijklmnopqrstuvwxyz 0123456789", true);
  l.draw_text(8, 216, "ABCDEFGHIJKLMNOPQRSTUVWXYZ !?@#.,", true);
  l.flush(); p.write_pbm("screens/glyphsheet.pbm", 2);
  printf("sheet written\n"); return 0;
}
