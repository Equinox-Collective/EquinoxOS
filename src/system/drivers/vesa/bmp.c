#include "bmp.h"
#include "vesa.h"
#include "../../mem/memory.h"
#include "../../../syslibc/string.h"

void draw_bmp(const uint8_t* data, int start_x, int start_y) {
    bmp_file_header_t* file_header = (bmp_file_header_t*)data;
    if (file_header->type != 0x4D42) return;

    bmp_info_header_t* info_header = (bmp_info_header_t*)(data + sizeof(bmp_file_header_t));
    uint8_t* pixel_data = (uint8_t*)(data + file_header->offset);
    
    int width = info_header->width;
    int height = info_header->height;
    int row_size = (width * 3 + 3) & ~3; 

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int pixel_y = height - 1 - y; 
            uint8_t* p = pixel_data + (pixel_y * row_size) + (x * 3);
            uint32_t color = (p[2] << 16) | (p[1] << 8) | p[0];
            put_pixel(start_x + x, start_y + y, color);
        }
    }
}