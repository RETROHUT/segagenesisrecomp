#ifndef GENESIS_DISPLAY_WIDTH_H
#define GENESIS_DISPLAY_WIDTH_H

/* Width (in logical presentation units) a frame of `frame_width` pixels
 * occupies on screen. H32 and H40 differ only in the VDP's pixel clock: on a
 * TV both fill the same width, so a 256-pixel H32 frame is presented across
 * the H40 width with wider pixels instead of keeping its 8:7 shape. Every
 * other width (H40, custom-width video, widescreen margins) is already sized
 * for presentation. */
static inline int display_width_for_frame(int frame_width)
{
    return frame_width == 256 ? 320 : frame_width;
}

#endif
