#pragma once

namespace eidolon::companion {
// Shared 320x240 product layout. Content is clipped to its own viewport;
// neither notifications nor text can expand into the face or action row.
struct Rect { int x, y, w, h; };
struct Layout {
    Rect header, face, information, actions, detail;
    explicit constexpr Layout(int width, int height, bool actions_visible=true, bool information_visible=true)
        : header{12, 4, width-24, 28},
          face{0, 36, width, (information_visible ? height-(actions_visible?108:72) : height-(actions_visible?80:44))},
          information{12, height-(actions_visible?68:32), width-24, 24},
          actions{12, height-40, width-24, 36},
          detail{12, 40, width-24, height-116} {}
};
}
