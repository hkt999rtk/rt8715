#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "../carplay_ui_memory.h"

void RGB2YUV(uint8_t **rgb_buffer, uint8_t **yuv_buffer,
	     int bytes_per_pixel, int width, int height);

int main(void)
{
	/* Red, green, blue, white in a 2x2 image. */
	uint8_t rgb565[2*16*2] = { 0x00, 0xf8, 0xe0, 0x07 };
	uint8_t rgb888[2*16*3] = { 0, 0, 255, 0, 255, 0 };
	const uint8_t expected[] = { 82, 144, 41, 235, 90, 240 };
	uint8_t result565[sizeof(expected)] = {0};
	uint8_t result888[sizeof(expected)] = {0};
	uint8_t *source = rgb565;
	uint8_t *output = result565;
	rgb565[16*2] = 0x1f; rgb565[16*2+2] = 0xff;
	rgb565[16*2+3] = 0xff;
	rgb888[16*3] = 255;
	rgb888[16*3+3] = 255; rgb888[16*3+4] = 255;
	rgb888[16*3+5] = 255;

	RGB2YUV(&source, &output, 2, 2, 2);
	source = rgb888;
	output = result888;
	RGB2YUV(&source, &output, 3, 2, 2);
	if (memcmp(result565, expected, sizeof(expected)) != 0 ||
	    memcmp(result888, expected, sizeof(expected)) != 0) {
		fprintf(stderr, "RGB565/888 to I420 conversion mismatch\n");
		return 1;
	}

    const int sizes[][2]={{2,2},{4,2},{6,4},{10,6},{32,18},{640,480},{1088,608},{1918,34},{1920,1080},{1920,1088},{2560,1200},{2560,1440}};
    uint32_t random=1234567;
    for(int bpp=2;bpp<=3;++bpp) for(size_t t=0;t<sizeof(sizes)/sizeof(sizes[0]);++t) {
        int w=sizes[t][0],h=sizes[t][1]; size_t pixels=(size_t)w*h;
        size_t source_stride=(size_t)((w+15)&~15)*bpp;
        carbox_ui_layout l; int padded=!carbox_ui_layout_init(&l,w,h);
        size_t capacity=source_stride*(size_t)h;
        if(padded && l.total>capacity)capacity=l.total;
        uint8_t *source=malloc(source_stride*(size_t)h),*image=malloc(capacity),*expected=malloc(pixels*3/2);
        assert(source && image && expected);
        memset(source,0xa5,source_stride*(size_t)h);
        for(int row=0;row<h;++row) for(size_t col=0;col<(size_t)w*bpp;++col) {
            random^=random<<13;random^=random>>17;random^=random<<5;
            source[(size_t)row*source_stride+col]=random;
        }
        memcpy(image,source,source_stride*(size_t)h);
        RGB2YUV(&source,&expected,bpp,w,h);
        assert(!carbox_ui_inplace(image,bpp,w,h));
        assert(!memcmp(image,expected,pixels*3/2));
        if(padded) {
            carbox_ui_pad(image,&l,w,h);
            size_t start[3]={0,pixels,pixels*5/4};
            for(int plane=0;plane<3;++plane) {
                int rows=plane?h/2:h,cols=plane?w/2:w;
                for(int r=0;r<rows;++r)assert(!memcmp(image+l.offset[plane]+r*l.stride[plane],expected+start[plane]+(size_t)r*cols,cols));
            }
        }
        free(source);free(image);free(expected);
    }
    puts("PASS: RGB conversion, in-place permutation and padded planes");
    return 0;
}
