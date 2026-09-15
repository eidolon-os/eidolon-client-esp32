#pragma once

namespace eidolon::companion {
// Shared 320x240 product layout. Content is clipped to its own viewport;
// neither notifications nor text can expand into the face or action row.
struct Rect { int x, y, w, h; };
struct Layout {
    Rect header, face, information, actions, detail;
    explicit constexpr Layout(int width, int height)
        : header{12, 4, width-24, 28},
          face{0, 36, width, height-108},
          information{12, height-68, width-24, 24},
          actions{12, height-40, width-24, 36},
          detail{12, 40, width-24, height-116} {}
};
}
