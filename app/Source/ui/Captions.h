#pragma once

#include "HardwareView.h"
#include "MappingText.h"

/* What each part of the drawn unit does under a configuration: its captions
 * and tooltips. A knob's caption is its turn, with its push after it. */
inline void setCaptions (HardwareView::State& s, const midi_control_config& cfg)
{
    using Part = HardwareView::Part;
    for (int i = (int) Part::screen + 1; i < (int) Part::count; ++i)
    {
        const auto p = (Part) i;
        const auto ids = HardwareView::idsOf (p);
        mapping::Text t;
        if (ids.encoder >= 0)
        {
            t = mapping::encoder (cfg, ids.encoder);
            const auto push = mapping::button (cfg, ids.button);
            if (push.assigned)
                t.detail << mapping::separator() << "push " << push.what;
        }
        else if (ids.analog >= 0)
            t = mapping::analog (cfg, ids.analog);
        else if (ids.button >= 0)
            t = mapping::button (cfg, ids.button);
        s.captions[(size_t) p] = { t.what, t.detail, t.assigned };
    }
}
