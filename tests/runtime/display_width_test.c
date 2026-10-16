#include "display_width.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    /* H40 frames present at their pixel width. */
    assert(display_width_for_frame(320) == 320);

    /* H32 frames fill the same width as H40, with wider pixels, as on a TV:
     * the VDP's pixel clock changes, the picture's width does not. */
    assert(display_width_for_frame(256) == 320);

    /* Widened canvases (custom-width video, widescreen margins) are already
     * sized for presentation and keep their width. */
    assert(display_width_for_frame(336) == 336);
    assert(display_width_for_frame(424) == 424);

    puts("display_width: ok");
    return 0;
}
