#pragma once

namespace eidolon::companion {
// Shared 320x240 product layout. Content is clipped to its own viewport;
// neither notifications nor text can expand into the face or action row.
struct Rect { int x, y, w, h; };
struct Layout {
    Rect header, face, information, actions, detail;
    explicit constexpr Layout(int width, int height, bool actions_visible=true, bool information_visible=true, int information_height=52)
        : header{12, 4, width-24, 28},
          face{8, 36, width-16, (information_visible ? height-(actions_visible?94:58)-information_height : height-(actions_visible?80:44))},
          information{8, height-(actions_visible?48:12)-information_height, width-16, information_height},
          actions{12, height-40, width-24, 36},
          detail{12, 40, width-24, height-116} {}
};
}
