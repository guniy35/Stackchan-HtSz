#include "lvgl_image.h"
#include <cbin_font.h>

#include <esp_log.h>
#include <stdexcept>
#include <cstring>
#include <esp_heap_caps.h>

#define TAG "LvglImage"


LvglRawImage::LvglRawImage(void* data, size_t size) {
    bzero(&image_dsc_, sizeof(image_dsc_));
    image_dsc_.data_size = size;
    image_dsc_.data = static_cast<uint8_t*>(data);
    image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_dsc_.header.cf = LV_COLOR_FORMAT_RAW_ALPHA;
    image_dsc_.header.w = 0;
    image_dsc_.header.h = 0;
}

bool LvglRawImage::IsGif() const {
    auto ptr = (const uint8_t*)image_dsc_.data;
    return ptr[0] == 'G' && ptr[1] == 'I' && ptr[2] == 'F';
}

LvglCBinImage::LvglCBinImage(void* data) {
    image_dsc_ = cbin_img_dsc_create(static_cast<uint8_t*>(data));
}

LvglCBinImage::~LvglCBinImage() {
    if (image_dsc_ != nullptr) {
        cbin_img_dsc_delete(image_dsc_);
    }
}

LvglAllocatedImage::LvglAllocatedImage(void* data, size_t size) {
    bzero(&image_dsc_, sizeof(image_dsc_));
    image_dsc_.data_size = size;
    image_dsc_.data = static_cast<uint8_t*>(data);

    // [本地补丁] 原实现先 bzero 再调 lv_image_decoder_get_info：dsc 头被清零后
    // cf=LV_COLOR_FORMAT_UNKNOWN(0)，bin 解码器 info 直接判 INVALID——任何格式都
    // 抛 "Failed to get image info"（self.screen.preview_image 因此不可用）。
    // 改为：数据本身是 LVGL v9 bin 图时直接从头填充 header，跳过 get_info。
    if (size >= sizeof(lv_image_header_t)) {
        lv_image_header_t* h = static_cast<lv_image_header_t*>(data);
        if (h->magic == LV_IMAGE_HEADER_MAGIC && h->cf != LV_COLOR_FORMAT_UNKNOWN) {
            memcpy(&image_dsc_.header, h, sizeof(lv_image_header_t));
            return;
        }
    }

    if (lv_image_decoder_get_info(&image_dsc_, &image_dsc_.header) != LV_RESULT_OK) {
        ESP_LOGE(TAG, "Failed to get image info, data: %p size: %u", data, size);
        throw std::runtime_error("Failed to get image info");
    }
}

LvglAllocatedImage::LvglAllocatedImage(void* data, size_t size, int width, int height, int stride, int color_format) {
    bzero(&image_dsc_, sizeof(image_dsc_));
    image_dsc_.data_size = size;
    image_dsc_.data = static_cast<uint8_t*>(data);
    image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_dsc_.header.cf = color_format;
    image_dsc_.header.w = width;
    image_dsc_.header.h = height;
    image_dsc_.header.stride = stride;
}

LvglAllocatedImage::~LvglAllocatedImage() {
    if (image_dsc_.data) {
        heap_caps_free((void*)image_dsc_.data);
        image_dsc_.data = nullptr;
    }
}