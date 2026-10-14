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
	uint8_t rgb565[] = { 0x00, 0xf8, 0xe0, 0x07,
			     0x1f, 0x00, 0xff, 0xff };
	uint8_t rgb888[] = { 0, 0, 255, 0, 255, 0,
			     255, 0, 0, 255, 255, 255 };
	const uint8_t expected[] = { 82, 144, 41, 235, 90, 240 };
	uint8_t result565[sizeof(expected)] = {0};
	uint8_t result888[sizeof(expected)] = {0};
	uint8_t *source = rgb565;
	uint8_t *output = result565;

	RGB2YUV(&source, &output, 2, 2, 2);
	source = rgb888;
	output = result888;
	RGB2YUV(&source, &output, 3, 2, 2);
	if (memcmp(result565, expected, sizeof(expected)) != 0 ||
	    memcmp(result888, expected, sizeof(expected)) != 0) {
		fprintf(stderr, "RGB565/888 to I420 conversion mismatch\n");
		return 1;
	}

    const int sizes[][2]={{2,2},{4,2},{6,4},{10,6},{32,18},{640,480},{1920,1088},{2560,1200},{2560,1440}};
    uint32_t random=1234567;
    for(int bpp=2;bpp<=3;++bpp) for(size_t t=0;t<sizeof(sizes)/sizeof(sizes[0]);++t) {
        int w=sizes[t][0],h=sizes[t][1]; size_t pixels=(size_t)w*h;
        carbox_ui_layout l; int padded=!carbox_ui_layout_init(&l,w,h);
        size_t capacity=pixels*bpp;
        if(padded && l.total>capacity)capacity=l.total;
        uint8_t *source=malloc(pixels*bpp),*image=malloc(capacity),*expected=malloc(pixels*3/2);
        assert(source && image && expected);
        for(size_t i=0;i<pixels*bpp;++i){random^=random<<13;random^=random>>17;random^=random<<5;source[i]=random;}
        memcpy(image,source,pixels*bpp);
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
