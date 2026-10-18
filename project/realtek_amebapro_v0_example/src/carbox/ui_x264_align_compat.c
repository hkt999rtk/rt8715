/* Match the original Accessory UI stream dimensions.  Its surface and I420
 * converter use the macroblock-aligned width and height, including the edge
 * columns, rather than cropping to the negotiated visible dimensions. */
extern void lib_x264_init(int width, int height, int fps);

void carbox_ui_x264_init_aligned(int width, int height, int fps)
{
	lib_x264_init((width + 15) & ~15, (height + 15) & ~15, fps);
}
