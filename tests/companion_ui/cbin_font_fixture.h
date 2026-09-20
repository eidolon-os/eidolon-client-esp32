#pragma once
#include <lvgl.h>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

// The shipped cbin embeds ESP32 32-bit descriptors. Convert only pointer-bearing
// descriptors for the 64-bit host, retaining the actual glyphs/metrics/bitmaps.
// Layout mirrors 78__xiaozhi-fonts/src/cbin_font.c, not a substitute test font.
class CbinFontFixture {
public:
    explicit CbinFontFixture(const char* path) {
        std::ifstream file(path,std::ios::binary);assert(file);
        bytes_={std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
        const size_t d=u32(24), maps=d+u32(d+8), kern=d+u32(d+12);
        const auto flags=u16(d+18);
        dsc_.kern_scale=u16(d+16);
        dsc_.cmap_num=flags&0x1ff;
        dsc_.bpp=(flags>>9)&15;
        dsc_.kern_classes=(flags>>13)&1;
        dsc_.bitmap_format=(flags>>14)&3;
        dsc_.stride=bytes_.at(d+20);
        dsc_.glyph_bitmap=at(d+u32(d));
        static_assert(sizeof(lv_font_fmt_txt_glyph_dsc_t)==16);
        dsc_.glyph_dsc=reinterpret_cast<const lv_font_fmt_txt_glyph_dsc_t*>(at(d+u32(d+4)));
        cmaps_.resize(dsc_.cmap_num);
        for (size_t i=0;i<cmaps_.size();++i) {
            const size_t m=maps+i*20;
            auto& cmap=cmaps_[i];
            cmap.range_start=u32(m);cmap.range_length=u16(m+4);cmap.glyph_id_start=u16(m+6);
            cmap.unicode_list=u32(m+8) ? reinterpret_cast<const uint16_t*>(at(maps+u32(m+8))) : nullptr;
            cmap.glyph_id_ofs_list=u32(m+12) ? at(maps+u32(m+12)) : nullptr;
            cmap.list_length=u16(m+16);cmap.type=static_cast<lv_font_fmt_txt_cmap_type_t>(bytes_.at(m+18));
        }
        dsc_.cmaps=cmaps_.data();
        if (u32(d+12)) {
            assert(dsc_.kern_classes); // The production Noto resource uses class kerning.
            kern_.class_pair_values=reinterpret_cast<const int8_t*>(at(kern+u32(kern)));
            kern_.left_class_mapping=at(kern+u32(kern+4));
            kern_.right_class_mapping=at(kern+u32(kern+8));
            kern_.left_class_cnt=bytes_.at(kern+12);kern_.right_class_cnt=bytes_.at(kern+13);
            dsc_.kern_dsc=&kern_;
        }
        font_.get_glyph_dsc=lv_font_get_glyph_dsc_fmt_txt;
        font_.get_glyph_bitmap=lv_font_get_bitmap_fmt_txt;
        font_.line_height=u32(12);font_.base_line=u32(16);
        font_.subpx=bytes_.at(20)&3;font_.kerning=(bytes_.at(20)>>2)&1;
        font_.static_bitmap=(bytes_.at(20)>>3)&1;
        font_.underline_position=static_cast<int8_t>(bytes_.at(21));
        font_.underline_thickness=bytes_.at(22);font_.dsc=&dsc_;
    }
    const lv_font_t* font() const { return &font_; }
private:
    const uint8_t* at(size_t n) const { assert(n<bytes_.size());return bytes_.data()+n; }
    uint16_t u16(size_t n) const { assert(n+2<=bytes_.size());return bytes_[n] | (uint16_t(bytes_[n+1])<<8); }
    uint32_t u32(size_t n) const { return u16(n) | (uint32_t(u16(n+2))<<16); }
    std::vector<uint8_t> bytes_;
    std::vector<lv_font_fmt_txt_cmap_t> cmaps_;
    lv_font_fmt_txt_kern_classes_t kern_{};
    lv_font_fmt_txt_dsc_t dsc_{};
    lv_font_t font_{};
};
